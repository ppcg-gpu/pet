# ASTImporter removeDecl crash on a mixed C/C++ link -- recorded, not live

Reported from the literal-check tool that wraps the pet linker
(ast_link.cc / link_equivalence.cc at the versions named below). This is a
record of a crash we hit during development; it no longer reproduces on the
current trees, so what follows is the stack, the conditions, and the exact
commit pins, not a two-file reproducer. If the class is recognisable from
the stack alone, that may be enough; if not, the trees named by the pins
can be regenerated from this repository at those commits.

## The stack (gdb, all frames verified)

    #0 clang::DeclContext::removeDecl(clang::Decl*)            libclang-cpp 22.1
    #1 clang::ASTNodeImporter::ImportDeclContext(DeclContext*, bool)
    #2 clang::ASTImporter::ImportDefinition(Decl*)
    #3 link_importer::import_or_unify(clang::Decl*)            ast_link.cc
    #4 link_importer::ImportImpl(clang::Decl*)                 ast_link.cc
    #5 clang::ASTImporter::Import(clang::Decl*)
    #6 clang::ASTImporter::ImportContext(DeclContext*)
    #7 clang::ASTNodeImporter::ImportDeclContext(DeclContext*&, DeclContext*&)
    #8 clang::ASTNodeImporter::ImportDeclParts(...)

The crash is a SIGSEGV inside removeDecl during a forced ImportDeclContext,
reached from ImportDefinition -- the same window as the WasImportedHere
removal path: a declaration removed from a context that is itself mid-
import.

## Conditions

- A mixed link: ~57 units, C (.c.o) and C++ (.cpp.o), ggml and engine both.
- Unit order: shell-sorted, ggml units arriving BEFORE engine units.
- Link target: the lightest C++ unit (our lightest_unit rule; ggml-
  threading in that set).
- The same set with engine units first linked clean and produced a full
  report; only the arrival order flipped it.
- The trees were generated 2026-08-20 02:02 by clang 22.1.5 (-emit-ast via
  a compiler wrapper preserving all build flags).

## Pins

- pet linker in-tree copy at commit aa24e21cc (four mixed-link fixes
  landed there; the crash was observed with that exact code).
- clang 22.1.5 (Arch), libclang-cpp.so.22.1.
- The AST files were of sources at commit d1ba707f^..aa24e21cc.

## Why we did not chase it here

The residual 5k refusals on the mixed corpus are libstdc++ internals
(_Rb_tree::_M_erase and friends) arriving in an order the unification does
not join, and our immediate work moved to per-width kernel bodies, which
need only single-unit links. The order-dependence itself is the finding we
consider yours: import order should not decide between a clean link and a
crash, and the removeDecl window is where it did.
