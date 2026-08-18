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
	/* And the same for where the code came from, since whatever the
	 * generator has to say about a declaration is said against the
	 * locations that declaration carries.
	 */
	ci->setFileManager(&ctx.getSourceManager().getFileManager());
	ci->setSourceManager(&ctx.getSourceManager());

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
	CompilerInstance *ci = configure(ctx);

	auto generator = CreateLLVMCodeGen(*ci, "linked", llvm_ctx);
	generator->Initialize(ctx);

	/* Every declaration of the link, and not only the ones some entry
	 * point reaches, so that a body that was lost is missed here and
	 * not somewhere further along.
	 */
	for (Decl *d : ctx.getTranslationUnitDecl()->decls())
		generator->HandleTopLevelDecl(DeclGroupRef(d));
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
