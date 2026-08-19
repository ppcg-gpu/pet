# What these patches are

`pet` links translation units by importing the declarations of one into
the context of another with `clang::ASTImporter`.  The importer was
written for a different job -- putting two views of one program side by
side, as a debugger does -- and linking asks a few things of it that it
was never asked before.  Where the answer has to change, the change is
here rather than worked around in `pet`, because the decisions are taken
inside `ASTNodeImporter`, which clang does not expose: it is a file-local
class and the AST nodes grant it friendship by name.  A copy of it
outside clang cannot be made to compile -- it reaches into
`CXXRecordDecl::DefinitionData`, `APValue`, `Decl::IdentifierNamespace`
and `FriendDecl`'s trailing objects, all of them private -- and a copy
under the same name would collide with the one clang exports.

Applied to LLVM **22.1.5** (tag `llvmorg-22.1.5`, commit
`b12b5102827affa51b9080ee9b5cc20dfa33a793`, tree
`5ea218a153f4d2f815b8244eab3e4b4ba5e00e6c`):

    cd llvm-project
    patch -p1 < .../ThirdParty/pet/llvm-patches/0001-ast-importer-for-linking.patch

## 0001-ast-importer-for-linking.patch

Seven changes of substance, each one measured against the whole engine
(51 units of `llama-dspark`: `llama`, `ggml`, `ggml-base`, `ggml-cpu`).

**A record in a C++ context is a C++ record.**  The importer makes a
plain `RecordDecl` when the record it copies is a plain one, which is
what a C unit holds.  Everything that reads a record back in a C++
context -- the code generator's `CheckAggExprForMemSetUse`,
`TagDecl::startDefinition` walking the chain -- takes the language of
the context for the kind of the record and casts unconditionally.  645
plain records (93 names) were being made in the linked C++ context; now
none are.

**A member of a C struct is public.**  C has nothing to say about access
and a C++ record insists that every member say something.  One rule,
`importedAccess`, applied at the seventeen places that ask.

**A record names each of its members once.**  Two units that both
declare a member give two declarations of the one member, and putting
the second in the record as well leaves a record that says the same
member twice -- which the table of virtual methods reads as two members
and stops on.  4958 such second additions; now none.

**A member belongs to the definition of its record, not to a mention of
it.**  A record is in a translation unit both as a mention and as a
definition, and a field imported into the mention is a field the layout
never sees.  38 of 255 fields were landing in a mention; now none.

**A friend is not among a record's declarations.**  A record holds its
friends in a list of their own, so asking where one sits among the
members asks after something that was never there.

**What a record is, is what the record it was copied from is.**  Setting
the bases and adding the members makes clang work all of that out again
from pieces that arrive one at a time, and a base whose own definition
has not arrived yet is not yet known to be empty.  Three empty bases
then take a byte each instead of none: `std::_Hashtable<unsigned int,
...>` came out 64 bytes where the ordinary build makes it 56, every
member after the bases moved by eight, and `_M_emplace_uniq` divided by
what it read as the bucket count -- `test-quantize-fns` died of a
division by zero before reaching `main`.  The bits are restored from the
record they were copied from once everything is in place.

**OpenMP.**  The importer had no case for any of it.  Added:
`CapturedDecl`, `CapturedStmt`, and the `barrier`, `single`, `parallel`
and `for` directives with the `num_threads`, `reduction` and `schedule`
clauses -- everything `ggml` uses.  Without them nine functions and
everything reaching them were refused, `ggml_graph_compute` among them.

Alongside these the patch carries diagnostics, each behind an
environment variable and silent otherwise: `AST_LINK_TRACE_CONFLICT`,
`AST_LINK_TRACE_UNSUPPORTED`, `PET_IMPORT_TRACE`, `PET_MAP_TRACE`,
`PET_FIELD_TRACE`, `PET_STMT_TRACE`.  They are how the above were found
and they are how the next one will be.

## What is not here, and why

Two assertions in `clang/lib/AST/ExprConstant.cpp` say that a compound
literal used as an lvalue cannot happen in C++.  A linked AST holds C
bodies in a context whose language is C++, so they can and do -- fifteen
of them in `ggml`, all from the SIMD intrinsic headers.  Nothing else
about the evaluation cares.  Both assertions are compiled out of a
released clang, so the link is done with one; a clang built with
assertions will stop there.

The end-of-unit work that a precompiled header leaves to its reader --
defining the tables of virtual methods, and so instantiating the virtual
methods only the table names -- is not patched either.  `pet` asks for it
by saying the unit is whole rather than a prefix (`whole_unit_action` in
`pet.cc`), which is what it is: nothing is going to include it.
