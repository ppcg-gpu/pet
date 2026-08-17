# Drive one AST linking test.
#
# Serialises the named translation units of a corpus with pet's own
# emitter, links them and compares the report against the expected one.
# The report mentions no paths, so the comparison is exact rather than
# fuzzy.
#
# The units are deliberately not serialised by running the compiler: they
# have to be read with the include paths, the macros and the predefines
# pet uses, or they describe a different program than the one pet sees.
#
#   EMITTER      the pet_emit_ast program
#   LINKER       the pet_ast_link program
#   SRCDIR       directory holding the corpus
#   BINDIR       directory to place the serialised units and the output in
#   SUFFIX       the extension of the units, ".c" when not given
#   SUMMARY_ONLY compare the counts only, not the listing before them
#   UNITS        '|' separated base names, in the order they are linked;
#                the first one is the unit the others are linked into
#   EXPECTED     file holding the expected report
#   EMIT_ARGS    '|' separated extra options for the emitter, such as -D
#   EXPECT_FAIL  set when the link is meant to be rejected

foreach(required EMITTER LINKER SRCDIR BINDIR UNITS EXPECTED)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "RunAstLinkTest: ${required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${BINDIR}")
file(MAKE_DIRECTORY "${BINDIR}")

string(REPLACE "|" ";" units "${UNITS}")
if(NOT DEFINED SUFFIX OR "${SUFFIX}" STREQUAL "")
    set(SUFFIX ".c")
endif()
set(emit_args "")
if(DEFINED EMIT_ARGS AND NOT "${EMIT_ARGS}" STREQUAL "")
    string(REPLACE "|" ";" emit_args "${EMIT_ARGS}")
endif()

set(asts "")
foreach(unit ${units})
    execute_process(
        COMMAND "${EMITTER}" -I "${SRCDIR}" ${emit_args}
                "${SRCDIR}/${unit}${SUFFIX}" "${BINDIR}/${unit}.ast"
        RESULT_VARIABLE result
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "serialising ${unit}${SUFFIX} failed with ${result}")
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

# A corpus that reaches into the standard library is checked on the
# counts alone: which of its own functions libstdc++ calls is its
# business and changes with its version, while what the link made of it
# does not.
if(SUMMARY_ONLY)
    file(STRINGS "${BINDIR}/report.txt" lines)
    set(summary "")
    foreach(line ${lines})
        if(line MATCHES "^(units |refused [0-9]|calls |records )")
            string(APPEND summary "${line}\n")
        endif()
    endforeach()
    file(WRITE "${BINDIR}/summary.txt" "${summary}")
    set(report "${BINDIR}/summary.txt")
else()
    set(report "${BINDIR}/report.txt")
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND} -E compare_files "${EXPECTED}" "${report}"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    file(READ "${report}" got)
    file(READ "${EXPECTED}" want)
    message(FATAL_ERROR
        "the linked AST is not what was expected.\n"
        "--- expected ---\n${want}"
        "--- got ---\n${got}")
endif()
