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

#include <set>

#include <stdio.h>
#include <string.h>

#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclGroup.h>
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
	invocation->getTargetOpts().Triple =
			ctx.getTargetInfo().getTriple().str();

	ci = new CompilerInstance(invocation);
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
	if (getenv("PET_IR_TRACE")) { fprintf(stderr, "STEP configure\n"); fflush(stderr); }
	CompilerInstance *ci = configure(ctx);

	if (getenv("PET_IR_TRACE")) { fprintf(stderr, "STEP create\n"); fflush(stderr); }
	auto generator = CreateLLVMCodeGen(*ci, "linked", llvm_ctx);
	if (getenv("PET_IR_TRACE")) { fprintf(stderr, "STEP initialize\n"); fflush(stderr); }
	generator->Initialize(ctx);
	if (getenv("PET_IR_TRACE")) { fprintf(stderr, "STEP walk\n"); fflush(stderr); }

	/* Every declaration of the link, and not only the ones some entry
	 * point reaches, so that a body that was lost is missed here and
	 * not somewhere further along.
	 *
	 * They are walked one at a time rather than with a range for, so
	 * that a chain closing on itself is said out loud rather than
	 * followed until the stack is gone: a linked unit holds what
	 * several imports put there, and nothing has checked that what
	 * they left behind is a list.
	 */
	{
		TranslationUnitDecl *tu = ctx.getTranslationUnitDecl();
		std::set<Decl *> walked;
		long n = 0;

		if (getenv("PET_IR_TRACE")) {
			fprintf(stderr, "STEP begin\n");
			fflush(stderr);
		}
		DeclContext::decl_iterator it = tu->decls_begin();
		if (getenv("PET_IR_TRACE")) {
			fprintf(stderr, "STEP got begin\n");
			fflush(stderr);
		}
		for (; it != tu->decls_end(); ++it) {
			if (getenv("PET_IR_TRACE") && n < 5) {
				fprintf(stderr, "STEP iter %ld\n", n);
				fflush(stderr);
			}
			Decl *d = *it;
			if (getenv("PET_IR_TRACE") && n < 5) {
				fprintf(stderr, "STEP deref %ld -> %p\n", n,
					(void *) d);
				fflush(stderr);
			}

			if (!walked.insert(d).second) {
				fprintf(stderr, "the declarations of the link "
					"close on themselves after %ld\n", n);
				return 1;
			}
			if (getenv("PET_IR_TRACE")) {
				auto *nd = dyn_cast<NamedDecl>(d);
				fprintf(stderr, "GEN %ld %s %s\n", ++n,
					d->getDeclKindName(),
					nd ? nd->getNameAsString().c_str() : "?");
				fflush(stderr);
			}
			generator->HandleTopLevelDecl(DeclGroupRef(d));
		}
	}
	generator->HandleTranslationUnit(ctx);

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
