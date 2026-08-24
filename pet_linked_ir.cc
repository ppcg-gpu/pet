/* Generate LLVM IR from a linked AST.
 *
 * Counting what a link resolved says how much of it came together; it
 * does not say that what came together is a program.  Handing the linked
 * AST to the code generator does: the generator walks every declaration
 * it holds and builds a module out of them, the verifier then says
 * whether that module is well formed, and what comes out can be
 * assembled and run.  A link that lost a body, or kept two records of
 * one type, or left a call pointing at nothing, does not survive that.
 *
 *	pet_linked_ir out.ll unit.ast...
 *
 * The first unit is the one everything else is linked into, as in
 * pet_ast_link.
 */

#include <map>
#include <clang_features.h>

#include <set>
#include <string>

#include <stdio.h>
#include <string.h>

#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclGroup.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/PrettyPrinter.h>
#include <clang/AST/RecordLayout.h>
#include <iterator>
#include <llvm/Support/raw_ostream.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/TargetInfo.h>
#include <clang/CodeGen/ModuleBuilder.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/TextDiagnosticPrinter.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include "ast_link.h"

using namespace clang;

/* A consumer that writes down what it is told and nothing more.
 *
 * What the generator has to say is worth reading, but rendering it the
 * usual way means going back to the text a declaration came from, and a
 * linked AST holds declarations from several units at once, whose
 * locations only mean something against the unit they came from.  So
 * the message is printed and the location is not.
 */
struct plain_diagnostics : public DiagnosticConsumer {
	int errors = 0;

	void HandleDiagnostic(DiagnosticsEngine::Level level,
				const Diagnostic &info) override {
		llvm::SmallString<200> msg;

		info.FormatDiagnostic(msg);
		if (level >= DiagnosticsEngine::Error)
			errors++;
		fprintf(stderr, "%s: %s\n",
			level >= DiagnosticsEngine::Error ? "error" : "warning",
			msg.c_str());
	}
};

/* A compiler instance that says what the linked AST says.
 *
 * The code generator asks its caller for the options a translation unit
 * was read with rather than reading them off the unit, so they are put
 * back: the language and the target of the linked context, and nothing
 * else, since nothing else is being compiled here.
 */
static CompilerInstance *configure(ASTContext &ctx)
{
	auto invocation = std::make_shared<CompilerInvocation>();
	CompilerInstance *ci;

	invocation->getLangOpts() = ctx.getLangOpts();
	/* The same optimisation level the units were built with.  At zero
	 * the generator emits the body of an inline instantiation only to
	 * mark it as available elsewhere and drop it, which leaves the
	 * calls to it with nothing to reach: std::max<int> and the three
	 * hundred like it are declared and never defined.  Above zero the
	 * bodies are kept, which is what the compilation of a unit does
	 * and what makes the result linkable.
	 */
	invocation->getCodeGenOpts().OptimizationLevel = 2;
	invocation->getTargetOpts().Triple =
			ctx.getTargetInfo().getTriple().str();

	ci = new CompilerInstance(invocation);
	/* The instance is given a file system of its own first.  Nothing
	 * here opens a file, but createDiagnostics reaches for the
	 * instance's file system to hand to the engine it makes, and an
	 * instance that was never given one holds nothing to reach for:
	 * the constructor does not make one, whether or not it is handed
	 * an invocation.
	 */
	ci->createVirtualFileSystem();
	ci->createDiagnostics(new plain_diagnostics(),
				/*ShouldOwnClient=*/true);
	/* The linked context was read with a target of its own and carries
	 * it, so it is used rather than made again: what the code is
	 * generated for has to be what it was read for.
	 */
	ci->setTarget(const_cast<TargetInfo *>(&ctx.getTargetInfo()));
	/* Where the code came from is deliberately not handed over.  A
	 * linked unit holds declarations from several units and a
	 * location only means something against the one it came from, so
	 * nothing here may go looking one up; the consumer above prints
	 * what it is told and asks no questions.
	 */

	return ci;
}

