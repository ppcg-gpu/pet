# Drive one AST linking test.
#
# Serialises the named translation units of a corpus, links them and
# compares the report against the expected one.  The report mentions no
# paths, so the comparison is exact rather than fuzzy.
#
#   CLANG        the compiler used to serialise the units
#   LINKER       the pet_ast_link program
#   SRCDIR       directory holding the corpus
#   BINDIR       directory to place the serialised units and the output in
#   UNITS        '|' separated base names, in the order they are linked;
#                the first one is the unit the others are linked into
#   EXPECTED     file holding the expected report
#   EXPECT_FAIL  set when the link is meant to be rejected

foreach(required CLANG LINKER SRCDIR BINDIR UNITS EXPECTED)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "RunAstLinkTest: ${required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${BINDIR}")
file(MAKE_DIRECTORY "${BINDIR}")

string(REPLACE "|" ";" units "${UNITS}")

set(asts "")
foreach(unit ${units})
    execute_process(
        COMMAND "${CLANG}" -emit-ast -I "${SRCDIR}"
                -o "${BINDIR}/${unit}.ast" "${SRCDIR}/${unit}.c"
        RESULT_VARIABLE result
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "serialising ${unit}.c failed with ${result}")
    endif()
    list(APPEND asts "${BINDIR}/${unit}.ast")
endforeach()

execute_process(
    COMMAND "${LINKER}" --verbose ${asts}
    OUTPUT_FILE "${BINDIR}/report.txt"
    ERROR_FILE "${BINDIR}/diagnostics.txt"
    RESULT_VARIABLE result
)

if(EXPECT_FAIL)
    if(result EQUAL 0)
        message(FATAL_ERROR
            "the link was expected to be rejected but succeeded")
    endif()
elseif(NOT result EQUAL 0)
    file(READ "${BINDIR}/diagnostics.txt" diagnostics)
    message(FATAL_ERROR "linking failed with ${result}\n${diagnostics}")
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND} -E compare_files
            "${EXPECTED}" "${BINDIR}/report.txt"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    file(READ "${BINDIR}/report.txt" got)
    file(READ "${EXPECTED}" want)
    message(FATAL_ERROR
        "the linked AST is not what was expected.\n"
        "--- expected ---\n${want}"
        "--- got ---\n${got}")
endif()
