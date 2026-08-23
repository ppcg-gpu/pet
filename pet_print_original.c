/* Print the code every scop of a C source file was extracted from.
 *
 * pet_scop_print_original reads a region back from the file its
 * pet_loc names.  Nothing else in pet asks for that, so without this
 * the function goes untested: a scop that comes out of a linked AST
 * names whichever unit its function was written in, and one that comes
 * out of a single source file names that file, and both have to print.
 */
#include <stdio.h>
#include <stdlib.h>

#include <isl/arg.h>
#include <isl/ctx.h>
#include <isl/options.h>
#include <isl/printer.h>

#include <pet.h>

#include "options.h"

struct options {
	struct isl_options	*isl;
	struct pet_options	*pet;
	char			*input;
};

ISL_ARGS_START(struct options, options_args)
ISL_ARG_CHILD(struct options, isl, "isl", &isl_options_args, "isl options")
ISL_ARG_CHILD(struct options, pet, NULL, &pet_options_args, "pet options")
ISL_ARG_ARG(struct options, input, "input", NULL)
ISL_ARGS_END

ISL_ARG_DEF(options, struct options, options_args)

/* Print the original code of "scop" in place of the scop itself,
 * which is what a transformation that gave up would do.
 */
static __isl_give isl_printer *print_it(__isl_take isl_printer *p,
	struct pet_scop *scop, void *user)
{
	p = pet_scop_print_original(scop, p);
	pet_scop_free(scop);

	return p;
}

int main(int argc, char *argv[])
{
	isl_ctx *ctx;
	struct options *options;
	int r;

	options = options_new_with_defaults();
	ctx = isl_ctx_alloc_with_options(&options_args, options);
	argc = options_parse(options, argc, argv, ISL_ARG_ALL);

	r = pet_transform_C_source(ctx, options->input, stdout, &print_it, NULL);

	isl_ctx_free(ctx);

	return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
