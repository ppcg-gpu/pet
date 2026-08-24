/*
 * Copyright 2017      Sven Verdoolaege. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *    1. Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *    2. Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY SVEN VERDOOLAEGE ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL SVEN VERDOOLAEGE OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation
 * are those of the authors and should not be interpreted as
 * representing official policies, either expressed or implied, of
 * Sven Verdoolaege.
 */

#include "clang.h"
#include "expr_plus.h"
#include "id.h"
#include "inlined_calls.h"
#include "scan.h"
#include "tree.h"

#include "config.h"

using namespace std;
using namespace clang;

pet_inlined_calls::~pet_inlined_calls()
{
	vector<pet_tree *>::iterator it;
	map<Stmt *, isl_id *>::iterator it_id;

	for (it = inlined.begin(); it != inlined.end(); ++it)
		pet_tree_free(*it);
	for (it_id = call2id.begin(); it_id != call2id.end(); ++it_id)
		isl_id_free(it_id->second);
}

/* This method is called for each call expression "call"
 * in an expression statement.
 *
 * If the body of what is called can be reached, then add it to
 * this->calls.
 *
 * What is asked is whether the body is there, not whether the word
 * "inline" was written in front of it.  The word says what the author
 * meant the compiler to do about the cost of a call and says nothing
 * about what the call does, and a program written across many files
 * hardly ever says it: reading a linked AST, every body is there, and
 * a scop that stops at every call is a scop of one function.
 *
 * A function whose body is already being put in place is left alone --
 * see PetScan::already_inlining -- since putting it inside itself is
 * something that ends only when the stack does.
 */
bool pet_inlined_calls::VisitCallExpr(clang::CallExpr *call)
{
	FunctionDecl *named = pet_clang_direct_callee(call);
	FunctionDecl *fd = pet_clang_find_function_decl_with_body(named);

	/* A function whose accesses were written down separately is one
	 * whose body was written down not to be looked at: that is what
	 * saying pencil_access(f) about it means.  Putting the body in
	 * place of the call reads what the author said to read instead
	 * of, and what it says is usually something a scop cannot hold,
	 * which is why anyone writes the annotation.
	 */
	if (named && scan->get_summary_function(call) != named)
		return true;

	/* What a call was found to name, and whether a body came with it.
	 * Asked about one name through PET_CALL_TRACE, since a link of an
	 * engine makes millions of these.
	 */
	if (const char *want = getenv("PET_CALL_TRACE")) {
		std::string name = named ?
			named->getNameAsString() : std::string("<no name>");

		if (name.find(want) != std::string::npos) {
			int n = 0;

			if (named)
				n = std::distance(named->redecls_begin(),
						named->redecls_end());
			fprintf(stderr, "a call names %s, %s, a chain of %d, "
				"and a body %s\n", name.c_str(),
				named ? ((Decl *) named)->getDeclKindName() : "nothing",
				n, fd ? "was found" : "was not found");
		}
	}

	if (fd && !scan->already_inlining(fd))
		calls.push_back(call);

	return true;
}

/* Extract a pet_tree corresponding to the inlined function that
 * is called by "call" and store it in this->inlined.
 * If, moreover, the inlined function has a return value,
 * then create a corresponding variable and store it in this->call2id.
 *
 * While extracting the inlined function, take into account
 * the mapping from call expressions to return variables for
 * previously extracted inlined functions in order to handle
 * nested calls.
 *
 * The declaration used is the one the body belongs to, which is the one
 * VisitCallExpr weighed when it decided this call could be put in place
 * at all.  A call names whichever declaration was visible where it was
 * written, and that is a different object from the one carrying the
 * body whenever the two were written apart -- which, after a link, is
 * every call that crosses a unit.  The two declare parameters of the
 * same names and types and none of the same identity, so binding the
 * arguments to the named declaration's parameters binds them to
 * variables the body never mentions: the body goes on naming its own,
 * those names resolve against whatever the caller happens to have, and
 * a body whose parameter had to be renamed for clashing with the caller
 * reads the caller's variable instead of its argument.
 */
void pet_inlined_calls::add(CallExpr *call)
{
	FunctionDecl *fd = pet_clang_find_function_decl_with_body(
					pet_clang_direct_callee(call));
	QualType qt;
	isl_id *id = NULL;

	qt = fd->getReturnType();
	if (!qt->isVoidType()) {
		id = pet_id_ret_from_type(scan->ctx, scan->walk->n_ret++, qt);
		call2id[call] = isl_id_copy(id);
	}
	scan->call2id = &this->call2id;
	pet_tree *tree = scan->extract_inlined_call(call, fd, id);
	scan->call2id = NULL;
	isl_id_free(id);

	/* Nothing came back, so nothing is put in place of the call and
	 * the call stays what it was: one statement that asks nothing of
	 * what it calls.  The variable that was going to hold what it
	 * returns goes with it, or the call would be replaced by a read
	 * of something never written.
	 */
	if (!tree) {
		if (getenv("PET_INLINE_TRACE"))
			fprintf(stderr, "the call to %s stays a call\n",
				fd->getNameAsString().c_str());
		std::map<Stmt *, isl_id *>::iterator it = call2id.find(call);

		if (it != call2id.end()) {
			isl_id_free(it->second);
			call2id.erase(it);
		}
		return;
	}

	inlined.push_back(tree);
	done.insert(call);
}

/* Collect all the call expressions in "stmt" that need to be inlined,
 * the corresponding pet_tree objects and the variables that store
 * the return values.
 *
 * The call expressions are first collected outermost to innermost.
 * Then the corresponding inlined functions are extracted in reverse order
 * to ensure that a nested call is performed before an outer call.
 */
void pet_inlined_calls::collect(Stmt *stmt)
{
	int n;

	TraverseStmt(stmt);

	n = calls.size();
	for (int i = n - 1; i >= 0; --i)
		add(cast<CallExpr>(calls[i]));
}

/* Add the inlined call expressions to "tree", where "tree" corresponds
 * to the original expression statement containing the calls, but with
 * the calls replaced by accesses to the return variables in this->call2id.
 * In particular, construct a new block containing declarations
 * for the return variables in this->call2id, the inlined functions
 * from innermost to outermost and finally "tree" itself.
 */
__isl_give pet_tree *pet_inlined_calls::add_inlined(__isl_take pet_tree *tree)
{
	pet_tree *block;
	int n;
	std::vector<pet_tree *>::iterator it_in;
	std::map<Stmt *, isl_id *>::iterator it;

	if (inlined.empty())
		return tree;

	n = call2id.size() + inlined.size() + 1;

	block = pet_tree_new_block(scan->ctx, 1, n);
	for (it = call2id.begin(); it != call2id.end(); ++it) {
		pet_expr *expr;
		pet_tree *decl;

		expr = pet_expr_access_from_id(isl_id_copy(it->second),
						scan->ast_context);
		decl = pet_tree_new_decl(expr);
		block = pet_tree_block_add_child(block, decl);
	}
	for (it_in = inlined.begin(); it_in != inlined.end(); ++it_in)
		block = pet_tree_block_add_child(block, pet_tree_copy(*it_in));
	block = pet_tree_block_add_child(block, tree);

	return block;
}