/* How many finished records of each name the unit holds.
 *
 * More than one is a type the link failed to make one of, and every
 * expression written against either of them reaches for a member the
 * other does not have.
 */
static std::map<std::string, int> record_count;

/* How many of the declarations the unit holds carry a body, and how
 * many are a mention of something with none.
 */
static long bodies, bodyless;

/* How many delete expressions the unit holds, and how many of them
 * have nothing to call.
 */
static long deletes, empty_deletes;

/* How many member accesses the unit holds, and how many of them read a
 * field from a record that does not hold it.
 */
static long accesses, astray;

/* How many fields are not in the definition of the record they say they
 * are in.
 */
static long split;

/* How many initialisers name a member of another record, and how many
 * name a base.
 */
static long stray_members, bases;

/* Say which delete expressions of "fn" have no operator delete.
 *
 * A delete expression carries the operator delete it is to call, and it
 * carries it itself rather than asking the type for it.  Where the
 * expression was brought across and the operator was not, what is left
 * is a delete that calls nothing, and the generator asks it what to
 * call the moment it reaches it.
 */
static void check_deletes(FunctionDecl *fn, long &deletes, long &empty)
{
	struct walker : RecursiveASTVisitor<walker> {
		FunctionDecl *fn;
		long &deletes, &empty;

		walker(FunctionDecl *fn, long &deletes, long &empty)
			: fn(fn), deletes(deletes), empty(empty) {}

		bool VisitCXXDeleteExpr(CXXDeleteExpr *e) {
			deletes++;
			if (e->getOperatorDelete())
				return true;
			empty++;
			fprintf(stderr, "the delete in %s has nothing to "
				"call\n",
				fn->getQualifiedNameAsString().c_str());
			return true;
		}
	};

	if (!fn->doesThisDeclarationHaveABody())
		return;
	/* Not the templates themselves.  Which operator delete a delete
	 * expression calls is settled when a template is made into
	 * something, and until then it is empty because there is nothing
	 * yet to settle it.
	 */
	if (fn->isTemplated() || fn->isDependentContext())
		return;

	walker w(fn, deletes, empty);
	w.TraverseStmt(fn->getBody());
}

/* The type a tag declaration declares, named the way the version of
 * clang in hand names it.
 */
#ifdef ASTCONTEXT_HAS_CANONICAL_TAG_TYPE
static QualType tag_type_of(ASTContext &ctx, const TagDecl *tag)
{
	return ctx.getCanonicalTagType(tag);
}
#else
static QualType tag_type_of(ASTContext &ctx, const TagDecl *tag)
{
	return ctx.getTagDeclType(tag);
}
#endif

/* Say everything about every record of a given name.
 *
 * Two records of one name in one unit are two types, and an expression
 * written against one reaches into the other; which of the two it is
 * cannot be seen from the outside, so it is printed: what each of them
 * was made from, what it holds, and which record each member says it
 * belongs to.
 */
static void show_one(Decl *d, const char *want);

/* Go through everything the unit holds, including what hangs off the
 * templates, and say everything about every record of the given name.
 */
static void show_records(DeclContext *dc, const char *want,
	std::set<Decl *> &seen)
{
	for (Decl *d : dc->decls()) {
		if (!seen.insert(d).second)
			continue;

		show_one(d, want);

		if (auto *td = dyn_cast<ClassTemplateDecl>(d))
			for (auto *spec : td->specializations()) {
				if (!seen.insert(spec).second)
					continue;
				show_one(spec, want);
				show_records(spec, want, seen);
			}
		if (auto *nested = dyn_cast<DeclContext>(d))
			show_records(nested, want, seen);
	}
}

