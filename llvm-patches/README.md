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
    patch -p1 < .../ThirdParty/pet/llvm-patches/0002-source-manager-total-order.patch
    patch -p1 < .../ThirdParty/pet/llvm-patches/0003-structural-equivalence-across-languages.patch
    patch -p1 < .../ThirdParty/pet/llvm-patches/0004-import-float16-as-float16.patch

## 0004-import-float16-as-float16.patch

`BuiltinTypes.def` lists the builtin `Float16` as naming the singleton
`HalfTy`:

    FLOATING_TYPE(Float16, HalfTy)

`_Float16` and `__fp16` are two types.  `ASTContext` holds a `Float16Ty`
of its own and initialises it as `BuiltinType::Float16`, so the two are
kept apart everywhere else; it is this one table entry that conflates
them.  `VisitBuiltinType` imports builtin types by expanding that table,
so an imported `_Float16` comes back as `__fp16`.

That alone is only a spelling.  What makes it a refusal is what the
target already holds.  Where a unit of the target's own declares the
same thing, it is `_Float16` there, having been read rather than
imported; the arriving declaration is `__fp16`; the two are weighed
against each other and are not the same type.  The typedef is refused,
and every declaration that mentions it goes with it.

The patch answers a floating type with the singleton named after its
`Id` rather than with the one the table gives.  Seven of the eight
floating entries already name `Id##Ty` -- `Half` is `HalfTy`, `Float` is
`FloatTy`, and so on -- so this changes the `Float16` case and no other.
`BuiltinTypes.def` is left alone: `HalfTy` may be wanted there by the
other places that expand it.

### What it was measured against

`llvm-patches/../../../scratch/ppcg/repro-float16` holds the case: two
units include a header declaring

    typedef _Float16 v32 __attribute__((__vector_size__(64), __aligned__(64)));

with a `static __inline__` function returning it, the way `immintrin.h`
is written, and both are linked into a third unit that never mentions
the type.

Three control arms say what the case is about, and each of them links
cleanly: the same shape written with `float`, the same three units with
the target written in C, and the same three units with a carrier first
rather than the target.  The last of these is what identifies the
condition -- the target of a link is its first unit, so with a carrier
first the type is present natively and everything is weighed against
that, while with the target first every copy arrives through the import.

The refusals grow by four for each carrier added -- 0, 4, 8 -- which is
the shape of one target copy being compared against each arriving one.
Writing the carriers with `__fp16` instead makes it link, which closes
the loop from the other side: the mismatch is exactly the source's
`_Float16` against the target's imported `__fp16`.

Over llama-dspark, emitted with the whole SIMD group off, this is the
whole of what 52 units refuse.  `immintrin.h` is included by
`ggml-cpu-impl.h` under `#if defined(__SSE__)`, which is baseline on
x86-64 and no `GGML_*` option removes, so the AVX-512 FP16 declarations
are in every C++ unit of `ggml-cpu` whether or not the instructions may
be used.

### Open

Why a target written in C takes a different path is not explained.  It
does not refuse, and it resolves more calls rather than fewer -- two
against the C++ target's one -- so it is not doing less.  The relaxation
at `ASTContext.cpp:10538`, which treats `Float16` and `Half` as
compatible, is guarded by `getLangOpts().OpenCL` and is not the answer.

## 0003-structural-equivalence-across-languages.patch

A record described by a header is read into a `RecordDecl` by a unit
read as C and into a `CXXRecordDecl` by a unit read as C++, and 0001
makes the linked context keep the C++ one.  The comparison that decides
whether two declarations are the same entity then answers no on the kind
of declaration alone, before it has looked at anything, and every C
function whose arguments mention a struct becomes two entities of one
name -- 652 of them over 49 units of an engine.

The two are compared as records instead, which is what the comparison
would do for either of them, so two structs that really differ still
come out different.  This is the same relaxation `link_equivalence.cc`
already carries for the comparison `pet` does itself, moved to the one
clang does for the importer, with its reason unchanged: that is the
whole of the difference between comparing two units of one language,
which is what clang is built for, and comparing two units of a program,
which may be written in both.

Alongside it, diagnostics behind `PET_DECL_TRACE`: what the comparison
of two functions decided, which pair of declarations the general
comparison found to differ, which pair of records is being weighed,
which member of the two the comparison stopped on, and which two kinds
of context it refused.  That is how the 652 were traced to their cause,
and the 29 that were left after them.

The same difference is asked about a third time, of the context a
record sits in.  A record written inside another -- an anonymous struct
inside an anonymous union, as glibc writes `__atomic_wide_counter` --
sits in a `RecordDecl` on the side read as C and in a `CXXRecordDecl` on
the side read as C++, and the walk up the contexts answers no on the
kind alone.  What that costs is the whole way out: `__atomic_wide_counter`
differs, so `__pthread_cond_s` differs, so `pthread_cond_t` differs, so
`ggml_threadpool` differs, so `ggml_barrier` is three functions of one
name rather than one.  The two kinds are taken for one there as well,
and the names of the contexts are still compared, so two records that
really sit in different places still come out different.

Only those two kinds, named one by one: a class template specialization
is a record too, and the comparison a few lines below asks the other
side to be a specialization as well.  Written as `isa<RecordDecl>` it
lets through a specialization weighed against a plain record and hands
that comparison a null -- `IsStructurallyEquivalent(..., D2=0x0)`, which
is where linking 51 units died.

## 0002-source-manager-total-order.patch

`SourceManager::isBeforeInTranslationUnit` walks up the include chains
of both locations looking for a root they share.  Two files that were
never part of one translation unit have none, and it ends in
`llvm_unreachable("Unsortable locations found")`.  A linked AST holds
the files of every unit in one manager, so that is not a corner case
there but the ordinary case: any two units at all.

The order is settled instead, by file ID, which is what the same
function already does a few lines earlier for two built-in buffers of
different files -- "we just claim that lower IDs come first".  What that
buys is a total order, and the same one every time the same files are
read the same way, which is what every caller of this already takes it
to be.  It is deliberately not claimed to be the order the units
arrived in: a file loaded from elsewhere is given a negative ID and
later ones smaller ones still, so by file ID the last unit read sorts
first.

Found by the declaration printer, which weighs the location of an
attribute against the location of the record it is written on to decide
which side to print it: over 49 units those two are routinely in
different units.  It will not stay found there -- pet orders statements
by where they were written, and a scop built out of several units asks
this of every pair.

Going over every function of 49 units, this took the walk from 1867
functions to 35592.  With it applied, linking the 51 units the engine
acceptance was taken from gives 30393 functions and 4665 globals with
nothing refused, as it did before.

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
