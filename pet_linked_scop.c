/* Extract a scop from a linked AST and print it.
 *
 * This is the whole point of linking: a loop whose body is a call to a
 * function written in another translation unit only becomes a scop worth
 * having once that function is part of the same AST.
 */
#include <stdio.h>
#include <stdlib.h>

#include <isl/arg.h>
#include <isl/ctx.h>
#include <isl/options.h>

#include <pet.h>

#include "ast_link.h"
#include "scop_yaml.h"

struct options {
	struct isl_options	*isl;
	struct pet_options	*pet;
	char			*function;
	int			map;
	int			foreach;
};

ISL_ARGS_START(struct options, options_args)
ISL_ARG_CHILD(struct options, isl, "isl", &isl_options_args, "isl options")
ISL_ARG_CHILD(struct options, pet, NULL, &pet_options_args, "pet options")
ISL_ARG_STR(struct options, function, 0, "function", "name", NULL,
	"only extract from the function with this name")
ISL_ARG_BOOL(struct options, map, 0, "map", 0,
	"go over every function and say where each scop ended, "
	"rather than extracting one")
ISL_ARG_BOOL(struct options, foreach, 0, "foreach", 0,
	"go over every function with pet_linked_ast_foreach_scop, "
	"emitting each scop found")
ISL_ARGS_END

ISL_ARG_DEF(options, struct options, options_args)

/* Callback for pet_linked_ast_foreach_scop: emit the scop and a separator. */
static int emit_scop(__isl_take pet_scop *scop, void *user)
{
	pet_scop_emit(stdout, scop);
	pet_scop_free(scop);
	printf("---\n");
	return 0;
}

int main(int argc, char **argv)
{
	isl_ctx *ctx;
	struct options *options;
	struct pet_linked_ast *linked;
	struct pet_scop *scop;
	char **units;
	int n, n_units;

	/* The units to link are however many are given, which the option
	 * parser has no way of describing, so they are separated from the
	 * options by hand: everything from the first argument that is not
	 * an option onwards is a unit.
	 */
	for (n = 1; n < argc; ++n)
		if (argv[n][0] != '-')
			break;
	units = argv + n;
	n_units = argc - n;
	argc = n;

	options = options_new_with_defaults();
	if (!options)
		return EXIT_FAILURE;
	ctx = isl_ctx_alloc_with_options(&options_args, options);
	argc = options_parse(options, argc, argv, ISL_ARG_ALL);

	if (n_units < 1) {
		fprintf(stderr, "%s: no units to link\n", argv[0]);
		isl_ctx_free(ctx);
		return EXIT_FAILURE;
	}

	linked = pet_ast_link((const char **) units, n_units);
	if (!linked) {
		fprintf(stderr, "%s: cannot link\n", argv[0]);
		isl_ctx_free(ctx);
		return EXIT_FAILURE;
	}
	if (pet_linked_ast_n_refused(linked) != 0) {
		int i, n = pet_linked_ast_n_refused(linked);

		fprintf(stderr, "%s: %d declaration(s) could not be linked\n",
			argv[0], n);
		for (i = 0; i < n; ++i)
			fprintf(stderr, "  %s: %s\n",
				pet_linked_ast_refused(linked, i),
				pet_linked_ast_refused_why(linked, i));
		/* In --map mode, refused declarations are not fatal: the map
		 * will report what it can find, and a CXXMethodDecl that
		 * cannot cross into a C-only RecordDecl is a normal outcome
		 * of mixed C and C++ AST linkage.
		 */
		if (!options->map) {
			pet_ast_link_free(linked);
			isl_ctx_free(ctx);
			return EXIT_FAILURE;
		}
	}

	/* Asked for the map, every function is gone over and nothing is
	 * extracted: the two are different questions and the answer to
	 * one is not the answer to the other.
	 */
	if (options->map) {
		int r = pet_linked_ast_map(ctx, linked, stdout);

		pet_ast_link_free(linked);
		isl_ctx_free(ctx);

		return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
	}

	if (options->foreach) {
		int r = pet_linked_ast_foreach_scop(ctx, linked, emit_scop, NULL);

		pet_ast_link_free(linked);
		isl_ctx_free(ctx);

		return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
	}

	scop = pet_scop_extract_from_linked_ast(ctx, linked, options->function);
	if (!scop) {
		fprintf(stderr, "%s: no scop found\n", argv[0]);
		pet_ast_link_free(linked);
		isl_ctx_free(ctx);
		return EXIT_FAILURE;
	}

	pet_scop_emit(stdout, scop);

	pet_scop_free(scop);
	pet_ast_link_free(linked);
	isl_ctx_free(ctx);

	return EXIT_SUCCESS;
}
