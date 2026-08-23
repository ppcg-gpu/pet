# Verify that the map goes over every function without a worker dying.
#
# pet_linked_ast_map hands the entries to forked children.  A child that
# dies takes its share of the map with it, and the run says so and fails.
# This is the test for the going over itself rather than for what it
# found: it asks that the map comes back whole.
#
#   EMITTER    the pet_emit_ast program
#   EXTRACTOR  the pet_linked_scop program
#   SRCDIR     directory holding the corpus
#   BINDIR     directory to place the serialised units and the output in
#   UNITS      '|' separated base names with extensions, in link order

foreach(required EMITTER EXTRACTOR SRCDIR BINDIR UNITS)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "RunMapCompletesTest: ${required} is required")
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
    COMMAND "${EXTRACTOR}" --autodetect --map ${asts}
    OUTPUT_FILE "${BINDIR}/map.txt"
    ERROR_FILE "${BINDIR}/diagnostics.txt"
    RESULT_VARIABLE result
)
file(READ "${BINDIR}/diagnostics.txt" diagnostics)

# First, that there was a map to come back from.  A corpus that stopped
# offering functions would leave this passing on a question it never
# asked, which is how a test quietly stops being one.  The headers behind
# two lines of C++ offer hundreds, so the floor is set well under what
# any version of them gives and still far above nothing.
file(STRINGS "${BINDIR}/map.txt" lines)
list(LENGTH lines n_lines)
if(n_lines LESS 100)
    message(FATAL_ERROR
        "the map holds ${n_lines} function(s); the corpus is meant to "
        "reach the whole of a stream header.\n"
        "diagnostics:\n${diagnostics}")
endif()

if(diagnostics MATCHES "died with signal")
    message(FATAL_ERROR
        "a part of the map died, so the map is not whole.\n"
        "${diagnostics}")
endif()

if(NOT result EQUAL 0)
    message(FATAL_ERROR
        "the map failed with ${result}.\n${diagnostics}")
endif()
