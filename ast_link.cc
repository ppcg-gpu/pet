#include <memory>
#include <vector>

#include <clang/AST/ASTImporter.h>
#include <clang/AST/ASTImporterSharedState.h>
#include <clang/Frontend/ASTUnit.h>
#include <clang/Frontend/CompilerInstance.h>
#include <llvm/Support/VirtualFileSystem.h>

#include "ast_link.h"

using namespace clang;

/* The linked AST.
 *
 * "units" holds every translation unit that was read.  The first is the
 * one the others were imported into, so it owns the linked context, but
 * the rest have to be kept alive as well: the importer leaves the linked
 * declarations referring to source locations and identifiers that belong
 * to the unit they came from.
 *
 * "shared" is what makes this a link rather than a series of unrelated
 * imports.  It records what has already been brought in, so that a second
 * unit mentioning the same entity reuses the declaration instead of
 * creating another one.
 *
 * "n_refused" counts the declarations the importer would not accept.
 */
struct pet_linked_ast {
	std::vector<std::unique_ptr<ASTUnit> > units;
	std::shared_ptr<ASTImporterSharedState> shared;
	std::shared_ptr<DiagnosticOptions> diag_opts;
	IntrusiveRefCntPtr<DiagnosticsEngine> diags;
	int n_refused;

	pet_linked_ast() : n_refused(0) {}
};

/* Read the translation unit serialised in "file".
 */
static std::unique_ptr<ASTUnit> read_unit(struct pet_linked_ast *linked,
	const char *file)
{
	FileSystemOptions fs_opts;
	HeaderSearchOptions hs_opts;

	return ASTUnit::LoadFromASTFile(file, RawPCHContainerReader(),
		ASTUnit::LoadEverything, llvm::vfs::getRealFileSystem(),
		linked->diag_opts, linked->diags, fs_opts, hs_opts);
}

/* Import every top level declaration of "src" into the linked context.
 */
static void link_unit(struct pet_linked_ast *linked, ASTUnit *src)
{
	ASTUnit *target = linked->units[0].get();
	ASTImporter importer(target->getASTContext(),
		target->getFileManager(), src->getASTContext(),
		src->getFileManager(), /*MinimalImport=*/false,
		linked->shared);

	for (Decl *d : src->getASTContext().getTranslationUnitDecl()->decls()) {
		auto res = importer.Import(d);
		if (res)
			continue;
		llvm::consumeError(res.takeError());
		linked->n_refused++;
	}
}

/* Link the translation units serialised in the "n" files in "files".
 *
 * The first unit is read directly into the result; it is the context
 * everything else is imported into.
 */
struct pet_linked_ast *pet_ast_link(const char **files, int n)
{
	if (n < 1)
		return NULL;

	struct pet_linked_ast *linked = new pet_linked_ast();
	linked->diag_opts = std::make_shared<DiagnosticOptions>();
	linked->diags = CompilerInstance::createDiagnostics(
		*llvm::vfs::getRealFileSystem(), *linked->diag_opts);

	std::unique_ptr<ASTUnit> first = read_unit(linked, files[0]);
	if (!first) {
		delete linked;
		return NULL;
	}
	linked->units.push_back(std::move(first));
	linked->shared = std::make_shared<ASTImporterSharedState>(
		*linked->units[0]->getASTContext().getTranslationUnitDecl());

	for (int i = 1; i < n; ++i) {
		std::unique_ptr<ASTUnit> src = read_unit(linked, files[i]);
		if (!src) {
			pet_ast_link_free(linked);
			return NULL;
		}
		link_unit(linked, src.get());
		linked->units.push_back(std::move(src));
	}

	return linked;
}

void pet_ast_link_free(struct pet_linked_ast *linked)
{
	delete linked;
}

clang::ASTContext &pet_linked_ast_context(struct pet_linked_ast *linked)
{
	return linked->units[0]->getASTContext();
}

int pet_linked_ast_n_refused(struct pet_linked_ast *linked)
{
	return linked->n_refused;
}
