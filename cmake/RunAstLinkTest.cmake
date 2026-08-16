# Drive the AST linking test.
#
# Serialises the translation units of the test corpus, links them and
# compares the report against the expected one.  The report deliberately
# mentions no paths, so the comparison is exact rather than fuzzy.
#
#   CLANG      the compiler used to serialise the units
#   LINKER     the pet_ast_link program
#   SRCDIR     directory holding the corpus
#   BINDIR     directory to place the serialised units and the output in
#   EXPECTED   file holding the expected report

foreach(required CLANG LINKER SRCDIR BINDIR EXPECTED)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "RunAstLinkTest: ${required} is required")
    endif()
endforeach()

file(MAKE_DIRECTORY "${BINDIR}")

# total.c is linked first, so that the calls it makes to functions defined
# in the other two units are the ones being resolved.
set(units total scale sum)
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
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "linking failed with ${result}")
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