static void show_one(Decl *d, const char *want)
{
	auto *rd = dyn_cast<CXXRecordDecl>(d);

	if (!rd || rd->getNameAsString() != want)
		return;

	ASTContext &ctx = rd->getASTContext();
	std::string written, canonical;

	if (auto *spec = dyn_cast<ClassTemplateSpecializationDecl>(rd)) {
		llvm::raw_string_ostream w(written), c(canonical);
		llvm::SmallVector<TemplateArgument, 4> canon;

		printTemplateArgumentList(w, spec->getTemplateArgs().asArray(),
					ctx.getPrintingPolicy());
		for (const TemplateArgument &a :
				spec->getTemplateArgs().asArray())
			canon.push_back(ctx.getCanonicalTemplateArgument(a));
		printTemplateArgumentList(c, canon, ctx.getPrintingPolicy());
	}

	/* The type itself, not how it is written.  Two records of one
	 * canonical type are one type, whatever the printer says: the
	 * printer leaves out an argument that was defaulted, so two
	 * records that differ in exactly that print alike.
	 */
	QualType self = tag_type_of(ctx, rd);

	fprintf(stderr, "record %p %s%s canonical%s %s fields %u type %p\n",
		(void *) rd, want, written.c_str(), canonical.c_str(),
		rd->isCompleteDefinition() ? "finished" : "a-mention",
		rd->isCompleteDefinition() ?
			(unsigned) std::distance(rd->field_begin(),
						rd->field_end()) : 0u,
		self.getCanonicalType().getAsOpaquePtr());

	/* Where everything in the record sits, as clang works it out.
	 * The same dump the compiler makes with -fdump-record-layouts,
	 * so that a layout the link produced can be held against the
	 * layout the ordinary build produced for the same record.
	 */
	if (rd->isCompleteDefinition() && getenv("PET_IR_LAYOUT")) {
		llvm::errs() << "  it is " << (rd->isEmpty() ? "" : "not ")
			     << "empty, has " << rd->getNumBases()
			     << " base(s), and is "
			     << (rd->isPolymorphic() ? "" : "not ")
			     << "polymorphic\n";
		for (const CXXBaseSpecifier &b : rd->bases()) {
			auto *base = b.getType()->getAsCXXRecordDecl();

			llvm::errs() << "  base "
				     << b.getType().getAsString() << " is "
				     << (!base ? "not a record" :
					 !base->hasDefinition() ? "a mention" :
					 base->isEmpty() ? "empty" :
					 "not empty");
			if (base && base->hasDefinition())
				llvm::errs() << ", "
					     << ctx.getASTRecordLayout(base)
						     .getSize().getQuantity()
					     << " byte(s), "
					     << std::distance(base->field_begin(),
							      base->field_end())
					     << " field(s)";
			llvm::errs() << "\n";
		}
		ctx.DumpRecordLayout(rd, llvm::errs(), /*Simple=*/false);
		llvm::errs().flush();
	}

	/* The chain this record is one link of.  Two links that are both
	 * finished, or one name standing for two fields across the
	 * chain, is a record the generator can lay out one way and an
	 * expression can read the other way.
	 */
	unsigned links = 0, finished = 0;
	std::map<std::string, int> named;

	for (auto *r : rd->redecls()) {
		auto *rr = cast<CXXRecordDecl>(r);

		links++;
		if (!rr->isCompleteDefinition())
			continue;
		finished++;
		for (FieldDecl *f : rr->fields())
			named[f->getNameAsString()]++;
	}
	if (finished > 1)
		fprintf(stderr, "    ITS CHAIN HAS %u FINISHED LINKS OF %u\n",
			finished, links);
	for (auto &n : named)
		if (n.second > 1)
			fprintf(stderr, "    ITS CHAIN HAS %d FIELDS CALLED "
				"%s\n", n.second, n.first.c_str());

	if (!rd->isCompleteDefinition())
		return;

	for (FieldDecl *f : rd->fields()) {
		bool listed = false;

		for (FieldDecl *g : f->getParent()->fields())
			if (g == f) {
				listed = true;
				break;
			}
		fprintf(stderr, "    field %p %s, its record is %p%s%s\n",
			(void *) f, f->getNameAsString().c_str(),
			(void *) f->getParent(),
			f->getParent() == rd ? "" : " WHICH IS NOT THIS ONE",
			listed ? "" : " AND DOES NOT LIST IT");
	}
}

