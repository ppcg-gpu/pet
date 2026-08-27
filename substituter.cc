/*
 * Copyright 2011      Leiden University. All rights reserved.
 * Copyright 2015      Sven Verdoolaege. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY LEIDEN UNIVERSITY ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL LEIDEN UNIVERSITY OR
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
 * Leiden University.
 */

#include <isl/id.h>
#include <isl/space.h>
#include <isl/aff.h>

#include "expr.h"
#include "expr_arg.h"
#include "substituter.h"

/* Add the substitution of "id" by "expr" to the list of substitutions.
 * "expr" should be either an access expression or the address of
 * an access expression.
 */
void pet_substituter::add_sub(__isl_take isl_id *id, __isl_take pet_expr *expr)
{
	subs[id] = expr;
}

extern "C" {
	static __isl_give pet_expr *substitute_access(__isl_take pet_expr *expr,
		void *user);
}

/* Perform the substitutions stored in "subs" on "expr" and return
 * the results.
 * In particular, perform the substitutions on each of the access
 * subexpressions in "expr".
 */
__isl_give pet_expr *pet_substituter::substitute(__isl_take pet_expr *expr)
{
	if (subs.size() == 0)
		return expr;
	expr = pet_expr_map_access(expr, &substitute_access, this);
	return expr;
}

/* Perform the substitutions stored in "subs" on "tree" and return
 * the results.
 * In particular, perform the substitutions on each of the access
 * expressions in "tree".
 */
__isl_give pet_tree *pet_substituter::substitute(__isl_take pet_tree *tree)
{
	if (subs.size() == 0)
		return tree;
	tree = pet_tree_map_access_expr(tree, &substitute_access, this);
	return tree;
}

/* Insert the arguments of "subs" into the front of the argument list of "expr".
 * That is, the first arguments of the result are equal to those of "subs".
 */
static __isl_give pet_expr *insert_arguments(__isl_take pet_expr *expr,
	__isl_keep pet_expr *subs)
{
	int i, n;

	n = pet_expr_get_n_arg(subs);
	for (i = 0; i < n; ++i) {
		pet_expr *arg;

		arg = pet_expr_get_arg(subs, i);
		expr = pet_expr_insert_arg(expr, i, arg);
	}

	return expr;
}

/* Adjust the domain of "mpa" to match "space".
 *
 * In particular, both the domain of "mpa" and space are of the form
 *
 *	[]
 *
 * if #args = 0 or
 *
 *	[[] -> [args]]
 *
 * if #args > 0
 *
 * The number of arguments in "space" is greater than or equal to
 * the number of those in the domain of "mpa".  In particular,
 * for the domain of "mpa", the number is n_orig; for "space",
 * the number is n_orig + n_extra.  The additional "n_extra"
 * arguments need to be added at the end.
 *
 * If n_extra = 0, then nothing needs to be done.
 * If n_orig = 0, then [[] -> [args]] -> [] is plugged into the domain of "mpa".
 * Otherwise, [[] -> [args,args']] -> [[] -> [args]] is plugged
 * into the domain of "mpa".
 */
static __isl_give isl_multi_pw_aff *align_domain(
	__isl_take isl_multi_pw_aff *mpa, __isl_take isl_space *space,
	int n_orig, int n_extra)
{
	isl_multi_aff *ma;

	if (n_extra == 0) {
		isl_space_free(space);
		return mpa;
	}
	space = isl_space_unwrap(space);
	if (n_orig == 0) {
		ma = isl_multi_aff_domain_map(space);
	} else {
		isl_multi_aff *ma1, *ma2;
		ma1 = isl_multi_aff_domain_map(isl_space_copy(space));
		ma2 = isl_multi_aff_range_map(space);
		ma2 = isl_multi_aff_drop_dims(ma2,
					    isl_dim_out, n_orig, n_extra);
		ma = isl_multi_aff_range_product(ma1, ma2);
	}

	mpa = isl_multi_pw_aff_pullback_multi_aff(mpa, ma);
	return mpa;
}

/* Does "expr" read the parameter itself rather than something it points at?
 *
 * A body that subscripts a pointer parameter has an access with at least
 * one dimension -- the subscript.  One that only passes the parameter on,
 * or assigns it, accesses the pointer variable, which is
 * zero-dimensional.
 */
static bool substituted_is_value(__isl_keep pet_expr *expr)
{
	isl_multi_pw_aff *index;
	isl_size dim;

	index = pet_expr_access_get_index(expr);
	dim = isl_multi_pw_aff_dim(index, isl_dim_out);
	isl_multi_pw_aff_free(index);

	return dim == 0;
}

