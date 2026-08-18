#include <memory>
#include <string>
#include <vector>

#include <clang_features.h>

#include <clang/AST/ASTImporter.h>
#include <clang/AST/ASTImportError.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/Frontend/TextDiagnosticPrinter.h>
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
	/* One for each unit that is read.
	 *
	 * A unit takes the engine it is read with and hands it its own
	 * source manager, so an engine shared between units ends up
	 * holding the last one's.  Everything clang then says about a
	 * declaration of any other unit is said against locations that
	 * manager does not have, and it does not merely say it wrongly:
	 * working out how serious a diagnostic is means looking its
	 * location up, and looking up a location from elsewhere asserts.
	 */
	std::vector<IntrusiveRefCntPtr<DiagnosticsEngine> > unit_diags;
	std::vector<std::string> refused;
	std::vector<std::string> refused_why;
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
	/* What the importer says about a declaration it would not take is
	 * the only account of why, so it is printed rather than dropped.
	 */
	linked->diags = new DiagnosticsEngine(ids, *linked->diag_opts,
				new TextDiagnosticPrinter(llvm::errs(),
							*linked->diag_opts));
}

#else

static void make_diagnostics(struct pet_linked_ast *linked)
{
	IntrusiveRefCntPtr<DiagnosticIDs> ids(new DiagnosticIDs());

	linked->diag_opts = new DiagnosticOptions();
	linked->diags = new DiagnosticsEngine(ids, linked->diag_opts,
				new TextDiagnosticPrinter(llvm::errs(),
							&*linked->diag_opts));
}

#endif

/* An engine of its own for the unit about to be read.
 */
static IntrusiveRefCntPtr<DiagnosticsEngine> unit_diagnostics(
	struct pet_linked_ast *linked)
{
	IntrusiveRefCntPtr<DiagnosticIDs> ids(new DiagnosticIDs());
	IntrusiveRefCntPtr<DiagnosticsEngine> diags;

#ifdef DIAGNOSTICSENGINE_TAKES_DIAGOPTIONS_REFERENCE
	diags = new DiagnosticsEngine(ids, *linked->diag_opts,
			new TextDiagnosticPrinter(llvm::errs(),
						*linked->diag_opts));
#else
	diags = new DiagnosticsEngine(ids, linked->diag_opts,
			new TextDiagnosticPrinter(llvm::errs(),
						&*linked->diag_opts));
#endif
	linked->unit_diags.push_back(diags);

	return diags;
}

#ifdef LOADFROMASTFILE_TAKES_VFS_FIRST

static std::unique_ptr<ASTUnit> read_unit(struct pet_linked_ast *linked,
	const char *file)
{
	FileSystemOptions fs_opts;
	HeaderSearchOptions hs_opts;

	return ASTUnit::LoadFromASTFile(file, RawPCHContainerReader(),
		ASTUnit::LoadEverything, llvm::vfs::getRealFileSystem(),
		linked->diag_opts, unit_diagnostics(linked), fs_opts, hs_opts);
}

#else

static std::unique_ptr<ASTUnit> read_unit(struct pet_linked_ast *linked,
	const char *file)
{
	FileSystemOptions fs_opts;
	std::shared_ptr<HeaderSearchOptions> hs_opts =
		std::make_shared<HeaderSearchOptions>();

	return ASTUnit::LoadFromASTFile(file, RawPCHContainerReader(),
		ASTUnit::LoadEverything, unit_diagnostics(linked), fs_opts,
		hs_opts);
}

#endif

