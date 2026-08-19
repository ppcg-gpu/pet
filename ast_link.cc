#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <clang_features.h>

#include <clang/AST/ASTImporter.h>
#include <clang/AST/ASTImportError.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclGroup.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/Sema/Sema.h>
#include <clang/Parse/ParseAST.h>
#include <clang/Parse/Parser.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Frontend/TextDiagnosticPrinter.h>
#include <clang/AST/ASTImporterSharedState.h>
#include <clang/Frontend/ASTUnit.h>
#include <clang/Serialization/ASTReader.h>
#include <clang/Frontend/CompilerInstance.h>
#include <llvm/Support/VirtualFileSystem.h>

#include "ast_link.h"
#include "link_equivalence.h"

using namespace clang;

/* The language a linkage specification names when it names C.
 *
 * Held the way the version of clang in hand names it: what used to be
 * a constant of the declaration itself is now an enumeration of its
 * own.
 */
#ifdef HAS_LINKAGESPECLANGUAGEIDS
static LinkageSpecLanguageIDs c_language(void)
{
	return LinkageSpecLanguageIDs::C;
}
#else
static LinkageSpecDecl::LanguageIDs c_language(void)
{
	return LinkageSpecDecl::lang_c;
}
#endif

/* The type that "tag" declares.
 *
 * Held the way the version of clang in hand names it: what used to be
 * asked for with getTagDeclType is now asked for with
 * getCanonicalTagType, which answers with the canonical type.  Either
 * answer serves, since what is done with it is a comparison that looks
 * through however a type was written.
 */
#ifdef ASTCONTEXT_HAS_CANONICAL_TAG_TYPE
static QualType tag_type(ASTContext &ctx, const TagDecl *tag)
{
	return ctx.getCanonicalTagType(tag);
}
#else
static QualType tag_type(ASTContext &ctx, const TagDecl *tag)
{
	return ctx.getTagDeclType(tag);
}
#endif

/* The type that "d" names, or a null type when it names none.
 *
 * A tag declaration names the type it declares, a typedef names what it
 * stands for, and a function or a variable names the type it has.  What
 * each of them is, is that type: two declarations of one name with that
 * type equal are one entity, and with it unequal are two.  Nothing else
 * here names a type.
 */
