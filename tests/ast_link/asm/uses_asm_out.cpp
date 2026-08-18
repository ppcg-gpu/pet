/* Two units that each carry assembly written at the outermost level of
 * a file.
 *
 * clang reads such a declaration, holds it in the AST and generates it,
 * and the importer alone has no case for it: FileScopeAsm is not
 * mentioned in ASTImporter.cpp at all, so it falls to the refusal every
 * unhandled node falls to.  That is a gap in the importer rather than a
 * thing that cannot be carried over, and every unit that includes
 * <iostream> has one, so without it the standard library cannot be
 * linked whole.
 */
__asm__(".globl pet_ast_link_asm_out");

int out(int x)
{
	return x + 1;
}