/* Perform the substitutions that are stored in "substituter"
 * to the access expression "expr".
 *
 * If the identifier of the outer array accessed by "expr"
 * is not one of those that needs to be replaced, then nothing
 * needs to be done.
 *
 * Otherwise, check if an access expression or an address
 * of such an expression is being substituted in.
 *
 * Insert the arguments of the substitution into the argument
 * list of "expr" and adjust the index expression of the substitution
 * to match the complete list of arguments of "expr".
 * Finally, call pet_expr_access_patch to prefix "expr" with
 * the index expression of the substitution.
 */
static __isl_give pet_expr *substitute_access(__isl_take pet_expr *expr,
	void *user)
{
	pet_substituter *substituter = (pet_substituter *) user;
	int n, expr_n_arg;
	int is_addr = 0;
	isl_id *id;
	isl_multi_pw_aff *index;
	isl_space *space;
	pet_expr *subs = NULL;
	pet_expr *subs_expr;

	id = pet_expr_access_get_id(expr);
	if (substituter->subs.find(id) != substituter->subs.end())
		subs = substituter->subs[id];
	isl_id_free(id);
	if (!subs)
		return expr;

	subs_expr = pet_expr_copy(subs);
	if (pet_expr_is_address_of(subs_expr)) {
		subs_expr = pet_expr_arg(subs_expr, 0);
		is_addr = 1;
	}

	expr_n_arg = pet_expr_get_n_arg(expr);
	n = pet_expr_get_n_arg(subs_expr);
	expr = insert_arguments(expr, subs_expr);
	index = pet_expr_access_get_index(expr);
	space = isl_multi_pw_aff_get_domain_space(index);
	isl_multi_pw_aff_free(index);
	index = pet_expr_access_get_index(subs_expr);
	index = align_domain(index, space, n, expr_n_arg);

	/* A POINTER PARAMETER USED AS A VALUE IS NOT AN ACCESS TO PATCH.
	 *
	 * "is_addr" says the argument was "&x", so the body's access through
	 * the parameter is an offset into x and the two are combined by
	 * prefixing one with the other.  That assumes the body SUBSCRIPTS the
	 * parameter: patch_space takes the outermost dimension of the body's
	 * access to be the offset and appends the rest, so it needs one to
	 * take.
	 *
	 * A body that only reads the parameter has no dimension to give up.
	 * Its access is the pointer itself -- zero-dimensional -- and there
	 * is no array access to prefix, because the value substituted is an
	 * address rather than an element.  Measured on
	 * tests/ast_link/default_arg, where libstdc++ hands "&__val" to a
	 * body that passes "__ptr" on: "is_addr body_dims=0 body=__ptr",
	 * nine times, against body_dims=1 everywhere else.
	 *
	 * What the substitution means there is simply the argument: the
	 * body's "value of the parameter" IS "&x".  So the address expression
	 * takes the access's place, whole, rather than being folded into it.
	 */
	if (is_addr && substituted_is_value(expr)) {
		isl_multi_pw_aff_free(index);
		pet_expr_free(expr);
		expr = pet_expr_copy(subs);
		pet_expr_free(subs_expr);
		return expr;
	}

	expr = pet_expr_access_patch(expr, index, is_addr);

	/* THE COMPOSITION TRAVELS WITH THE SUBSTITUTION.
	 *
	 * The arena map is attached where the array's name is WRITTEN, and
	 * after inlining the body writes a parameter instead.  So a buffer
	 * the source touches both ways -- once directly, once by passing
	 * "&name[i]" into a helper -- came out split in two: the direct
	 * access composed onto the storage's representative, the inlined
	 * one stayed on the member, and the two halves were different
	 * arrays to isl.  No flow edge could join them, and the direct
	 * write died with no reader.
	 *
	 * Measured on the 402-node scop, on ffn_gate_1_141 (member 12 of 19
	 * in hca_k_all_3_350's storage, at offset 5120), which one statement
	 * writes by name and two helpers read through a parameter:
	 *
	 *   may_read   composed  2   (the two inlined bodies)
	 *   may_write  composed  1   (the inlined clamp's write)
	 *   plain_may_write      2   (those, plus the direct write)
	 *
	 * The direct write is in the plain view and missing from the
	 * composed one, because it alone had left for the representative.
	 * The substitution is exactly the moment the body's access learns
	 * which array it really touches, so it is where the map belongs.
	 */
	if (subs_expr->acc.arena)
		expr = pet_expr_access_set_arena(expr,
				isl_union_map_copy(subs_expr->acc.arena));

	pet_expr_free(subs_expr);

	return expr;
}

/* Free all elements in the substitutions.
 */
pet_substituter::~pet_substituter()
{
	std::map<isl_id *, pet_expr *>::iterator it;

	for (it = subs.begin(); it != subs.end(); ++it) {
		isl_id_free(it->first);
		pet_expr_free(it->second);
	}
}
