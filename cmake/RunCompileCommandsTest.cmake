# Check that a file is parsed with the options its project builds it with.
#
# A file of any real project reaches for headers that only the project's
# own options say where to find, so without them it does not parse at
# all.  The options are in the compilation database the build system
# writes, and this checks that pet reads them from there.
#
#   PET       the pet program
#   SRCDIR    directory holding the corpus
#   BINDIR    directory to write the database and the output in
#   SOURCE    the file to parse, relative to the corpus
#
# The database is written here rather than kept in the corpus because it
# names absolute paths, which are only known once this runs.

foreach(required PET SRCDIR BINDIR SOURCE)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "RunCompileCommandsTest: ${required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${BINDIR}")
file(MAKE_DIRECTORY "${BINDIR}")

file(WRITE "${BINDIR}/compile_commands.json"
"[{\"directory\": \"${BINDIR}\",
   \"file\": \"${SRCDIR}/${SOURCE}\",
   \"command\": \"cc -I${SRCDIR}/include -c ${SRCDIR}/${SOURCE}\"}]
")

# Without the database first, to make sure the corpus really does need
# it.  A source that cannot be read is a failure, and says so.
execute_process(
    COMMAND "${PET}" "${SRCDIR}/${SOURCE}"
    OUTPUT_VARIABLE without ERROR_QUIET
    RESULT_VARIABLE result
)
if(without MATCHES "schedule:")
    message(FATAL_ERROR
        "${SOURCE} parses without the options of its project, so this "
        "says nothing about whether they were read")
endif()
if(result EQUAL 0)
    message(FATAL_ERROR
        "${SOURCE} could not be read, and pet reported success anyway")
endif()

execute_process(
    COMMAND "${PET}" "--compile-commands=${BINDIR}" "${SRCDIR}/${SOURCE}"
    OUTPUT_VARIABLE scop
    ERROR_VARIABLE err
    RESULT_VARIABLE result
)
file(WRITE "${BINDIR}/scop.yaml" "${scop}")
if(NOT result EQUAL 0)
    message(FATAL_ERROR
        "${SOURCE} was read with the options of its project, and pet "
        "reported failure anyway\n${err}")
endif()
if(NOT scop MATCHES "schedule:")
    message(FATAL_ERROR
        "${SOURCE} does not parse with the options of its project\n${err}")
endif()

# The scop has to be the one the options ask for: the header defines
# what the loop multiplies by, so a scop extracted without it would
# either not exist or say something else.
if(NOT scop MATCHES "3")
    message(FATAL_ERROR
        "the scop does not use what the header defines\n${scop}")
endif()