/* Say which member accesses reach for a field of a record other than
 * the one they are reading from.
 *
 * The generator finds a member's place by asking the record the base
 * expression has; if the field it is given belongs to another link of
 * that record's chain, the record does not know it and the generator
 * stops.  Both are in the linked unit and both are right about
 * themselves, so nothing about either says it on its own -- only the
 * pair does.
 */
static void check_members(FunctionDecl *fn, long &accesses, long &astray)
{
	struct walker : RecursiveASTVisitor<walker> {
		FunctionDecl *fn;
		long &accesses, &astray;

		walker(FunctionDecl *fn, long &accesses, long &astray)
			: fn(fn), accesses(accesses), astray(astray) {}

		bool VisitMemberExpr(MemberExpr *me) {
			auto *field = dyn_cast<FieldDecl>(me->getMemberDecl());

			if (!field)
				return true;

			QualType base = me->getBase()->getType();
			if (base->isPointerType())
				base = base->getPointeeType();

			TagDecl *of = base->getAsTagDecl();
			if (!of)
				return true;

			accesses++;
			if (of == field->getParent())
				return true;
			/* Reading a member of a base class through an
			 * expression of the derived one is how C++ is
			 * written, and the record it names is the derived
			 * one.  Only two links of one chain are a fault:
			 * there the record and the field are the same
			 * entity told apart.
			 */
			if (of->getCanonicalDecl() !=
			    field->getParent()->getCanonicalDecl())
				return true;

			astray++;
			fprintf(stderr, "in %s the field %s of %p is read "
				"from %p, and the two are %s\n",
				fn->getQualifiedNameAsString().c_str(),
				field->getNameAsString().c_str(),
				(void *) field->getParent(), (void *) of,
				"links of one chain");
			return true;
		}
	};

	if (!fn->doesThisDeclarationHaveABody())
		return;

	walker w(fn, accesses, astray);
	w.TraverseStmt(fn->getBody());
}

/* Say which of a constructor's initialisers name something that is not
 * of the record the constructor belongs to.
 *
 * A constructor lays out its record by walking its initialisers, so a
 * member named there has to be a member of that record and a base named
 * there has to be one of its bases.  Where a constructor was carried
 * into a record taken for the one it was written for, the initialisers
 * still name what they named, and the record it is now of does not know
 * them.
 */
static void check_inits(FunctionDecl *fn)
{
	auto *ctor = dyn_cast<CXXConstructorDecl>(fn);

	if (!ctor || !ctor->doesThisDeclarationHaveABody())
		return;

	const CXXRecordDecl *of = ctor->getParent();
	const RecordDecl *def = of->getDefinition();

	for (auto *init : ctor->inits()) {
		if (FieldDecl *f = init->getMember()) {
			bool there = false;

			for (const FieldDecl *g : (def ? def : of)->fields())
				if (g == f) {
					there = true;
					break;
				}
			if (!there) {
				stray_members++;
				if (stray_members <= 3)
					fprintf(stderr, "the constructor of %s "
						"begins with the member %s, "
						"which is of another record\n",
						of->getNameAsString().c_str(),
						f->getNameAsString().c_str());
			}
			continue;
		}
		if (init->isBaseInitializer())
			bases++;
	}
}

/* Count the records in "dc" and say which of them list a field that
 * belongs to another record.
 */
