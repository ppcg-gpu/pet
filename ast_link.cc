#include <memory>
#include <string>
#include <vector>

#include <clang_features.h>

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
 * "refused" names the declarations the importer would not accept, which
 * is how an incompatibility between two units shows up: a function
 * declared with different types, or a struct defined with different
 * fields, cannot become one entity.
 */
struct pet_linked_ast {
	std::vector<std::unique_ptr<ASTUnit> > units;
	std::shared_ptr<ASTImporterSharedState> shared;
	/* Held the way the version of clang in hand wants it: what used
	 * to be an object with a count of its own is now one the caller
	 * keeps.
	 */
#ifdef DIAGNOSTICSENGINE_TAKES_DIAGOPTIONS_REFERENCE
	std::shared_ptr<DiagnosticOptions> diag_opts;
#else
	IntrusiveRefCntPtr<DiagnosticOptions> diag_opts;
#endif
	IntrusiveRefCntPtr<DiagnosticsEngine> diags;
	std::vector<std::string> refused;
};

/* Read the translation unit serialised in "file", and make the
 * diagnostics engine that reading it needs.
 *
 * Both have moved between versions of clang.  Reading used to take the
 * diagnostics engine fourth and the header search options as a shared
 * pointer, and now takes the file system and the diagnostic options
 * first and the header search options by reference.  The engine used to
 * be handed options with a count of their own, and is now handed ones
 * the caller keeps.
 *
 * The engine is given a consumer that says nothing.  What the importer
 * refuses is reported by name through pet_linked_ast_refused, which is
 * what a caller acts on, while clang's own account of why would land on
 * the standard error of whatever is reading the trees.  A consumer is
 * not optional either: an engine without one crashes the first time it
 * has something to say.
 */
#ifdef DIAGNOSTICSENGINE_TAKES_DIAGOPTIONS_REFERENCE

static void make_diagnostics(struct pet_linked_ast *linked)
{
	IntrusiveRefCntPtr<DiagnosticIDs> ids(new DiagnosticIDs());

	linked->diag_opts = std::make_shared<DiagnosticOptions>();
	linked->diags = new DiagnosticsEngine(ids, *linked->diag_opts,
				new IgnoringDiagConsumer());
}

#else

static void make_diagnostics(struct pet_linked_ast *linked)
{
	IntrusiveRefCntPtr<DiagnosticIDs> ids(new DiagnosticIDs());

	linked->diag_opts = new DiagnosticOptions();
	linked->diags = new DiagnosticsEngine(ids, linked->diag_opts,
				new IgnoringDiagConsumer());
}

#endif

#ifdef LOADFROMASTFILE_TAKES_VFS_FIRST

static std::unique_ptr<ASTUnit> read_unit(struct pet_linked_ast *linked,
	const char *file)
{
	FileSystemOptions fs_opts;
	HeaderSearchOptions hs_opts;

	return ASTUnit::LoadFromASTFile(file, RawPCHContainerReader(),
		ASTUnit::LoadEverything, llvm::vfs::getRealFileSystem(),
		linked->diag_opts, linked->diags, fs_opts, hs_opts);
}

#else

static std::unique_ptr<ASTUnit> read_unit(struct pet_linked_ast *linked,
	const char *file)
{
	FileSystemOptions fs_opts;
	std::shared_ptr<HeaderSearchOptions> hs_opts =
		std::make_shared<HeaderSearchOptions>();

	return ASTUnit::LoadFromASTFile(file, RawPCHContainerReader(),
		ASTUnit::LoadEverything, linked->diags, fs_opts, hs_opts);
}

#endif

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
		auto *nd = dyn_cast<NamedDecl>(d);
		linked->refused.push_back(nd ? nd->getNameAsString() : "");
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
	make_diagnostics(linked);

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

clang::Preprocessor &pet_linked_ast_preprocessor(struct pet_linked_ast *linked)
{
	return linked->units[0]->getPreprocessor();
}

int pet_linked_ast_n_refused(struct pet_linked_ast *linked)
{
	return linked->refused.size();
}

const char *pet_linked_ast_refused(struct pet_linked_ast *linked, int i)
{
	return linked->refused[i].c_str();
}
