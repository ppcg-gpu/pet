#ifndef PET_AST_LINK_H
#define PET_AST_LINK_H

/* Linking of the ASTs of several translation units into a single AST.
 *
 * This is the counterpart of what a linker does to object files, at the
 * level of the abstract syntax tree.  Every translation unit is imported
 * into one context, so that:
 *
 *  - a function declared in one unit and defined in another becomes a
 *    single entity whose declarations share one redeclaration chain, and
 *    a call to it reaches the definition;
 *  - entities with internal linkage stay distinct even when several units
 *    give them the same name;
 *  - a type described by a common header becomes one type, rather than one
 *    per unit.
 *
 * A call whose callee is defined in none of the linked units simply stays
 * unresolved, exactly as an undefined symbol would.
 */

#if defined(__cplusplus)
extern "C" {
#endif

struct pet_linked_ast;

/* Link the translation units serialised in the "n" files in "files".
 * Returns NULL if any of them cannot be read or if the units cannot be
 * combined.
 */
struct pet_linked_ast *pet_ast_link(const char **files, int n);
void pet_ast_link_free(struct pet_linked_ast *linked);

/* The number of declarations that could not be imported.  Anything other
 * than zero means the result is not a faithful link: the declarations are
 * missing from it, and so is anything that referred to them.
 */
int pet_linked_ast_n_refused(struct pet_linked_ast *linked);

/* The name of the "i"th declaration that could not be imported, or the
 * empty string for one that has no name.
 */
const char *pet_linked_ast_refused(struct pet_linked_ast *linked, int i);

/* Why the "i"th declaration could not be imported, as the importer put
 * it.
 */
const char *pet_linked_ast_refused_why(struct pet_linked_ast *linked, int i);

#if defined(__cplusplus)
}

#include <clang/AST/ASTContext.h>
#include <clang/Lex/Preprocessor.h>

/* The context holding the linked AST.
 */
clang::ASTContext &pet_linked_ast_context(struct pet_linked_ast *linked);

/* The preprocessor of the unit everything was linked into.  Whatever
 * reads the linked AST needs it to make sense of source locations.
 */
clang::Preprocessor &pet_linked_ast_preprocessor(struct pet_linked_ast *linked);
#endif

#endif