static void check_records(DeclContext *dc, long &records, long &wrong)
{
	for (Decl *d : dc->decls()) {
		if (auto *rd = dyn_cast<RecordDecl>(d)) {
			if (rd->isCompleteDefinition()) {
				std::set<std::string> seen;

				records++;
				/* Only what a name belongs to on its own.
				 * A template makes as many records of one
				 * name as it is used with, and those are
				 * told apart by their arguments and not by
				 * the name.
				 */
				if (!rd->getNameAsString().empty() &&
				    !isa<ClassTemplateSpecializationDecl>(rd) &&
				    (rd->getDeclContext()->isTranslationUnit() ||
				     rd->getDeclContext()->isExternCContext()))
					record_count[rd->getNameAsString()]++;
				for (FieldDecl *f : rd->fields()) {
					/* The record a field says it is in
					 * and the definition of that
					 * record have to be one object:
					 * the layout is built from the
					 * definition, and a field the
					 * definition does not hold has no
					 * place in it.
					 */
					RecordDecl *def = f->getParent()
							->getDefinition();
					bool there = false;

					for (FieldDecl *g : def ? def->fields()
							: rd->fields())
						if (g == f) {
							there = true;
							break;
						}
					if (!there) {
						split++;
						if (split <= 3)
							fprintf(stderr, "the "
							"field %s of %s is not "
							"in the definition of "
							"its record\n",
							f->getNameAsString()
								.c_str(),
							rd->getNameAsString()
								.c_str());
					}
					/* A name that is there twice is a
					 * member that arrived twice: one of
					 * the two is in the record's layout
					 * and the other is not, and which
					 * one an expression reaches for is
					 * not the record's to say.
					 */
					if (!f->getNameAsString().empty() &&
					    !seen.insert(f->getNameAsString())
							.second) {
						wrong++;
						fprintf(stderr, "the record %s "
							"lists the field %s "
							"twice\n",
							rd->getNameAsString()
								.c_str(),
							f->getNameAsString()
								.c_str());
					}
					if (f->getParent() == rd)
						continue;
					wrong++;
					fprintf(stderr, "the record %s lists "
						"the field %s, which belongs "
						"to %s\n",
						rd->getNameAsString().c_str(),
						f->getNameAsString().c_str(),
						f->getParent()->getNameAsString()
							.c_str());
				}
			}
		}
		if (auto *fn = dyn_cast<FunctionDecl>(d)) {
			if (fn->doesThisDeclarationHaveABody())
				bodies++;
			else
				bodyless++;
			/* Asked about one function by name: whether the
			 * link holds a body for it anywhere along its
			 * chain, which is what the code generator asks
			 * before it writes one out.
			 */
			if (const char *want = getenv("PET_IR_BODY")) {
				std::string name =
					fn->getQualifiedNameAsString();

				if (name.find(want) != std::string::npos) {
					const FunctionDecl *def = NULL;

					fn->hasBody(def);
					fprintf(stderr, "%s: this one %s a "
						"body, the chain %s, %ld "
						"declaration(s) long\n",
						name.c_str(),
						fn->doesThisDeclarationHaveABody()
							? "has" : "has no",
						def ? "has one" : "has none",
						(long) std::distance(
							fn->redecls_begin(),
							fn->redecls_end()));
				}
			}
			check_deletes(fn, deletes, empty_deletes);
			check_members(fn, accesses, astray);
			check_inits(fn);
		}
		/* An instantiation of a template is in no context's list
		 * of declarations: it hangs off the template it was made
		 * from, and most of what a unit of C++ holds is one.
		 */
		if (auto *td = dyn_cast<ClassTemplateDecl>(d))
			for (auto *spec : td->specializations())
				check_records(spec, records, wrong);
		if (auto *td = dyn_cast<FunctionTemplateDecl>(d))
			for (auto *spec : td->specializations()) {
				check_deletes(spec, deletes, empty_deletes);
				check_members(spec, accesses, astray);
				check_records(cast<DeclContext>(spec), records,
						wrong);
			}

		if (auto *nested = dyn_cast<DeclContext>(d))
			check_records(nested, records, wrong);
	}
}

