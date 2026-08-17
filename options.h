#include <isl/arg.h>
#include <isl/ctx.h>

#if defined(__cplusplus)
extern "C" {
#endif

struct pet_options {
	/* If autodetect is false, a scop delimited by pragmas is extracted,
	 * otherwise we take any scop that we can find.
	 */
	int	autodetect;
	int	detect_conditional_assignment;
	/* If encapsulate_dynamic_control is set, then any dynamic control
	 * in the input program will be encapsulated in macro statements.
	 * This means in particular that no statements with arguments
	 * will be created.
	 */
	int	encapsulate_dynamic_control;
	/* Support pencil builtins and pragmas */
	int	pencil;
	int	n_path;
	const char **paths;
	int	n_define;
	const char **defines;
	/* Where the compilation database of the project is, either the
	 * directory holding compile_commands.json or the file itself.
	 * When it is given, a file is parsed with the flags the project
	 * builds it with rather than with none.
	 */
	char	*compile_commands;

	unsigned signed_overflow;
};

ISL_ARG_CTX_DECL(pet_options, struct pet_options, pet_options_args)

#if defined(__cplusplus)
}
#endif
