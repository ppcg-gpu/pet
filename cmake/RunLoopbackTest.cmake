# Printing a scop back out gives the file with the scop reprinted.
#
# pet_loopback rewrites each scop it finds through the tree printer, so
# what comes out says whether every kind of tree in the scop can be
# printed.  A tree the printer has no working case for takes the whole
# run down with it, which is what a return of a call did.
#
#   TOOL      the pet_loopback program
#   SOURCE    the C file to run it on
#   EXPECTED  the output it must produce
#   BINDIR    directory to place the output in

foreach(required TOOL SOURCE EXPECTED BINDIR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "RunLoopbackTest: ${required} is required")
    endif()
endforeach()

file(MAKE_DIRECTORY "${BINDIR}")
get_filename_component(name "${SOURCE}" NAME)
set(output "${BINDIR}/${name}.loopback")

execute_process(
    COMMAND "${TOOL}" --autodetect "${SOURCE}"
    OUTPUT_FILE "${output}"
    ERROR_FILE "${BINDIR}/diagnostics.txt"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    file(READ "${BINDIR}/diagnostics.txt" diagnostics)
    message(FATAL_ERROR
        "printing the scop back out failed with ${result}\n${diagnostics}")
endif()

# First, that the thing this was written for is in the output at all.
# A corpus whose scop stopped holding a return would leave this passing
# on a question it never asked, which is how a test quietly stops being
# one.
file(READ "${output}" printed)
if(NOT printed MATCHES "return ")
    message(FATAL_ERROR
        "nothing was printed for the return; the corpus is meant to "
        "hold a scop that ends in one.\n"
        "printed:\n${printed}")
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND} -E compare_files "${EXPECTED}" "${output}"
    RESULT_VARIABLE same
)
if(NOT same EQUAL 0)
    message(FATAL_ERROR
        "the printed code is not what was expected.\n"
        "printed:\n${printed}")
endif()