/* An importer that links rather than merges.
 *
 * The importer clang provides is built for putting two translation
 * units side by side and asking whether what they say agrees.  Where a
 * declaration of some name is already in the target, it compares the
 * two structurally and refuses the import when they differ, which over
 * the C++ of a real project happens tens of thousands of times: the
 * declarations that differ are the templates of the standard library,
 * seen from units that included the same header, and what differs about
 * them is that one of the two is still being built when the comparison
 * is made.
 *
 * A linker does not ask that question.  Two declarations of one name
 * are one entity, and that is the whole of the rule; nothing about the
 * shape of either of them comes into it.
 *
 * The rule is applied only where the importer would otherwise refuse
 * over the name, and not before.  Everything it does when it does not
 * refuse is worth keeping, and the most important of it is that a
 * definition is preferred to a declaration of the same thing: a target
 * left holding the prototype where the source had the body is a link in
 * name only, and the calls it was meant to resolve go nowhere.
 *
 * The kind has to match as well, since a name may be a type in one
 * place and a function in another, and those are two entities and not
 * one.
 */
struct link_importer : public ASTImporter {
	link_importer(ASTContext &ToContext, FileManager &ToFileManager,
			ASTContext &FromContext, FileManager &FromFileManager,
			bool MinimalImport,
			std::shared_ptr<ASTImporterSharedState> SharedState,
			unsigned unit)
		: ASTImporter(ToContext, ToFileManager, FromContext,
				FromFileManager, MinimalImport, SharedState),
		  unit(unit) {}

protected:
	Expected<Decl *> ImportImpl(Decl *From) override {
		/* A declaration that only its own unit can name is that
		 * unit's own, and another unit is free to have one of the
		 * same name meaning something else: two files may each
		 * define a static function called clamp and mean different
		 * functions by it.  Brought into one unit they become one
		 * name used twice, which is not a program, so the one
		 * arriving is given a name of its own, which is what a
		 * linker does with a local symbol.
		 *
		 * The name is put on the declaration being imported rather
		 * than on the one that comes out, because the importer
		 * builds that one from this one and there is no moment in
		 * between.  It is put back afterwards, since the unit it
		 * came from is not ours to alter.
		 */
		NamedDecl *named = dyn_cast<NamedDecl>(From);
		DeclarationName saved;
		bool renamed = false;

		if (named && needs_own_name(named)) {
			saved = named->getDeclName();
			named->setDeclName(own_name(named));
			renamed = true;
		}

		Expected<Decl *> res = import_or_unify(From);

		if (renamed)
			named->setDeclName(saved);

		return res;
	}

private:
	unsigned unit;

	/* Does "named" have to be given a name of its own?
	 *
	 * Only what no other unit can name, and only where the target
	 * has the name already: the first unit to use a name keeps it.
	 */
	bool needs_own_name(NamedDecl *named) {
		if (!named->getIdentifier())
			return false;
		if (!named->getDeclContext()->isTranslationUnit())
			return false;
		if (named->hasExternalFormalLinkage())
			return false;
		if (named->getFormalLinkage() != Linkage::Internal)
			return false;

		DeclarationName name(&getToContext().Idents.get(
					named->getName()));

		return !getToContext().getTranslationUnitDecl()->lookup(
					name).empty();
	}

	/* A name no declaration of the target has.
	 */
	DeclarationName own_name(NamedDecl *named) {
		std::string base = named->getNameAsString();

		for (unsigned n = unit; ; ++n) {
			std::string tried = base + "__" + std::to_string(n);
			DeclarationName name(&getToContext().Idents.get(tried));

			if (getToContext().getTranslationUnitDecl()->lookup(
						name).empty())
				return DeclarationName(
					&getFromContext().Idents.get(tried));
		}
	}