static QualType type_named(ASTContext &ctx, const NamedDecl *d)
{
	if (auto *tag = dyn_cast<TagDecl>(d))
		return tag_type(ctx, tag);
	if (auto *td = dyn_cast<TypedefNameDecl>(d))
		return td->getUnderlyingType();
	if (isa<FunctionDecl>(d) || isa<VarDecl>(d))
		return cast<ValueDecl>(d)->getType();
	/* A constant of an enumeration is named by the enumeration it
	 * belongs to and not by its own type, because its own type is
	 * not the same question in the two languages: read as C it is
	 * an int and read as C++ it is the enumeration.  What makes two
	 * of them one is the enumeration they are of, and what keeps
	 * two apart is the value, which is compared beside this.
	 */
	if (auto *ec = dyn_cast<EnumConstantDecl>(d))
		if (auto *ed = dyn_cast<EnumDecl>(ec->getDeclContext()))
			return tag_type(ctx, ed);

	return QualType();
}

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
	/* Every declaration that was brought in from a unit other than
	 * the one everything was linked into, in the order it came.
	 */
	std::vector<Decl *> imported;
	/* The analysis the finishing is run under.
	 *
	 * It outlives the finishing, because what the finishing produced
	 * is in the context afterwards and the source the context reads
	 * from keeps a pointer to whichever analysis last introduced
	 * itself to it.  Declared last so that it goes before the units
	 * it was built over.
	 */
	std::unique_ptr<Sema> sema;
	/* Where what came from a unit read as C is put, in a unit read
	 * as C++.  Made when the first such unit is linked in.
	 */
	LinkageSpecDecl *c_linkage = NULL;
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
			unsigned unit, std::vector<Decl *> &imported)
		: ASTImporter(ToContext, ToFileManager, FromContext,
				FromFileManager, MinimalImport, SharedState),
		  shared_state(SharedState), unit(unit), imported(imported) {}

	/* The table the importer looks names up in, kept here because the
	 * importer's own copy of it cannot be reached from outside.
	 */
	std::shared_ptr<ASTImporterSharedState> shared_state;

	/* What to do about a name the target already has.
	 *
	 * The importer refuses, which is right for putting two views of
	 * one unit side by side and wrong for linking two units: a name
	 * that both use is not a clash, it is what makes them one
	 * program.  A function declared in one unit and defined in
	 * another arrives under the name it was written with, and so
	 * does each of two functions that C++ tells apart by their
	 * arguments and an object file does not tell apart at all.  What
	 * joins them afterwards is the name, which is what a linker
	 * joins by.
	 *
	 * Refusing loses the arriving declaration and everything in it:
	 * a definition met where the target holds a prototype is a body
	 * dropped on the floor.
	 *
	 * Only a function or a variable is let through.  Two types of
	 * one name may be two types, and letting both in would make a
	 * unit that describes neither; those are answered by
	 * find_same_type, which takes equal ones for one and leaves
	 * different ones refused.  Nor is one let through where
	 * find_same_type has an answer, since being one declaration is
	 * better than being two that a linker has to join.
	 */
	Expected<DeclarationName> HandleNameConflict(DeclarationName Name,
			DeclContext *DC, unsigned IDNS, NamedDecl **Decls,
			unsigned NumDecls) override {
		bool ours = importing && (isa<FunctionDecl>(importing) ||
					  isa<VarDecl>(importing));
		bool matched = ours && find_same_type(importing) != NULL;

		if (getenv("PET_LINK_ROOTS"))
			fprintf(stderr, "the target already has the name %s, "
				"and the link %s\n",
				Name.getAsString().c_str(),
				!ours ? "does not let it through" :
				matched ? "has something to be one with" :
				"lets it through");

		if (ours && !matched)
			return Name;

		return ASTImporter::HandleNameConflict(Name, DC, IDNS, Decls,
							NumDecls);
	}

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

		/* Which declaration the importer is working on, so that
		 * what it asks about a name it has met before can be
		 * answered about that declaration.
		 */
		Decl *outer = importing;
		importing = From;

		/* Whether anything below this import refused.  A refusal
		 * with nothing refused below it is where the trouble
		 * starts; every one above it is that one arriving again
		 * under another name, and counting those tells how far a
		 * single incompatibility reaches rather than how many
		 * there are.
		 */
		unsigned below = refusals;

		/* A member that only a C++ record can hold cannot be put
		 * into a record that is not one.
		 *
		 * Two records described by one header are one type, and
		 * taking them for one is what makes a link out of two
		 * units; but a unit read as C holds the type as a record
		 * with no C++ part, and a constructor, a destructor or a
		 * method has nowhere to go in it.  The importer does not
		 * ask -- it takes the context for a C++ record and reads
		 * it as one -- so it is asked here, and what cannot be
		 * carried is refused and said out loud rather than
		 * written into a record that cannot hold it.
		 */
		if (isa<CXXMethodDecl>(From)) {
			Decl *dc = GetAlreadyImportedOrNull(
				cast<Decl>(From->getDeclContext()));

			if (dc && isa<RecordDecl>(dc) &&
			    !isa<CXXRecordDecl>(dc))
				return llvm::make_error<ASTImportError>(
						ASTImportError::UnsupportedConstruct);
		}

		Expected<Decl *> res = import_or_unify(From);

		importing = outer;

		if (renamed)
			named->setDeclName(saved);
		if (!res) {
			if (refusals == below && getenv("PET_LINK_ROOTS")) {
				auto *nd = dyn_cast<NamedDecl>(From);

				const DeclContext *dc = From->getDeclContext();
				const auto *owner = dyn_cast<NamedDecl>(
						cast<Decl>(dc));

				fprintf(stderr, "the refusal starts at %s %s, "
					"which lives in %s %s\n",
					From->getDeclKindName(),
					nd ? nd->getQualifiedNameAsString()
						.c_str() : "<no name>",
					cast<Decl>(dc)->getDeclKindName(),
					owner ? owner->getQualifiedNameAsString()
						.c_str() : "<no name>");
			}
			refusals++;
		}

		if (res && *res) {
			carry_operator_delete(From, *res);
			carry_initialisers(From, *res);
		}

		/* Asked about one declaration by name: what became of it,
		 * unit by unit, and whether a body came with it.  A body
		 * that is in a unit and not in the link was dropped
		 * somewhere along this road, and this says at which unit.
		 */
		if (const char *want = getenv("PET_LINK_TRACE")) {
			auto *nd = dyn_cast<NamedDecl>(From);
			std::string name = nd ?
				nd->getQualifiedNameAsString() : std::string();

			if (nd && name.find(want) != std::string::npos) {
				auto *from_fn = dyn_cast<FunctionDecl>(From);
				auto *to_fn = res && *res ?
					dyn_cast<FunctionDecl>(*res) : NULL;
				const FunctionDecl *def = NULL;

				if (to_fn)
					to_fn->hasBody(def);
				fprintf(stderr, "unit %u brings %s: it %s a "
					"body, it %s, and the target's chain "
					"%s\n", unit, name.c_str(),
					from_fn && from_fn
						->doesThisDeclarationHaveABody()
						? "has" : "has no",
					!res ? "was refused" :
					*res == NULL ? "came to nothing" :
					"came across",
					!to_fn ? "is not a function" :
					def ? "has one" : "has none");
			}
		}

		/* Every declaration that comes across is written down, and
		 * here rather than where the top level ones are asked for,
		 * because most of them are not asked for: a method of a
		 * class template, or a body instantiated from one, is
		 * imported because something that was asked for reaches it.
		 * The source a unit is read from writes down what it
		 * deserialises for the same reason, and for the same
		 * purpose: what is written down here is what the linked
		 * unit announces, and what is never announced is never
		 * generated.
		 */
		if (res && *res)
			imported.push_back(*res);

		return res;
	}

private:
	unsigned unit;
	std::vector<Decl *> &imported;
	/* The declaration being imported, for the sake of what is asked
	 * about a name rather than about a declaration.
	 */
	Decl *importing = NULL;

public:
	/* How many constructors this importer identified with one the
	 * target had, and how many of those kept initialisers of another
	 * record.
	 */
	long seen_ctors = 0, stray_ctors = 0;

