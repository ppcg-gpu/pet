# Extract a scop from a linked AST and compare it against a reference.
#
# This is what the linking is for: a loop whose body is written in another
# translation unit only becomes a scop describing the whole nest once that
# unit is part of the same AST.
#
#   EMITTER    the pet_emit_ast program
#   EXTRACTOR  the pet_linked_scop program
#   COMPARER   the pet_scop_cmp program
#   SRCDIR     directory holding the corpus
#   BINDIR     directory to place the serialised units and the output in
#   UNITS      '|' separated base names, in the order they are linked
#   FUNCTION   the function to extract from
#   EXPECTED   the reference scop, relative to the corpus

foreach(required EMITTER EXTRACTOR COMPARER SRCDIR BINDIR UNITS FUNCTION
                 EXPECTED)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "RunLinkedScopTest: ${required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${BINDIR}")
file(MAKE_DIRECTORY "${BINDIR}")

string(REPLACE "|" ";" units "${UNITS}")

set(asts "")
foreach(unit ${units})
    execute_process(
        COMMAND "${EMITTER}" -I "${SRCDIR}"
                "${SRCDIR}/${unit}.c" "${BINDIR}/${unit}.ast"
        RESULT_VARIABLE result
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "serialising ${unit}.c failed with ${result}")
    endif()
    list(APPEND asts "${BINDIR}/${unit}.ast")
endforeach()

execute_process(
    COMMAND "${EXTRACTOR}" --autodetect "--function=${FUNCTION}" ${asts}
    OUTPUT_FILE "${BINDIR}/extracted.scop"
    ERROR_FILE "${BINDIR}/diagnostics.txt"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    file(READ "${BINDIR}/diagnostics.txt" diagnostics)
    message(FATAL_ERROR "extracting failed with ${result}\n${diagnostics}")
endif()

# pet_scop_cmp compares what the scops mean rather than how they are
# written, which is what the rest of the pet tests rely on as well.
execute_process(
    COMMAND "${COMPARER}" "${BINDIR}/extracted.scop" "${SRCDIR}/${EXPECTED}"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR
        "the scop extracted from the linked AST differs from "
        "${EXPECTED}; it was left in ${BINDIR}/extracted.scop")
endif()