int main(int argc, char *argv[])
{
	struct pet_linked_ast *linked;
	llvm::LLVMContext llvm_ctx;
	std::error_code ec;
	int refused;

	if (argc < 3) {
		fprintf(stderr, "usage: %s out.ll unit.ast...\n", argv[0]);
		return 1;
	}

	linked = pet_ast_link((const char **) argv + 2, argc - 2);
	if (!linked) {
		fprintf(stderr, "%s: cannot link\n", argv[0]);
		return 1;
	}

	refused = pet_linked_ast_n_refused(linked);
	if (refused != 0)
		fprintf(stderr, "%s: %d declaration(s) could not be linked\n",
			argv[0], refused);

	ASTContext &ctx = pet_linked_ast_context(linked);
	CompilerInstance *ci = configure(ctx);

	auto generator = CreateLLVMCodeGen(*ci, "linked", llvm_ctx);
	generator->Initialize(ctx);

	/* A record and its fields have to agree on which belongs to which
	 * before anything asks.
	 *
	 * A field knows the record it is part of and a record knows the
	 * fields it has, and importing brings the two across separately,
	 * so a link can leave a field pointing at a record that does not
	 * list it.  Nothing notices until something wants the field's
	 * position, at which point clang walks the record looking for it,
	 * does not find it, and stops far from here.
	 *
	 * Every record is looked at and not only the ones written at the
	 * outermost level, because most of them are not: a record inside
	 * a namespace, a record inside a record, a record a typedef
	 * stands for.
	 */
	if (getenv("PET_IR_RECORDS") || getenv("PET_IR_CHECK"))
		fprintf(stderr, "the linked unit's context is %p\n",
			(void *) &ctx);

	if (const char *want = getenv("PET_IR_RECORDS")) {
		std::set<Decl *> seen;

		show_records(ctx.getTranslationUnitDecl(), want, seen);
		fflush(stderr);
	}

	if (getenv("PET_IR_CHECK")) {
		long records = 0, wrong = 0;

		check_records(ctx.getTranslationUnitDecl(), records, wrong);
		for (auto &r : record_count) {
			if (r.second < 2)
				continue;
			wrong++;
			fprintf(stderr, "the unit holds %d finished records "
				"named %s\n", r.second, r.first.c_str());
		}
		fprintf(stderr, "checked %ld records, %ld are not what a "
			"record should be\n", records, wrong);
		fprintf(stderr, "the unit holds %ld functions with a body "
			"and %ld without\n", bodies, bodyless);
		fprintf(stderr, "and %ld delete expressions, %ld of them with "
			"nothing to call\n", deletes, empty_deletes);
		fprintf(stderr, "and %ld fields not in the definition of their "
			"record\n", split);
		fprintf(stderr, "and %ld initialisers naming a member of "
			"another record, out of %ld naming a base\n",
			stray_members, bases);
		fprintf(stderr, "and %ld member accesses, %ld of them reading "
			"a field the record does not hold\n", accesses,
			astray);
		fflush(stderr);
	}

	/* The unit is finished and handed over, both by ParseAST: it
	 * tells the source the serialised half was read from to offer
	 * what has to be emitted, it has the other half made -- the
	 * templates the unit uses are instantiated only when the unit is
	 * finished -- and it hands the whole over when there is nothing
	 * left to add.
	 */
	pet_linked_ast_finish(linked, *generator);

	llvm::Module *module = generator->GetModule();
	if (!module) {
		fprintf(stderr, "%s: no module came out of the link\n", argv[0]);
		return 1;
	}

	if (llvm::verifyModule(*module, &llvm::errs())) {
		fprintf(stderr, "%s: the module is not well formed\n", argv[0]);
		return 1;
	}

	llvm::raw_fd_ostream out(argv[1], ec);
	if (ec) {
		fprintf(stderr, "%s: cannot write %s: %s\n", argv[0], argv[1],
			ec.message().c_str());
		return 1;
	}
	module->print(out, NULL);

	printf("functions %ld globals %ld\n",
		(long) module->getFunctionList().size(),
		(long) module->global_size());

	return 0;
}
