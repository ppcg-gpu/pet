# Printing the original code of every scop gives the file back whole.
#
# The tool copies the text around each scop and prints the original text
# of the scop itself, so its output must equal its input byte for byte.
# An offset that is off by one, a region read from the wrong file, or a
# file that could not be opened all come out as a difference.
#
#   TOOL    the pet_print_original program
#   SOURCE  the C file to run it on
#   BINDIR  directory to place the output in

foreach(required TOOL SOURCE BINDIR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "RunPrintOriginalTest: ${required} is required")
    endif()
endforeach()

file(MAKE_DIRECTORY "${BINDIR}")
get_filename_component(name "${SOURCE}" NAME)
set(output "${BINDIR}/${name}.printed")

execute_process(
    COMMAND "${TOOL}" "${SOURCE}"
    OUTPUT_FILE "${output}"
    ERROR_FILE "${BINDIR}/diagnostics.txt"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    file(READ "${BINDIR}/diagnostics.txt" diagnostics)
    message(FATAL_ERROR
        "printing the original code failed with ${result}\n${diagnostics}")
endif()

# First, that anything was printed at all.  An empty output compares
# equal to nothing and would leave this passing on a question it never
# asked, which is how a test quietly stops being one.
file(SIZE "${output}" printed_size)
if(printed_size EQUAL 0)
    message(FATAL_ERROR
        "nothing was printed; the source was expected to hold scops "
        "whose original text is printed back")
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND} -E compare_files "${SOURCE}" "${output}"
    RESULT_VARIABLE same
)
if(NOT same EQUAL 0)
    file(READ "${output}" printed)
    message(FATAL_ERROR
        "the printed code is not the source it was read from.\n"
        "printed:\n${printed}")
endif()
