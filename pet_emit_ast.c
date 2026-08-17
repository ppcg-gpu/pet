/* Serialise the AST of a C source file the way pet reads it.
 *
 * The units that pet_ast_link links have to be read with the include
 * paths, the macros and the predefines pet itself uses, or they describe
 * a different program than the one pet would have seen.
 */
#include <stdio.h>
#include <stdlib.h>

#include <isl/ctx.h>
#include <pet.h>

struct options {
	struct pet_options *pet;
	char *input;
	char *output;
};

ISL_ARGS_START(struct options, options_args)
ISL_ARG_CHILD(struct options, pet, "pet", &pet_options_args, "pet options")
ISL_ARG_ARG(struct options, input, "input", NULL)
ISL_ARG_ARG(struct options, output, "output", NULL)
ISL_ARGS_END

ISL_ARG_DEF(options, struct options, options_args)

int main(int argc, char **argv)
{
	isl_ctx *ctx;
	struct options *options;
	int r;

	options = options_new_with_defaults();
	if (!options)
		return EXIT_FAILURE;
	ctx = isl_ctx_alloc_with_options(&options_args, options);
	argc = options_parse(options, argc, argv, ISL_ARG_ALL);

	r = pet_emit_ast(ctx, options->input, options->output);

	isl_ctx_free(ctx);

	return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
