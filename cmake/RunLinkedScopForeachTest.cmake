# Verify that --foreach finds every scop in a linked AST.
#
# With --foreach the tool must enumerate every scop it can find -- not
# just the first, not just what happens to be the largest.  The test
# counts how many scops were emitted and checks the output is valid
# multi-document YAML.
#
#   EMITTER    the pet_emit_ast program
#   EXTRACTOR  the pet_linked_scop program
#   SRCDIR     directory holding the corpus
#   BINDIR     directory to place the serialised units and the output in
#   UNITS      '|' separated base names with extensions, in link order
#              e.g. "unit_a.c|unit_b.c"

foreach(required EMITTER EXTRACTOR SRCDIR BINDIR UNITS)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "RunLinkedScopForeachTest: ${required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${BINDIR}")
file(MAKE_DIRECTORY "${BINDIR}")

string(REPLACE "|" ";" units "${UNITS}")

set(asts "")
foreach(unit ${units})
    execute_process(
        COMMAND "${EMITTER}" -I "${SRCDIR}"
                "${SRCDIR}/${unit}" "${BINDIR}/${unit}.ast"
        RESULT_VARIABLE result
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "serialising ${unit} failed with ${result}")
    endif()
    list(APPEND asts "${BINDIR}/${unit}.ast")
endforeach()

execute_process(
    COMMAND "${EXTRACTOR}" --autodetect --foreach ${asts}
    OUTPUT_FILE "${BINDIR}/foreach.txt"
    ERROR_FILE "${BINDIR}/diagnostics.txt"
    RESULT_VARIABLE result
)

if(NOT result EQUAL 0)
    file(READ "${BINDIR}/diagnostics.txt" diagnostics)
    message(FATAL_ERROR "--foreach failed with ${result}\n${diagnostics}")
endif()

file(READ "${BINDIR}/foreach.txt" output)

# First, that there was anything to foreach over.  A corpus that
# stopped producing multiple scops would leave this passing on a
# question it never asked, which is how a test quietly stops being
# one.
string(REGEX MATCHALL "start:" start_lines "${output}")
list(LENGTH start_lines n_scops)
if(n_scops LESS 2)
    message(FATAL_ERROR
        "--foreach found only ${n_scops} scop(s); at least 2 are "
        "expected.  The corpus may have changed so that it no longer "
        "exercises what this test was written for.\n"
        "output:\n${output}")
endif()

# And that the output is valid multi-document YAML: every scop but
# the first must be preceded by a document separator.  A CMake regex
# anchors to the whole string and not to each line, so "^---$" matches
# only an output that is nothing but a separator; the separator is
# looked for as a line of its own instead.
string(REGEX MATCHALL "\n---\n" sep_lines "${output}")
list(LENGTH sep_lines n_seps)
math(EXPR min_seps "${n_scops} - 1")
if(n_seps LESS min_seps)
    message(FATAL_ERROR
        "multi-document YAML separator mismatch: "
        "${n_scops} scop(s) but only ${n_seps} \"---\" separator(s) "
        "(need at least ${min_seps}).\n"
        "output:\n${output}")
endif()