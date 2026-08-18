# Build a program out of a linked AST and check that it is the program.
#
# What a link resolved can be counted, and the counts can be right about
# a link that is wrong: the corpus here reports every call resolved and
# nothing refused while holding two definitions of one name, which is
# not something that can be compiled.  So the linked AST is turned into
# LLVM IR, the IR into an object, the object into a program, and the
# program is run against the same one built the ordinary way.  Agreeing
# on the output is the whole of the check: it can only happen if every
# body came across, each call reached the one it was written for, and
# the types they pass between them line up.
#
#   EMITTER   pet_emit_ast
#   IR        pet_linked_ir
#   SRCDIR    directory holding the corpus
#   BINDIR    directory to work in
#   UNITS     '|' separated base names, the first being the one the rest
#             are linked into
#   DRIVER    a main() that calls the corpus, compiled with both
#   CC        the C compiler
#   LLC       the compiler that reads LLVM IR

foreach(required EMITTER IR SRCDIR BINDIR UNITS DRIVER CC LLC)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "RunLinkedIrTest: ${required} is required")
    endif()
endforeach()

string(REPLACE "|" ";" units "${UNITS}")

file(REMOVE_RECURSE "${BINDIR}")
file(MAKE_DIRECTORY "${BINDIR}")

function(run_or_fail what)
    execute_process(COMMAND ${ARGN}
        OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${what} failed with ${result}\n${out}${err}")
    endif()
endfunction()

set(sources "")
set(asts "")
foreach(unit ${units})
    run_or_fail("serialising ${unit}"
        "${EMITTER}" "${SRCDIR}/${unit}.c" "${BINDIR}/${unit}.ast")
    list(APPEND asts "${BINDIR}/${unit}.ast")
    list(APPEND sources "${SRCDIR}/${unit}.c")
endforeach()

run_or_fail("generating IR from the link"
    "${IR}" "${BINDIR}/linked.ll" ${asts})

run_or_fail("assembling the IR"
    "${LLC}" -O1 -c "${BINDIR}/linked.ll" -o "${BINDIR}/linked.o")

run_or_fail("building the program from the link"
    "${CC}" -O1 "-I${SRCDIR}" -o "${BINDIR}/from_link"
    "${BINDIR}/linked.o" "${DRIVER}")

run_or_fail("building the program the ordinary way"
    "${CC}" -O1 "-I${SRCDIR}" -o "${BINDIR}/ordinary"
    ${sources} "${DRIVER}")

execute_process(COMMAND "${BINDIR}/ordinary"
    OUTPUT_VARIABLE expected RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the ordinary program exited with ${result}")
endif()

execute_process(COMMAND "${BINDIR}/from_link"
    OUTPUT_VARIABLE got RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the program from the link exited with ${result}")
endif()

if(NOT expected STREQUAL got)
    message(FATAL_ERROR
        "the program built from the link computes something else.\n"
        "--- the ordinary program ---\n${expected}"
        "--- the one from the link ---\n${got}")
endif()