	Expected<Decl *> import_or_unify(Decl *From) {
		auto res = ASTImporter::ImportImpl(From);
		if (res)
			return res;

		ASTImportError::ErrorKind why = refusal(res.takeError());

		if (why == ASTImportError::UnsupportedConstruct)
			return import_unsupported(From);
		if (why != ASTImportError::NameConflict)
			return llvm::make_error<ASTImportError>(
					ASTImportError::Unknown);

		Decl *found = find_specialization(From);
		if (!found)
			found = find_in_specialization(From);
		/* Taking two declarations of a name for one entity is what a
		 * linker does, and it is right for a program that links, but
		 * it is not decided here: two records of one name may be two
		 * types, and nothing about them says which it is.  So it is
		 * asked for rather than assumed.
		 */
		if (!found && getenv("PET_LINK_BY_NAME"))
			found = find_in_target(From);
		if (!found)
			return llvm::make_error<ASTImportError>(
					ASTImportError::NameConflict);

		return MapImported(From, found);
	}

	/* Import a declaration the importer has no case for.
	 *
	 * Assembly written at the outermost level of a file is one such:
	 * clang reads it, holds it in the AST and generates it, and the
	 * importer alone does not know what to do with it, which is a gap
	 * in the importer rather than a thing that cannot be carried
	 * over.  Every unit that includes <iostream> has one, so without
	 * this the standard library cannot be linked whole.
	 */
	Expected<Decl *> import_unsupported(Decl *From) {
		auto *asm_decl = dyn_cast<FileScopeAsmDecl>(From);

		if (!asm_decl)
			return llvm::make_error<ASTImportError>(
					ASTImportError::UnsupportedConstruct);

		auto str = Import(asm_decl->getAsmStringExpr());
		if (!str)
			return str.takeError();
		auto asm_loc = Import(asm_decl->getAsmLoc());
		if (!asm_loc)
			return asm_loc.takeError();
		auto rparen_loc = Import(asm_decl->getRParenLoc());
		if (!rparen_loc)
			return rparen_loc.takeError();

		FileScopeAsmDecl *to = FileScopeAsmDecl::Create(getToContext(),
				getToContext().getTranslationUnitDecl(),
				*str, *asm_loc, *rparen_loc);

		MapImported(From, to);
		to->getDeclContext()->addDeclInternal(to);

		return to;
	}

private:
	/* Why the importer would not take a declaration.  The error is
	 * consumed either way, since it is answered here rather than
	 * passed on.
	 */
	static ASTImportError::ErrorKind refusal(llvm::Error err) {
		ASTImportError::ErrorKind kind = ASTImportError::Unknown;

		llvm::handleAllErrors(std::move(err),
			[&](const ASTImportError &e) {
				kind = e.Error;
			},
			[](const llvm::ErrorInfoBase &) {});

		return kind;
	}

	/* The specialisation of the target that is the same type as
	 * "From", or NULL when there is none.
	 *
	 * A template is instantiated as it is used, and each unit
	 * instantiates only the members it reached for, so the same
	 * specialisation holds different members in different units:
	 * std::vector<int> is one type, but a unit that called size()
	 * and a unit that called begin() materialised different parts of
	 * it.  Compared member by member the two come out unequal, which
	 * is what the importer concludes and why it refuses; by the rules
	 * of the language they are the same type, and which members were
	 * materialised says nothing about that.
	 *
	 * What does say it is the template and the arguments, so those
	 * are what is compared.
	 */
	Decl *find_specialization(Decl *From) {
		auto *spec = dyn_cast<ClassTemplateSpecializationDecl>(From);
		if (!spec)
			return NULL;

		auto tmpl = Import(spec->getSpecializedTemplate());
		if (!tmpl) {
			llvm::consumeError(tmpl.takeError());
			return NULL;
		}

		/* The arguments are carried over one at a time, and only the
		 * kinds that can be: a type, which is imported, and a value,
		 * which is a number and needs nothing done to it beyond the
		 * type it is counted in.  An argument of any other kind ends
		 * the attempt, since a specialisation named with one that was
		 * not carried over is not the one being looked for.
		 */
		llvm::SmallVector<TemplateArgument, 4> args;
		for (const TemplateArgument &from : spec->getTemplateArgs().asArray()) {
			if (from.getKind() == TemplateArgument::Type) {
				auto type = Import(from.getAsType());
				if (!type) {
					llvm::consumeError(type.takeError());
					return NULL;
				}
				args.push_back(TemplateArgument(*type));
			} else if (from.getKind() == TemplateArgument::Integral) {
				auto type = Import(from.getIntegralType());
				if (!type) {
					llvm::consumeError(type.takeError());
					return NULL;
				}
				args.push_back(TemplateArgument(getToContext(),
						from.getAsIntegral(), *type));
			} else {
				return NULL;
			}
		}

		void *pos;
		auto *to_tmpl = dyn_cast<ClassTemplateDecl>(*tmpl);
		if (!to_tmpl)
			return NULL;

		return to_tmpl->findSpecialization(args, pos);
	}