private:
	/* How many imports have refused so far.
	 */
	unsigned refusals = 0;

	/* What this comparison has found not to be equivalent, kept
	 * apart from what the importer's comparison found: the importer
	 * consults its own set, and a pair it has already refused over
	 * the kind of declaration it is would be refused again from the
	 * record rather than from the comparison, which is the very
	 * answer being asked again.
	 */
	LinkEquivalenceContext::NonEquivalentDeclSet non_equivalent;

	/* The name each entity of internal linkage was given, kept by the
	 * first of its declarations.
	 *
	 * A name belongs to the entity and not to the declaration.  A
	 * function declared at the top of a file and defined further down
	 * is two declarations of one entity, and asking about each of them
	 * on its own gives two answers: the first finds the name free and
	 * keeps it, the second finds the first one there and takes another
	 * -- leaving the calls at the first name and the body at the
	 * second, which is a body nothing can reach.
	 */
	std::map<const Decl *, DeclarationName> own_names;

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

		/* Another declaration of the same entity has been across
		 * already and settled the question for all of them.
		 */
		if (own_names.count(named->getCanonicalDecl()))
			return true;

		DeclarationName name(&getToContext().Idents.get(
					named->getName()));
		auto found = getToContext().getTranslationUnitDecl()
					->lookup(name);

		if (found.empty())
			return false;

		/* What the name finds may be this very entity, brought
		 * across by an earlier declaration of it.  A chain does
		 * not collide with itself, and treating it as if it did
		 * gives the calls one name and the body another.
		 */
		std::set<const Decl *> ours;

		for (const Decl *r : named->redecls())
			if (Decl *to = GetAlreadyImportedOrNull(r))
				ours.insert(to->getCanonicalDecl());

		for (NamedDecl *f : found)
			if (!ours.count(f->getCanonicalDecl()))
				return true;

		return false;
	}

	/* A name no declaration of the target has.
	 */
	DeclarationName own_name(NamedDecl *named) {
		const Decl *entity = named->getCanonicalDecl();
		auto known = own_names.find(entity);

		if (known != own_names.end())
			return known->second;

		std::string base = named->getNameAsString();

		for (unsigned n = unit; ; ++n) {
			std::string tried = base + "__" + std::to_string(n);
			DeclarationName name(&getToContext().Idents.get(tried));

			if (getToContext().getTranslationUnitDecl()->lookup(
						name).empty()) {
				DeclarationName mine(
					&getFromContext().Idents.get(tried));

				own_names[entity] = mine;
				return mine;
			}
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

		/* Only the specialisation itself, not its members: a member
		 * taken for another record's member while the records stay
		 * apart leaves a field whose record does not list it, and
		 * clang stops on that the moment anything asks the field
		 * where it sits.
		 */
		Decl *found = find_specialization(From);

		if (found && getenv("PET_LINK_WHY"))
			fprintf(stderr, "the link takes the specialisation %s "
				"for one the target has\n",
				cast<NamedDecl>(From)
					->getQualifiedNameAsString().c_str());

		if (!found)
			found = find_same_type(From);
		if (!found) {
			if (getenv("PET_LINK_ROOTS"))
				fprintf(stderr, "nothing was weighed for %s "
					"because it %s\n",
					From->getDeclKindName(),
					why_not_weighed(From));

			return llvm::make_error<ASTImportError>(
					ASTImportError::NameConflict);
		}

		/* The importer may have written down a mapping of its own
		 * before it gave up, and saying a second thing about the
		 * same declaration is what it will not have.
		 */
		if (Decl *already = GetAlreadyImportedOrNull(From))
			return already;

		Decl *to = MapImported(From, found);

		/* A definition taken for a bare declaration has to leave
		 * its body behind it.  This is the same as what is done
		 * for a record that arrives finished where the target
		 * holds a mention of it: the two are one, and the one
		 * they are is the finished one.
		 */
		carry_body(From, found);

		/* A record that arrives finished, taken for one the target
		 * holds unfinished, has to finish it.  Taking the two for
		 * one and stopping there would leave the target holding
		 * the name of a type and none of what it is made of, and
		 * every unit that reaches into it afterwards reaches into
		 * nothing.
		 */
		auto *from_rec = dyn_cast<RecordDecl>(From);
		auto *to_rec = dyn_cast<RecordDecl>(found);

		if (from_rec && to_rec && from_rec->isCompleteDefinition() &&
		    !to_rec->isCompleteDefinition() &&
		    !to_rec->isBeingDefined()) {
			if (llvm::Error err = ImportDefinition(From))
				llvm::consumeError(std::move(err));
		}

		carry_members(From, found);

		/* Two specialisations taken for one are one type, and the
		 * members each of them holds are the members of that one
		 * type.  Which of them were made is not the same on both
		 * sides -- a unit that called begin() and a unit that
		 * called size() made different ones -- so what the
		 * arriving one holds is asked for, and the importer puts
		 * each where it belongs.  Asked of the definition as a
		 * whole this does nothing, since the target's is finished
		 * already; asked member by member it brings across the
		 * ones the target never made.
		 */
		if (isa<ClassTemplateSpecializationDecl>(From) &&
		    isa<ClassTemplateSpecializationDecl>(found))
			for (Decl *member : cast<DeclContext>(From)->decls()) {
				auto res = Import(member);

				if (!res)
					llvm::consumeError(res.takeError());
			}

		return to;
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
	/* Carry the operator delete of "From" over to "to".
	 *
	 * What one declaration of an entity knows and another does not
	 * has to survive the two being made one.  Which operator delete a
	 * virtual destructor uses is such a thing: it is settled where
	 * the destructor's body is finished, which is one unit and not
	 * every unit, and the importer writes it down only on a
	 * declaration it creates.  Where it finds one in the target
	 * instead, the arriving declaration is dropped and what it knew
	 * goes with it, and a destructor that knows no operator delete is
	 * one the generator asks and gets nothing back from, which it
	 * does not survive.
	 */
	void carry_operator_delete(Decl *From, Decl *to) {
		auto *from_dtor = dyn_cast<CXXDestructorDecl>(From);
		auto *to_dtor = dyn_cast_or_null<CXXDestructorDecl>(to);

		if (!from_dtor || !to_dtor)
			return;
		if (!from_dtor->getOperatorDelete())
			return;
		if (to_dtor->getOperatorDelete())
			return;

		auto del = Import(const_cast<FunctionDecl *>(
					from_dtor->getOperatorDelete()));
		if (!del) {
			llvm::consumeError(del.takeError());
			return;
		}
		auto arg = Import(from_dtor->getOperatorDeleteThisArg());
		if (!arg) {
			llvm::consumeError(arg.takeError());
			return;
		}

		to_dtor->setOperatorDelete(cast<FunctionDecl>(*del), *arg);
	}

	/* Two declarations taken for one have their members taken for one.
	 *
	 * The comparison that said the two are the same type walked their
	 * members and found them to correspond one to one and in order,
	 * so the correspondence is known; this writes it down.  Without
	 * it the importer goes on to import each member of the arriving
	 * record on its own and decides by its own comparison whether it
	 * is the member the target already has -- the comparison that
	 * refused the records to begin with -- so a member is taken for a
	 * new one and added to a record that has already been laid out,
	 * which nothing notices until something asks that record where
	 * the member sits.
	 */
	void carry_members(Decl *From, Decl *to) {
		auto *from_rec = dyn_cast<RecordDecl>(From);
		auto *to_rec = dyn_cast_or_null<RecordDecl>(to);

		if (from_rec && to_rec) {
			/* Only where both are finished and hold as many
			 * members as each other.  A correspondence between
			 * a finished record and one still being built, or
			 * one holding fewer, is not a correspondence: it
			 * pairs the first few and leaves the rest to be
			 * imported as members of their own, which is a
			 * record whose members are some of one and some of
			 * another.
			 */
			if (!from_rec->isCompleteDefinition() ||
			    !to_rec->isCompleteDefinition())
				return;
			auto f = from_rec->field_begin();
			auto t = to_rec->field_begin();

			for (; f != from_rec->field_end() &&
			       t != to_rec->field_end(); ++f, ++t)
				map_once(*f, *t);
			return;
		}

		auto *from_enum = dyn_cast<EnumDecl>(From);
		auto *to_enum = dyn_cast_or_null<EnumDecl>(to);

		if (from_enum && to_enum) {
			auto f = from_enum->enumerator_begin();
			auto t = to_enum->enumerator_begin();

			for (; f != from_enum->enumerator_end() &&
			       t != to_enum->enumerator_end(); ++f, ++t)
				map_once(*f, *t);
		}
	}

	/* Carry the body of "From" over to "to".
	 *
	 * Where a definition was taken for a declaration the target
	 * already had, the target's is what is kept, and the target's
	 * carries no body: the body would be left in the unit it came
	 * from, where nothing in the linked unit can reach it.
	 *
	 * Whether the target carries one is asked of its declarations
	 * and not with hasBody, which answers for the template a
	 * declaration was made from as well, and a template always has
	 * a body.
	 *
	 * The parameters are said to be one another first, because the
	 * body names them, and a body naming the parameters of another
	 * declaration is not a body.
	 */
	void carry_body(Decl *From, Decl *to) {
		auto *from_fn = dyn_cast<FunctionDecl>(From);
		auto *to_fn = dyn_cast_or_null<FunctionDecl>(to);

		if (!from_fn || !to_fn)
			return;
		if (!from_fn->doesThisDeclarationHaveABody())
			return;
		for (auto *r : to_fn->redecls())
			if (r->doesThisDeclarationHaveABody())
				return;
		if (from_fn->getNumParams() != to_fn->getNumParams())
			return;

		for (unsigned i = 0; i < from_fn->getNumParams(); ++i)
			map_once(from_fn->getParamDecl(i),
					to_fn->getParamDecl(i));

		auto body = Import(from_fn->getBody());
		if (!body) {
			llvm::consumeError(body.takeError());
			return;
		}

		to_fn->setBody(*body);
	}

	/* Give a constructor the initialisers of the record it is now of.
	 *
	 * A constructor lays out its record by walking its initialisers,
	 * so each of them has to name a member of that record.  Where the
	 * arriving constructor was taken for one the target already had,
	 * the target's initialisers are kept -- and they name the members
	 * of the record that constructor was written for, which after the
	 * two records were made one is not the record it is now of.  What
	 * an initialiser names is written into it when it is made and
	 * cannot be said again, so the list is made again: each of the
	 * arriving one's is imported, which names the members through the
	 * same mapping as everything else, and the constructor is given
	 * that list.
	 */
	void carry_initialisers(Decl *From, Decl *to) {
		auto *from_ctor = dyn_cast<CXXConstructorDecl>(From);
		auto *to_ctor = dyn_cast_or_null<CXXConstructorDecl>(to);

		if (!from_ctor || !to_ctor)
			return;

		seen_ctors++;

		const RecordDecl *def = to_ctor->getParent()->getDefinition();
		if (!def)
			return;

		bool stray = false;
		for (auto *init : to_ctor->inits()) {
			FieldDecl *f = init->getMember();

			if (!f)
				continue;
			bool there = false;
			for (const FieldDecl *g : def->fields())
				if (g == f) {
					there = true;
					break;
				}
			if (!there) {
				stray = true;
				stray_ctors++;
				break;
			}
		}
		if (!stray)
			return;

		llvm::SmallVector<CXXCtorInitializer *, 4> made;
		for (auto *init : from_ctor->inits()) {
			if (getenv("PET_LINK_WHY") && init->getMember()) {
				FieldDecl *f = init->getMember();
				Decl *known = GetAlreadyImportedOrNull(f);

				fprintf(stderr, "the initialiser names %p, "
					"already taken for %p, in record %p; "
					"the record wanted is %p\n",
					(void *) f, (void *) known,
					(void *) f->getParent(),
					(void *) def);
			}

			auto res = Import(init);

			if (res && getenv("PET_LINK_WHY") &&
			    (*res)->getMember())
				fprintf(stderr, "    and the import gave %p "
					"in record %p\n",
					(void *) (*res)->getMember(),
					(void *) (*res)->getMember()
							->getParent());

			if (!res) {
				llvm::consumeError(res.takeError());
				return;
			}
			made.push_back(*res);
		}

		auto **store = new (getToContext())
				CXXCtorInitializer *[made.size()];
		std::copy(made.begin(), made.end(), store);
		to_ctor->setCtorInitializers(store);
		to_ctor->setNumCtorInitializers(made.size());
	}

	/* Say that "From" is "to", unless something has already been said
	 * about "From".
	 */
	void map_once(Decl *From, Decl *to) {
		if (!GetAlreadyImportedOrNull(From))
			MapImported(From, to);
	}

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

	/* The declaration of the target that is the same type as "From",
	 * or NULL when the target has none.
	 *
	 * A struct described by a header is one type, and two units that
	 * included the header agree on it -- but a unit read as C holds
	 * it as a record and a unit read as C++ holds it as a C++ record,
	 * because that is what each language reads a struct into.  The
	 * comparison clang makes of two declarations begins by comparing
	 * what kind of declaration each is (ASTStructuralEquivalence.cpp,
	 * CheckKindSpecificEquivalence: "Kind mismatch") and answers no
	 * before looking at anything else.  So every struct that a C unit
	 * and a C++ unit share is refused, and with it every function
	 * whose arguments mention it: over a project that has both, FILE
	 * and fclose and stdin and thousands more.
	 *
	 * The same comparison asked about the types rather than about the
	 * declarations does not go through that check -- two record types
	 * are compared as records, whichever kind of declaration named
	 * them -- and the types are what the question was about.  So it
	 * is asked that way.  Types that differ still answer no, which is
	 * what keeps two different structs of one name apart.
	 *
	 * A typedef is asked about the same way and for the same reason:
	 * what it names is a type, and a typedef of a struct is refused
	 * over the struct, which is refused over the kind.
	 *
	 * So is a function, and so is a variable, and for the same reason
	 * again: what refuses stdin is not stdin, it is FILE, which is
	 * compared inside the comparison of the two declarations of stdin
	 * and answers no over the kind.  A function or a variable is
	 * asked about only where it is a declaration and not a
	 * definition, since a definition holds what the target has not
	 * got and taking the two for one would leave it behind.
	 */
	Decl *find_same_type(Decl *From) {
		auto *from_named = dyn_cast<NamedDecl>(From);

		if (!from_named)
			return NULL;

		/* Looked for where it lives, and not only at the outermost
		 * level.  A member typedef, a method, a constant of an
		 * enumeration -- none of those is at the outermost level
		 * and each of them is one entity across the units all the
		 * same; looked for by name at the top they are not found,
		 * and not being found is a refusal that takes with it
		 * everything that mentions them.
		 *
		 * The context is not imported here, only looked up: it is
		 * either already across, in which case this is where the
		 * declaration belongs, or it is not, in which case there
		 * is nothing yet to be one with.
		 */
		DeclContext *to_dc = corresponding_context(From);
		if (!to_dc)
			return NULL;
		/* A definition may be taken for a declaration of the
		 * target only where the target's is a definition too.
		 *
		 * What the fence is for: taking a definition for a bare
		 * declaration keeps the target's, and the target's holds
		 * no body, so the body arrives and is dropped.  Where the
		 * target already carries one there is nothing to drop,
		 * and the two are one entity the same as any other pair.
		 *
		 * Everything a unit holds carries a body now that the
		 * units are written with their instantiations made, so a
		 * fence against every definition is a fence against
		 * almost everything.
		 */
		bool from_defines = !declaration_only(From);
		/* Only what a name reaches from outside the unit it was
		 * written in, since only that is one entity across units.
		 * A type carries no linkage and is not asked.
		 */
		if (isa<ValueDecl>(From) && !from_named->isExternallyVisible())
			return NULL;

		QualType from_type = type_named(getFromContext(), from_named);
		if (from_type.isNull())
			return NULL;

		IdentifierInfo *id = lookup_name(from_named);
		if (!id)
			return NULL;

		DeclarationName name(&getToContext().Idents.get(id->getName()));

		int tried = 0;
		NamedDecl *first = NULL;

		for (NamedDecl *found : named_in_to(to_dc, name)) {
			NamedDecl *cand = found;

			tried++;

			/* A record with no name of its own is looked for
			 * under the name of the typedef that stands for
			 * it, which is what the comparison names it by as
			 * well; what that name finds is the typedef, and
			 * what is wanted is the record it stands for.
			 */
			if (isa<TagDecl>(From) && !from_named->getIdentifier()) {
				auto *td = dyn_cast<TypedefNameDecl>(cand);

				if (!td)
					continue;
				cand = td->getUnderlyingType()->getAsTagDecl();
				if (!cand)
					continue;
			}

			/* A struct, a typedef, a function and a variable may
			 * share a name and are not one another.  Between a
			 * record and a C++ record the kind differs and the
			 * thing does not, which is what this is about.
			 */
			if (isa<TagDecl>(From)) {
				if (!isa<TagDecl>(cand))
					continue;
			} else if (cand->getKind() != From->getKind()) {
				continue;
			}

			QualType to_type = type_named(getToContext(), cand);
			if (to_type.isNull())
				continue;

			LinkEquivalenceContext ctx(
				getFromContext().getLangOpts(),
				getFromContext(), getToContext(),
				non_equivalent,
				LinkEquivalenceKind::Default,
				/*StrictTypeSpelling=*/false,
				/* Silent, because a difference here is an
				 * answer and not a fault -- two structs of
				 * one name may be two structs.  Asked to
				 * say why, it says.
				 */
				/*Complain=*/getenv("PET_LINK_WHY") != NULL);

			if (ctx.IsEquivalent(from_type, to_type)) {
				/* A finished one in preference to a
				 * mention of it.  A name may be both in
				 * the target, and taking the mention
				 * would leave the finished one beside it
				 * as a second record of one name.
				 */
				auto *rec = dyn_cast<RecordDecl>(cand);

				if (!rec || rec->isCompleteDefinition())
					return cand;
				if (!first)
					first = cand;
				continue;
			}

			if (getenv("PET_LINK_WHY"))
				fprintf(stderr, "the link takes %s for two: "
					"%s against %s\n",
					from_named->getNameAsString().c_str(),
					from_type.getAsString().c_str(),
					to_type.getAsString().c_str());
		}

		if (first)
			return first;

		if (getenv("PET_LINK_WHY") && !tried)
			fprintf(stderr, "the link finds nothing named %s to "
				"weigh %s against\n",
				from_named->getNameAsString().c_str(),
				from_type.getAsString().c_str());

		return NULL;
	}

	/* Why find_same_type had nothing to say about "From".
	 *
	 * Each of these is a door it turns back at, and which door is
	 * which class of refusal.
	 */
	const char *why_not_weighed(Decl *From) {
		auto *nd = dyn_cast<NamedDecl>(From);

		if (!nd)
			return "has no name at all";
		if (!declaration_only(From))
			return "is a definition and not a mention";
		if (isa<ValueDecl>(From) && !nd->isExternallyVisible())
			return "is not visible outside its own unit";
		if (type_named(getFromContext(), nd).isNull())
			return "is not a kind of thing that names a type";
		if (!lookup_name(nd))
			return "has no name to be looked for under";
		if (!corresponding_context(From))
			return "lives somewhere the target has not got";

		return "was weighed against every candidate and matched none";
	}

	/* The context of the target that answers to the context "From"
	 * lives in, or NULL when the target has none.
	 *
	 * The outermost level of one unit is the outermost level of the
	 * other; anything else has to have come across already, and what
	 * came across is what the importer wrote down.
	 */
	DeclContext *corresponding_context(Decl *From) {
		DeclContext *dc = From->getDeclContext();

		/* A constant of an enumeration that is not scoped is
		 * named where the enumeration is named, not inside it,
		 * and that is where one of the same name is to be looked
		 * for -- the enumeration itself is still being made when
		 * its constants arrive, so there is nothing to look in.
		 */
		if (auto *ed = dyn_cast<EnumDecl>(dc))
			if (!ed->isScoped())
				dc = ed->getDeclContext();

		if (dc->isTranslationUnit())
			return getToContext().getTranslationUnitDecl();

		Decl *to = GetAlreadyImportedOrNull(cast<Decl>(dc));

		return to ? dyn_cast<DeclContext>(to) : NULL;
	}

	/* Everything in "to_dc" that goes by "name".
	 *
	 * Copied from ASTImporter::findDeclsInToCtx, which is where the
	 * importer itself looks and which the link cannot call because it
	 * is private.  Looking anywhere else means seeing less than the
	 * importer sees: a record the importer made for an implicit
	 * declaration -- __va_list_tag is one -- never reaches the
	 * context's own lookup, because the context it was copied from
	 * does not list it either, and only the importer's table knows
	 * it is there.  The link would then find nothing to be one with,
	 * refuse the name as taken, and lose every function that names a
	 * va_list.
	 */
	SmallVector<NamedDecl *, 2> named_in_to(DeclContext *DC,
						DeclarationName Name) {
		/* We search in the redecl context because of transparent
		 * contexts.
		 */
		DeclContext *ReDC = DC->getRedeclContext();
		SmallVector<NamedDecl *, 2> Result;

		if (auto *table = shared_state->getLookupTable()) {
			if (ReDC->isNamespace()) {
				/* Namespaces can be reopened, and the table
				 * holds each opening on its own.
				 */
				for (auto *ns :
				     cast<NamespaceDecl>(ReDC)->redecls()) {
					auto found = table->lookup(
						cast<NamespaceDecl>(ns), Name);
					Result.append(found.begin(),
						      found.end());
				}
				return Result;
			}
			auto found = table->lookup(ReDC, Name);
			Result.append(found.begin(), found.end());
			return Result;
		}

		DeclContext::lookup_result plain = ReDC->noload_lookup(Name);
		Result.append(plain.begin(), plain.end());
		if (Result.empty())
			ReDC->localUncachedLookup(Name, Result);

		return Result;
	}

	/* The name to look for "d" under, or NULL when it has none.
	 *
	 * Its own name, and for a tag declaration that has none the name
	 * of the typedef that stands for it -- which is the name the
	 * comparison knows it by too (NameIsStructurallyEquivalent), so
	 * looking for it under any other name would be looking for
	 * something the comparison would not recognise.
	 */
	static IdentifierInfo *lookup_name(NamedDecl *d) {
		if (IdentifierInfo *id = d->getIdentifier())
			return id;
		if (auto *tag = dyn_cast<TagDecl>(d))
			if (TypedefNameDecl *td = tag->getTypedefNameForAnonDecl())
				return td->getIdentifier();

		return NULL;
	}

	/* Does "d" say only that something exists, rather than what it
	 * is?
	 *
	 * A type is always taken to say only that, since what a struct is
	 * is compared rather than carried; a function or a variable that
	 * carries a body or an initialiser is not.
	 */
	static bool declaration_only(Decl *d) {
		if (auto *fn = dyn_cast<FunctionDecl>(d))
			return !fn->doesThisDeclarationHaveABody();
		if (auto *var = dyn_cast<VarDecl>(d))
			return var->isThisDeclarationADefinition() ==
				VarDecl::DeclarationOnly;

		return true;
	}
};

/* Point every member access at the record it reads from.
 *
 * The generator finds a member's place by asking the record that the
 * base expression has, so a member has to be that record's: a field of
 * another link of the same chain is one the record does not know, and
 * the generator stops on it.  Two links come about because the base
 * type and the member arrive by different paths -- one before the
 * records were made one, the other after -- and each is right about
 * itself.
 *
 * So they are made to agree, which is a linker's work and not a
 * compiler's: the same field, named the same, of the record the base
 * has.  Where there is no such field the access is left as it is and
 * says so, rather than being pointed somewhere convenient.
 */
static void aim_members(FunctionDecl *fn, long &fixed, long &left)
{
	struct walker : RecursiveASTVisitor<walker> {
		long &fixed, &left;

		walker(long &fixed, long &left) : fixed(fixed), left(left) {}

		bool VisitMemberExpr(MemberExpr *me) {
			auto *field = dyn_cast<FieldDecl>(me->getMemberDecl());

			if (!field)
				return true;

			QualType base = me->getBase()->getType();
			if (base->isPointerType())
				base = base->getPointeeType();

			auto *of = dyn_cast_or_null<RecordDecl>(
					base->getAsTagDecl());
			if (!of || of == field->getParent())
				return true;
			/* Reading a member of a base class through an
			 * expression of the derived one is how C++ is
			 * written.  Only two links of one chain are the
			 * same entity told apart.
			 */
			if (of->getCanonicalDecl() !=
			    field->getParent()->getCanonicalDecl())
				return true;

			RecordDecl *has = of->getDefinition();
			if (!has) {
				left++;
				return true;
			}

			for (FieldDecl *f : has->fields()) {
				if (f->getDeclName() != field->getDeclName())
					continue;
				me->setMemberDecl(f);
				fixed++;
				return true;
			}

			left++;
			return true;
		}
	};

	if (!fn->doesThisDeclarationHaveABody())
		return;

	walker w(fixed, left);
	w.TraverseStmt(fn->getBody());
}

static void aim_context(DeclContext *dc, std::set<Decl *> &seen, long &fixed,
	long &left)
{
	for (Decl *d : dc->decls()) {
		if (!seen.insert(d).second)
			continue;

		if (auto *fn = dyn_cast<FunctionDecl>(d))
			aim_members(fn, fixed, left);

		if (auto *td = dyn_cast<ClassTemplateDecl>(d))
			for (auto *spec : td->specializations())
				aim_context(spec, seen, fixed, left);
		if (auto *td = dyn_cast<FunctionTemplateDecl>(d))
			for (auto *spec : td->specializations()) {
				if (!seen.insert(spec).second)
					continue;
				aim_members(spec, fixed, left);
			}

		if (auto *nested = dyn_cast<DeclContext>(d))
			aim_context(nested, seen, fixed, left);
	}
}

/* Is "d" one of the declarations a unit announces to whatever consumes
 * it?
 *
 * This is the rule the source a unit is read from applies to everything
 * it deserialises -- ASTReader::isConsumerInterestedIn -- said again
 * over what is not read but imported: a function that carries a body, a
 * variable at file scope that is a definition rather than a mention,
 * and assembly written at the outermost level.  Everything else is
 * either reached from one of those or is nothing to generate code from.
 */
static bool worth_announcing(Decl *d)
{
	if (auto *fn = dyn_cast<FunctionDecl>(d))
		return fn->doesThisDeclarationHaveABody();
	if (auto *var = dyn_cast<VarDecl>(d))
		return var->isFileVarDecl() &&
			var->isThisDeclarationADefinition() ==
				VarDecl::Definition;
	return isa<FileScopeAsmDecl>(d);
}

/* Finish the linked unit the way a compilation finishes one.
 *
 * A translation unit is not done when its declarations are in place.
 * clang drives the rest from ParseAST: it tells the source the unit was
 * read from which consumer to feed, lets the parser reach the end of the
 * file, has Sema act on that end, and only then hands the unit over.
 * Acting on the end is where the templates the unit uses are
 * instantiated, the virtual tables it needs are defined, the destructors
 * whose bodies were finished are checked, and the instantiations those
 * in turn ask for are instantiated again.
 *
 * A linked unit is one no parser ever reached the end of, so none of it
 * has happened.  What comes out without it calls functions nothing
 * defines: every std::vector<int>::clear the units left as a
 * declaration stays one.
 *
 * So the finishing is not written out here again, in whatever order it
 * was worked out to be: ParseAST is called, and it is ParseAST that
 * decides what finishing a unit consists of and in what order.  This is
 * only what it needs in order to be called at all -- an analysis to run
 * under, and a file to reach the end of.
 *
 * What the finishing produces is told to "consumer" and to nothing else.
 * That is why the analysis is made here rather than taken from the unit:
 * a unit is read under an analysis whose consumer ignores everything,
 * and an instantiation announced to it is in the context and reaches no
 * code generator, so a linked unit compiled afterwards calls std::max
 * with nothing to call.
 *
 * ParseAST hands the unit over when it is done, so what consumes it has
 * been given it by the time this returns.
 */
void pet_linked_ast_finish(struct pet_linked_ast *linked,
	ASTConsumer &consumer)
{
	ASTUnit *target = linked->units[0].get();
	Preprocessor &pp = target->getPreprocessor();
	SourceManager &sm = target->getSourceManager();

	linked->sema = std::unique_ptr<Sema>(new Sema(pp,
				target->getASTContext(), consumer));
	Sema &sema = *linked->sema;
	sema.Initialize();

	/* ParseAST finishes a unit only when there is something to lex:
	 * the scopes it needs are set up by the parser, and the end of
	 * the unit is acted on when the parser reaches the end of the
	 * file.  A linked unit has no file, so it is given an empty one.
	 * The parser reaches the end of it at once, having read nothing,
	 * and everything that follows reading happens.
	 */
	sm.setMainFileID(sm.createFileID(
		llvm::MemoryBuffer::getMemBuffer("", "<linked>")));

	/* What was imported is announced, because nothing else will.
	 * The unit that was read into is announced by the source it was
	 * read from, which ParseAST tells whom to feed; the units that
	 * were imported into it have no such source here, and what is
	 * never announced is never generated.
	 *
	 * The whole unit is gone through rather than the list of what
	 * was imported, because that list holds what the importer was
	 * asked for and not what it made: an instantiation reached
	 * through a template is in neither.  Announcing what was read
	 * as well costs nothing, since a declaration announced twice is
	 * emitted once.
	 */
	for (Decl *d : linked->imported)
		if (worth_announcing(d))
			consumer.HandleTopLevelDecl(DeclGroupRef(d));


	/* Before anything is generated, because it is what makes the
	 * unit one: a link joins declarations, and an expression that
	 * still reads from the declaration it arrived with has not been
	 * joined to anything.
	 */
	std::set<Decl *> aimed;
	long fixed = 0, left = 0;

	if (!getenv("PET_LINK_NO_AIM"))
		aim_context(target->getASTContext().getTranslationUnitDecl(),
				aimed, fixed, left);
	if (getenv("PET_LINK_WHY"))
		fprintf(stderr, "%ld member accesses were pointed at the "
			"record they read from, and %ld had nowhere to "
			"point\n", fixed, left);

	ParseAST(sema, /*PrintStats=*/false, /*SkipFunctionBodies=*/false);
}

/* Where the declarations brought from a unit read as C belong in a unit
 * read as C++.
 *
 * A C entity named in C++ is declared inside extern "C", and that is
 * not decoration: it is what says the entity's name is its name, rather
 * than a name with the types of its arguments worked into it.  A link
 * that brings a C unit into a C++ one and leaves its declarations at
 * the outermost level renames every one of them --
 * dequantize_row_q4_0 becomes _Z19dequantize_row_q4_0PK10block_q4_0Pfl
 * -- and nothing written against the C names can be linked with what
 * comes out.
 *
 * Made once and kept, because a second one would be a second block and
 * the declarations of one unit belong in one.
 */
static LinkageSpecDecl *c_linkage(struct pet_linked_ast *linked)
{
	ASTContext &ctx = linked->units[0]->getASTContext();

	if (!linked->c_linkage)
		linked->c_linkage = LinkageSpecDecl::Create(ctx,
			ctx.getTranslationUnitDecl(), SourceLocation(),
			SourceLocation(), c_language(), /*HasBraces=*/true);

	return linked->c_linkage;
}

/* Put "d" where a C entity belongs in a unit read as C++.
 *
 * Only what stands at the outermost level and is a function or a
 * variable, since those are what carry a name into an object file, and
 * only what is still at the outermost level: a declaration the target
 * already had is where the target put it, and where that is has already
 * been decided.
 */
static void give_c_linkage(struct pet_linked_ast *linked, Decl *d)
{
	TranslationUnitDecl *tu =
		linked->units[0]->getASTContext().getTranslationUnitDecl();

	if (!isa<FunctionDecl>(d) && !isa<VarDecl>(d))
		return;
	if (d->getDeclContext() != tu || d->getLexicalDeclContext() != tu)
		return;
	/* And only what is in the unit's list of declarations.  A
	 * declaration the target already had is one the target placed,
	 * and taking something for it does not put it anywhere new.
	 */
	if (!tu->containsDecl(d))
		return;

	LinkageSpecDecl *ls = c_linkage(linked);

	tu->removeDecl(d);
	d->setDeclContext(ls);
	d->setLexicalDeclContext(ls);
	ls->addDecl(d);
}

/* Say that "d" could not be linked, and why.
 */
static void note_refusal(struct pet_linked_ast *linked, Decl *d,
			llvm::Error err)
{
	std::string why = llvm::toString(std::move(err));
	auto *nd = dyn_cast<NamedDecl>(d);

	linked->refused.push_back(nd ? nd->getNameAsString() : "");
	linked->refused_why.push_back(std::string(d->getDeclKindName()) +
					" " + why);
}

/* Bring the body of "fn" over, and say so if it did not come.
 */
static void carry_over_body(struct pet_linked_ast *linked,
			link_importer &importer, FunctionDecl *fn)
{
	Decl *to = importer.GetAlreadyImportedOrNull(fn);

	if (!to) {
		auto res = importer.Import(fn);

		if (!res) {
			note_refusal(linked, fn, res.takeError());
			return;
		}
		to = *res;
	}

	auto *to_fn = dyn_cast_or_null<FunctionDecl>(to);
	const FunctionDecl *def = NULL;

	if (to_fn && to_fn->hasBody(def) && def)
		return;

	note_refusal(linked, fn, llvm::make_error<ASTImportError>(
					ASTImportError::Unknown));
}

/* Offer every function of "dc" that carries a body to the link.
 *
 * The outermost level of a unit is walked declaration by declaration,
 * and that reaches a member of a record only through the record.  Where
 * the record was taken for one the target already had, the importer
 * stops at the record: its members are the target's members, and the
 * arriving ones are never asked for.  That is right for the members
 * themselves and wrong for their bodies -- a class written in a header
 * is declared in every unit that includes it and defined in one, so the
 * unit that defines it is exactly the one whose members are not asked
 * for.
 *
 * So each is asked for by name here, and what came of it is looked at
 * rather than taken on trust: the importer writes down which target
 * declaration an arriving one became before it imports the body, so a
 * body that fails to come leaves the declaration behind it and the
 * next ask for it succeeds.  A function that has a body here and none
 * there is a body the link lost, whatever the importer said.
 */
static void offer_bodies(struct pet_linked_ast *linked,
			link_importer &importer, DeclContext *dc)
{
	for (Decl *d : dc->decls()) {
		if (auto *fn = dyn_cast<FunctionDecl>(d))
			if (fn->doesThisDeclarationHaveABody())
				carry_over_body(linked, importer, fn);
		/* An instantiation of a template is in no context's list
		 * of declarations: it hangs off the template it was made
		 * from.
		 */
		if (auto *td = dyn_cast<ClassTemplateDecl>(d))
			for (auto *spec : td->specializations())
				offer_bodies(linked, importer, spec);
		if (auto *td = dyn_cast<FunctionTemplateDecl>(d))
			for (auto *spec : td->specializations())
				if (spec->doesThisDeclarationHaveABody())
					carry_over_body(linked, importer, spec);
		if (auto *nested = dyn_cast<DeclContext>(d))
			if (!isa<FunctionDecl>(d))
				offer_bodies(linked, importer, nested);
	}
}

/* Import every top level declaration of "src" into the linked context.
 */
static void link_unit(struct pet_linked_ast *linked, ASTUnit *src)
{
	ASTUnit *target = linked->units[0].get();
	size_t first = linked->imported.size();
	link_importer importer(target->getASTContext(),
		target->getFileManager(), src->getASTContext(),
		src->getFileManager(), /*MinimalImport=*/false,
		linked->shared, linked->units.size(), linked->imported);
	for (Decl *d : src->getASTContext().getTranslationUnitDecl()->decls()) {
		auto res = importer.Import(d);
		if (res)
			continue;
		/* Why it was refused is the only thing that says what to
		 * do about it, so it is kept rather than thrown away.
		 */
		note_refusal(linked, d, res.takeError());
	}

	offer_bodies(linked, importer,
		src->getASTContext().getTranslationUnitDecl());

	if (getenv("PET_LINK_WHY"))
		fprintf(stderr, "the importer identified %ld constructors, %ld "
			"of them keeping initialisers of another record\n",
			importer.seen_ctors, importer.stray_ctors);

	if (src->getASTContext().getLangOpts().CPlusPlus ||
	    !target->getASTContext().getLangOpts().CPlusPlus)
		return;

	for (size_t i = first; i < linked->imported.size(); ++i)
		give_c_linkage(linked, linked->imported[i]);
}

/* Link the translation units serialised in the "n" files in "files".
 *
 * The first unit is read directly into the result; it is the context
 * everything else is imported into.
 *
 * The result is linked and not yet finished: finishing it produces
 * declarations, and where those go is the caller's to say, so it is
 * pet_linked_ast_finish that says it.
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
