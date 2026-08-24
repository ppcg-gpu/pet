/* Link the ASTs of several translation units and report what came out.
 *
 * The report is what makes the link checkable: which calls reach a
 * definition, which entities stayed distinct, and whether every type
 * described by a shared header became a single type.  It mentions no file
 * names or paths, so it can be compared against a fixed expectation.
 */
#include <map>
#include <set>
#include <string>

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "clang.h"

#include "ast_link.h"

using namespace clang;

/* What the finishing of the link produces is announced to this and read
 * by nobody: the report below is made from the context the finishing
 * leaves behind, not from what it announces.  It lasts as long as the
 * program because the analysis the finishing sets up keeps hold of it.
 */
static ASTConsumer nothing_reads_it;

/* Names of the functions that are called and whether they reach a body.
 */
struct call_collector : RecursiveASTVisitor<call_collector> {
	std::map<std::string, bool> resolved;

	bool VisitCallExpr(CallExpr *call) {
		FunctionDecl *fd = pet_clang_direct_callee(call);
		if (!fd)
			return true;
		const FunctionDecl *def = NULL;
		std::string name = fd->getNameAsString();
		bool has = fd->hasBody(def);
		/* A name is only reported as resolved if every call to it
		 * reaches a body.
		 */
		if (resolved.count(name))
			resolved[name] = resolved[name] && has;
		else
			resolved[name] = has;
		return true;
	}
};

static void usage(const char *argv0)
{
	fprintf(stderr, "usage: %s [--verbose] <unit.ast>...\n", argv0);
}

int main(int argc, char **argv)
{
	int verbose = 0;
	int first = 1;

	if (argc > 1 && !strcmp(argv[1], "--verbose")) {
		verbose = 1;
		first = 2;
	}
	if (argc - first < 1) {
		usage(argv[0]);
		return 2;
	}

	struct pet_linked_ast *linked;
	linked = pet_ast_link((const char **) argv + first, argc - first);
	if (!linked) {
		fprintf(stderr, "%s: cannot link\n", argv[0]);
		return 1;
	}
	pet_linked_ast_finish(linked, nothing_reads_it);

	ASTContext &ctx = pet_linked_ast_context(linked);

	call_collector calls;
	calls.TraverseDecl(ctx.getTranslationUnitDecl());

	/* Every declaration of a record with a given name has to share one
	 * canonical type, or the units were not really linked.
	 */
	std::map<std::string, std::set<const void *> > records;
	/* Functions with internal linkage stay distinct per unit, so count
	 * how many of each name the linked AST holds.
	 */
	std::map<std::string, int> internal;
	for (Decl *d : ctx.getTranslationUnitDecl()->decls()) {
		if (auto *rd = dyn_cast<RecordDecl>(d)) {
			if (rd->getNameAsString().empty())
				continue;
			/* The canonical declaration, rather than the
			 * canonical type: what is being asked is
			 * whether the two units' structs became one
			 * entity, and that is what a declaration
			 * having one canonical form means.  Asking it
			 * of the type would mean naming a way of
			 * getting at the type, and clang has renamed
			 * that more than once.
			 */
			records[rd->getNameAsString()].insert(
				(const void *) rd->getCanonicalDecl());
		} else if (auto *fd = dyn_cast<FunctionDecl>(d)) {
			if (fd->getFormalLinkage() != Linkage::Internal)
				continue;
			if (!fd->doesThisDeclarationHaveABody())
				continue;
			internal[fd->getNameAsString()]++;
		}
	}

	int unresolved = 0;
	for (auto &c : calls.resolved)
		if (!c.second)
			++unresolved;
	int split = 0;
	for (auto &r : records)
		if (r.second.size() != 1)
			++split;

	if (verbose) {
		int n = pet_linked_ast_n_refused(linked);
		for (int i = 0; i < n; ++i)
			printf("refused %s\n",
				pet_linked_ast_refused(linked, i));
		for (auto &c : calls.resolved)
			printf("call %s %s\n", c.first.c_str(),
				c.second ? "resolved" : "unresolved");
		for (auto &r : records)
			printf("record %s %zu\n", r.first.c_str(),
				r.second.size());
		for (auto &i : internal)
			printf("internal %s %d\n", i.first.c_str(), i.second);
	}

	printf("units %d\n", argc - first);
	printf("refused %d\n", pet_linked_ast_n_refused(linked));
	printf("calls %zu resolved %zu unresolved %d\n",
		calls.resolved.size(), calls.resolved.size() - unresolved,
		unresolved);
	printf("records %zu split %d\n", records.size(), split);

	int bad = pet_linked_ast_n_refused(linked) != 0 || split != 0;
	pet_ast_link_free(linked);

	return bad ? 1 : 0;
}