	/* The member of the target's specialisation that "From" is, or
	 * NULL when there is none.
	 *
	 * What holds of a specialisation holds of what it holds: a member
	 * materialised in one unit and not in another is the same member
	 * of the same type, and the unit that did without it says nothing
	 * to the contrary.  So a declaration whose context is a
	 * specialisation is looked for by name in the same specialisation
	 * of the target.
	 */
	Decl *find_in_specialization(Decl *From) {
		auto *dc = dyn_cast<ClassTemplateSpecializationDecl>(
					From->getDeclContext());
		auto *nd = dyn_cast<NamedDecl>(From);

		if (!dc || !nd)
			return NULL;

		IdentifierInfo *id = nd->getIdentifier();
		if (!id)
			return NULL;

		Decl *to_dc = find_specialization(dc);
		if (!to_dc)
			return NULL;

		DeclarationName name(&getToContext().Idents.get(id->getName()));
		for (NamedDecl *cand : cast<DeclContext>(to_dc)->lookup(name))
			if (cand->getKind() == From->getKind())
				return cand;

		return NULL;
	}

	/* The declaration of the target that "From" names, or NULL when
	 * the target has none.
	 *
	 * The name is looked up without asking the importer for anything.
	 * Asking it to import the context or the name would have it do
	 * part of the work before the work is decided on, and it does not
	 * survive being entered that way.  So only a declaration written
	 * at the outermost level and named by a plain identifier is looked
	 * for, which is what one translation unit offers another anyway.
	 */
	Decl *find_in_target(Decl *From) {
		auto *nd = dyn_cast<NamedDecl>(From);

		if (!nd || !From->getDeclContext()->isTranslationUnit())
			return NULL;

		IdentifierInfo *id = nd->getIdentifier();
		if (!id)
			return NULL;

		DeclContext *to_dc = getToContext().getTranslationUnitDecl();
		DeclarationName name(&getToContext().Idents.get(id->getName()));

		for (NamedDecl *cand : to_dc->lookup(name))
			if (cand->getKind() == From->getKind())
				return cand;

		return NULL;
	}
};

/* Import every top level declaration of "src" into the linked context.
 */
static void link_unit(struct pet_linked_ast *linked, ASTUnit *src)
{
	ASTUnit *target = linked->units[0].get();
	link_importer importer(target->getASTContext(),
		target->getFileManager(), src->getASTContext(),
		src->getFileManager(), /*MinimalImport=*/false,
		linked->shared, linked->units.size());

	for (Decl *d : src->getASTContext().getTranslationUnitDecl()->decls()) {
		auto res = importer.Import(d);
		if (res)
			continue;
		/* Why it was refused is the only thing that says what to
		 * do about it, so it is kept rather than thrown away.
		 */
		std::string why = llvm::toString(res.takeError());
		auto *nd = dyn_cast<NamedDecl>(d);
		linked->refused.push_back(nd ? nd->getNameAsString() : "");
		linked->refused_why.push_back(std::string(d->getDeclKindName()) +
						" " + why);
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

const char *pet_linked_ast_refused_why(struct pet_linked_ast *linked, int i)
{
	return linked->refused_why[i].c_str();
}
