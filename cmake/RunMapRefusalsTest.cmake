# Verify that --map mode survives declarations the link refuses.
#
# Two units that define the same struct differently cannot become one
# entity: the importer refuses the struct and everything written in terms
# of it.  Asked for a map, the link must come back anyway -- the map says
# what it can find about the functions that did link, and a refusal is a
# thing to report, not a thing to die on.
#
#   EMITTER    the pet_emit_ast program
#   EXTRACTOR  the pet_linked_scop program
#   SRCDIR     directory holding the corpus
#   BINDIR     directory to place the serialised units and the output in
#   UNITS      '|' separated base names with extensions, in link order
#              e.g. "unit_a.c|unit_b.c"

foreach(required EMITTER EXTRACTOR SRCDIR BINDIR UNITS)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "RunMapRefusalsTest: ${required} is required")
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

# In --map mode a refused declaration must not make the process fail.
execute_process(
    COMMAND "${EXTRACTOR}" --autodetect --map ${asts}
    OUTPUT_FILE "${BINDIR}/map.txt"
    ERROR_FILE "${BINDIR}/diagnostics.txt"
    RESULT_VARIABLE result
)
file(READ "${BINDIR}/diagnostics.txt" diagnostics)

# First, that there was anything to survive.  A corpus that stopped
# producing refusals would leave this passing on a question it never
# asked, which is how a test quietly stops being one.
if(NOT diagnostics MATCHES "([0-9]+) declaration\\(s\\) could not be linked")
    message(FATAL_ERROR
        "the link refused nothing, so nothing was asked: the corpus is "
        "meant to hold two definitions of one struct.\n"
        "diagnostics:\n${diagnostics}")
endif()
set(n_refused "${CMAKE_MATCH_1}")

if(NOT result EQUAL 0)
    message(FATAL_ERROR
        "--map failed with ${result}; it must survive the ${n_refused} "
        "declaration(s) it refused, not die on them.\n${diagnostics}")
endif()

# And that what was refused was named, since a link that swallows what it
# could not take says less than one that says which name it was.
if(NOT diagnostics MATCHES "thing: Record")
    message(FATAL_ERROR
        "--map survived but the refused struct is not named in the "
        "diagnostics; it was expected to be there.\n"
        "diagnostics:\n${diagnostics}")
endif()
