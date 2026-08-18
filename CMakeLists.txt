# file(CHMOD), used by the test framework below, needs 3.19.
cmake_minimum_required(VERSION 3.21)

project(pet VERSION 0.11.8 LANGUAGES C CXX)

option(PET_ENABLE_DEBUG_HOOKS "Enable debug hooks that pause on crash for debugger attachment" OFF)
set(PET_FORCE_LLVM_VERSION "" CACHE STRING "Force a specific LLVM version to be used (e.g. 15)")

# Add custom modules path
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)

# Establish BUILD_TESTING before the subprojects, so that turning testing
# off reaches all of them.
include(CTest)

# Keep the installed programs pointing at the libraries in the same prefix.
if(NOT DEFINED CMAKE_INSTALL_RPATH)
  set(CMAKE_INSTALL_RPATH "$ORIGIN;$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()

# The LLVM version is picked by the clang_feature_tests module, which is
# reached through isl and extract_interface below.
if(PET_FORCE_LLVM_VERSION)
  set(LLVM_VERSION ${PET_FORCE_LLVM_VERSION})
  message(STATUS "Enforcing LLVM version: ${PET_FORCE_LLVM_VERSION}")
endif()

find_package(LibYAML REQUIRED)

# gitversion.h, reported by pet --version
execute_process(
  COMMAND git describe --always --dirty
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE GIT_HEAD_VERSION
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
  RESULT_VARIABLE GIT_DESCRIBE_RESULT
)
if(NOT GIT_DESCRIBE_RESULT EQUAL 0 OR NOT GIT_HEAD_VERSION)
  set(GIT_HEAD_VERSION "pet-${PROJECT_VERSION}")
endif()
file(GENERATE
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gitversion.h
  CONTENT "#define GIT_HEAD_ID \"${GIT_HEAD_VERSION}\"\n"
)

# isl is built as part of this project.  It also brings in extract_interface
# and, with it, the clang_feature_tests target that provides clang_features.h
# and the Clang/LLVM include and link flags.
add_subdirectory(ThirdParty/isl)

# Configure config.h from template
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/config.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/config.h
  @ONLY
)

# Define library sources
set(PET_SOURCES
  aff.c
  array.c
  ast_link.cc
  clang.cc
  context.c
  expr.c
  expr_arg.c
  expr_plus.cc
  filter.c
  id.cc
  inlined_calls.cc
  inliner.cc
  isl_id_to_pet_expr.c
  killed_locals.cc
  loc.c
  nest.c
  options.c
  patch.c
  pet_expr_to_isl_pw_aff.c
  print.c
  tree.c
  tree2scop.c
  scan.cc
  scop.c
  scop_plus.cc
  skip.c
  substituter.cc
  summary.c
  value_bounds.c
  version.cc
  pet.cc
)

# Configure debug hooks
if(PET_ENABLE_DEBUG_HOOKS)
  list(APPEND PET_SOURCES debug_hooks.c)
endif()

# Create pet library.  The version is the ABI version that the libtool
# -version-info 10:8:0 of the autotools build produced, which is unrelated
# to the release version above.
add_library(pet SHARED ${PET_SOURCES})
set_target_properties(pet PROPERTIES
  VERSION 10.0.8
  SOVERSION 10
)

if(PET_ENABLE_DEBUG_HOOKS)
  target_compile_definitions(pet PRIVATE PET_ENABLE_DEBUG_HOOKS)
endif()

target_include_directories(pet
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(pet PUBLIC isl PRIVATE clang_feature_tests)

# The programs shipped with pet.  Their only C++ source is the library they
# link against, so the linker language has to be set explicitly.
function(add_pet_program name)
  add_executable(${name} ${ARGN})
  set_target_properties(${name} PROPERTIES LINKER_LANGUAGE CXX)
  target_include_directories(${name} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
  )
  target_link_libraries(${name} PRIVATE pet)
endfunction()

add_pet_program(pet_exe emit.c main.c)
set_target_properties(pet_exe PROPERTIES OUTPUT_NAME pet)
target_include_directories(pet_exe PRIVATE ${LIBYAML_INCLUDE_DIRS})
target_link_libraries(pet_exe PRIVATE ${LIBYAML_LIBRARIES})

add_pet_program(pet_scop_cmp parse.c pet_scop_cmp.c)
target_include_directories(pet_scop_cmp PRIVATE ${LIBYAML_INCLUDE_DIRS})
target_link_libraries(pet_scop_cmp PRIVATE ${LIBYAML_LIBRARIES})

add_pet_program(pet_emit_ast pet_emit_ast.c)
add_pet_program(pet_ast_link pet_ast_link.cc)
# emit.c holds the yaml writer, which is not part of the library.
# The only program here whose own source is C++ against clang, so it
# needs what clang_feature_tests carries rather than only what pet
# re-exports.
add_pet_program(pet_linked_ir pet_linked_ir.cc)
target_link_libraries(pet_linked_ir PRIVATE clang_feature_tests)
add_pet_program(pet_linked_scop pet_linked_scop.c emit.c)
target_include_directories(pet_linked_scop PRIVATE ${LIBYAML_INCLUDE_DIRS})
target_link_libraries(pet_linked_scop PRIVATE ${LIBYAML_LIBRARIES})
# It reads the Clang headers directly, which pet only links privately.
target_link_libraries(pet_ast_link PRIVATE clang_feature_tests)
add_pet_program(pet_codegen pet_codegen.c)
add_pet_program(pet_check_code pet_check_code.c)
add_pet_program(pet_loopback pet_loopback.c)

# Installation
install(TARGETS pet
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES include/pet.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Only the pet program itself was installed by the autotools build; the
# others were noinst_PROGRAMS, that is, test drivers.
install(TARGETS pet_exe
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# pkg-config description, as AX_CREATE_PKGCONFIG_INFO used to produce.
#
# Written during installation rather than during configuration, because
# cmake --install --prefix chooses the prefix at that point, and a file
# naming the configured one would send consumers to a different tree.
set(PET_PC ${CMAKE_CURRENT_BINARY_DIR}/pet.pc)
install(CODE "
    # Baked in here; only the prefix is left to the install script, which is
    # the one thing cmake --install --prefix can still change.
    set(PROJECT_VERSION \"${PROJECT_VERSION}\")
    set(dir_LIB \"${CMAKE_INSTALL_LIBDIR}\")
    set(dir_INCLUDE \"${CMAKE_INSTALL_INCLUDEDIR}\")

    set(PKGCONFIG_PREFIX \"\${CMAKE_INSTALL_PREFIX}\")
    foreach(d LIB INCLUDE)
        # An absolute install directory is not under the prefix, so it must
        # not be written relative to it.
        if(IS_ABSOLUTE \"\${dir_\${d}}\")
            set(PKGCONFIG_\${d}DIR \"\${dir_\${d}}\")
        else()
            set(PKGCONFIG_\${d}DIR \"\\\${prefix}/\${dir_\${d}}\")
        endif()
    endforeach()
    configure_file(\"${CMAKE_CURRENT_SOURCE_DIR}/cmake/pet.pc.in\" \"${PET_PC}\" @ONLY)
")
install(FILES ${PET_PC} DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# Adding PET unit tests for ctest
# Everything below is registered only when testing is on.
if(BUILD_TESTING)

# Create a directory for tests if it doesn't exist yet
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_outputs")

#--------------------------------------------------------------------
# AST linking
#--------------------------------------------------------------------

# Link the units of a corpus and check the report against "expected".
#
#   name      what the property being checked is called
#   corpus    directory under tests/ast_link
#   units     base names, in the order they are linked
#   expected  the expected report, relative to the corpus
#   ARGN      "EXPECT_FAIL" when the link is meant to be rejected, and
#             "EMIT_ARGS" followed by options for the emitter
function(add_ast_link_test name corpus units expected)
    set(expect_fail 0)
    set(emit_args "")
    set(collecting 0)
    foreach(arg ${ARGN})
        if(arg STREQUAL "EXPECT_FAIL")
            set(expect_fail 1)
            set(collecting 0)
        elseif(arg STREQUAL "EMIT_ARGS")
            set(collecting 1)
        elseif(collecting)
            list(APPEND emit_args ${arg})
        endif()
    endforeach()
    list(JOIN units "|" units_arg)
    list(JOIN emit_args "|" emit_args_arg)
    add_test(NAME PET_AstLink_${name}
        COMMAND ${CMAKE_COMMAND}
            -DEMITTER=$<TARGET_FILE:pet_emit_ast>
            -DLINKER=$<TARGET_FILE:pet_ast_link>
            -DSRCDIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/ast_link/${corpus}
            -DBINDIR=${CMAKE_CURRENT_BINARY_DIR}/test_outputs/ast_link/${name}
            "-DUNITS=${units_arg}"
            "-DEMIT_ARGS=${emit_args_arg}"
            -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/tests/ast_link/${corpus}/${expected}
            -DEXPECT_FAIL=${expect_fail}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunAstLinkTest.cmake
    )
endfunction()

# A call to a function defined in another unit reaches its definition, two
# static functions sharing a name stay distinct, and the struct described
# by the shared header becomes one type.
add_ast_link_test(merge merge "total;scale;sum" expected-all.txt)

# Leaving the units that hold the definitions out is not an error, any
# more than linking without an object file is: the calls simply stay
# unresolved.  This is what makes the test above assert something.
add_ast_link_test(incomplete merge "total" expected-total-only.txt)

# Units that disagree about a type and about the type of a function
# describe programs that cannot be made into one.  The declarations
# involved are refused by name and the link is rejected.
# Two units that instantiate one template and reach for different
# members of it.  Which members a unit materialised is not a property of
# the type, so the two are one type and the link has nothing to refuse.
add_test(NAME PET_AstLink_lazy
    COMMAND ${CMAKE_COMMAND}
        -DEMITTER=$<TARGET_FILE:pet_emit_ast>
        -DLINKER=$<TARGET_FILE:pet_ast_link>
        -DSRCDIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/ast_link/lazy
        -DBINDIR=${CMAKE_CURRENT_BINARY_DIR}/test_outputs/ast_link/lazy
        -DSUFFIX=.cpp
        -DSUMMARY_ONLY=ON
        "-DUNITS=uses_size|uses_begin"
        -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/tests/ast_link/lazy/expected.txt
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunAstLinkTest.cmake
)

add_ast_link_test(conflict conflict "incompatible_a;incompatible_b"
    expected.txt EXPECT_FAIL)

# The units are serialised by pet rather than by running the compiler,
# so that they are read with the options pet was given.  The same source
# describes a different program depending on a macro, and the linked AST
# has to show it.
add_ast_link_test(options_off options "conditional" expected-plain.txt)
add_ast_link_test(options_on options "conditional" expected-defined.txt
    EMIT_ARGS -D WITH_HELPER)

# Extract a scop from a linked corpus and compare it against a reference.
#
#   name      what the case is called
#   corpus    directory under tests/ast_link
#   units     base names, in the order they are linked
#   function  the function to extract from
#   expected  the reference scop, relative to the corpus
function(add_linked_scop_test name corpus units function expected)
    list(JOIN units "|" units_arg)
    add_test(NAME PET_LinkedScop_${name}
        COMMAND ${CMAKE_COMMAND}
            -DEMITTER=$<TARGET_FILE:pet_emit_ast>
            -DEXTRACTOR=$<TARGET_FILE:pet_linked_scop>
            -DCOMPARER=$<TARGET_FILE:pet_scop_cmp>
            -DSRCDIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/ast_link/${corpus}
            -DBINDIR=${CMAKE_CURRENT_BINARY_DIR}/test_outputs/linked_scop/${name}
            "-DUNITS=${units_arg}"
            -DFUNCTION=${function}
            -DEXPECTED=${expected}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunLinkedScopTest.cmake
    )
endfunction()

# What a project builds a file with, taken from the compilation database
# its build system writes.  Without those options a file of any real
# project does not parse at all.
add_test(NAME PET_CompileCommands
    COMMAND ${CMAKE_COMMAND}
        -DPET=$<TARGET_FILE:pet_exe>
        -DSRCDIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/compile_commands
        -DBINDIR=${CMAKE_CURRENT_BINARY_DIR}/test_outputs/compile_commands
        -DSOURCE=unit.c
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunCompileCommandsTest.cmake
)

# A blur over a two dimensional image whose row pass lives in its own
# translation unit.  Linked, the scop covers both loops and the statement
# is the assignment inside the row pass; the caller on its own only has
# the outer loop over an opaque call, and nothing to schedule inside it.
add_linked_scop_test(blur app_blur "blur;blur_row" blur linked.scop)
add_linked_scop_test(blur_unlinked app_blur "blur" blur alone.scop)

#--------------------------------------------------------------------
# PET Test Framework
#--------------------------------------------------------------------

# Function to add standard PET tests
function(add_pet_test source_file pet_options test_group)
    # Get the filename without extension and path
    get_filename_component(test_name ${source_file} NAME_WE)
    get_filename_component(test_dir ${source_file} DIRECTORY)

    # Output files
    set(output_scop "${CMAKE_CURRENT_BINARY_DIR}/test_outputs/${test_group}_${test_name}.test.scop")
    set(reference_scop "${CMAKE_CURRENT_SOURCE_DIR}/tests/${test_name}.scop")

    # For different test groups that might be in subdirectories, adjust the path
    if(NOT test_group STREQUAL "standard")
        set(reference_scop "${CMAKE_CURRENT_SOURCE_DIR}/tests/${test_group}/${test_name}.scop")
    endif()

    # Create test name with test group prefix to avoid conflicts
    set(test_target "pet_test_${test_group}_${test_name}")

    # Write a bash wrapper script to handle errors gracefully
    set(wrapper_script "${CMAKE_CURRENT_BINARY_DIR}/test_outputs/${test_group}_${test_name}_wrapper.sh")
    file(WRITE ${wrapper_script} "#!/bin/bash\n")
    file(APPEND ${wrapper_script} "set -e\n")
    file(APPEND ${wrapper_script} "echo \"Running pet on ${source_file} with output to ${output_scop}\"\n")
    file(APPEND ${wrapper_script} "$1 ${pet_options} $2 > $3 || { echo \"Error running pet on $2\"; exit 1; }\n")
    file(APPEND ${wrapper_script} "echo \"Comparing ${output_scop} with reference ${reference_scop}\"\n")
    file(APPEND ${wrapper_script} "$4 $3 $5 || { echo \"Error comparing SCOPs\"; exit 1; }\n")
    file(APPEND ${wrapper_script} "exit 0\n")

    # Make the script executable
    file(CHMOD ${wrapper_script} PERMISSIONS
         OWNER_READ OWNER_WRITE OWNER_EXECUTE
         GROUP_READ GROUP_EXECUTE
         WORLD_READ WORLD_EXECUTE)

    # Add test directly to CTest using the wrapper script
    add_test(
        NAME PET_Test_${test_group}_${test_name}
        COMMAND ${wrapper_script}
                $<TARGET_FILE:pet_exe>
                ${source_file}
                ${output_scop}
                $<TARGET_FILE:pet_scop_cmp>
                ${reference_scop}
    )

    # Create a custom target for manual running via build system
    add_custom_target(${test_target}
        COMMAND ${CMAKE_CTEST_COMMAND} -R "^PET_Test_${test_group}_${test_name}$" -V
        COMMENT "Running test for ${test_name}"
        VERBATIM
    )

    # Return the target name to the parent scope so it can be used in dependencies
    set(${test_name}_${test_group}_TARGET ${test_target} PARENT_SCOPE)
endfunction()

#--------------------------------------------------------------------
# Codegen Test Framework
#--------------------------------------------------------------------

# Function to add code generation tests
function(add_codegen_test source_file)
    # NAME_WE and EXT split at the *first* dot, which mangles inputs such as
    # p.delft-0.in: the name becomes "p" and the extension ".delft-0.in", so
    # the file is no longer recognised as a schedule with options.  Split at
    # the last dot instead.
    get_filename_component(test_name ${source_file} NAME_WLE)
    get_filename_component(test_dir ${source_file} DIRECTORY)
    get_filename_component(dir_name ${test_dir} NAME)
    get_filename_component(ext ${source_file} LAST_EXT)

    # Create a unique test group identifier based on the directory path
    if(dir_name STREQUAL "codegen")
        set(test_group "codegen")
    elseif(dir_name STREQUAL "cloog")
        set(test_group "cloog")
    elseif(dir_name STREQUAL "omega")
        set(test_group "omega")
    elseif(dir_name STREQUAL "pldi2012")
        set(test_group "pldi")
    else()
        # Use a hash of the directory path to ensure uniqueness
        string(MD5 dir_hash "${test_dir}")
        string(SUBSTRING ${dir_hash} 0 6 test_group)
    endif()

    # A schedule tree and a schedule with options can share a base name,
    # for instance unroll10.st and unroll10.in, so the extension has to be
    # part of the group too or their targets and tests collide.
    string(SUBSTRING ${ext} 1 -1 ext_name)
    set(test_group "${test_group}_${ext_name}")

    # Output C file
    set(output_c_base "${CMAKE_CURRENT_BINARY_DIR}/test_outputs/${test_group}_${test_name}.test.c")

    # List of options for testing.  An empty element vanishes when a CMake
    # list is expanded, so the run without options is named rather than
    # spelled as an empty string and translated back below.
    set(options_list
        "default"
        "--separate"
        "--atomic"
        "--isl-no-ast-build-atomic-upper-bound"
    )

    # If the file is .in, add --read-options to the list
    if(ext STREQUAL ".in")
        list(APPEND options_list "--read-options")
    endif()

    # Special option for .st files
    set(tree_option "")
    if(ext STREQUAL ".st")
        set(tree_option "--tree")
    endif()

    # Create a general target for all options of this file
    set(test_target "codegen_test_${test_group}_${test_name}")
    add_custom_target(${test_target})

    # For each option, create a separate test
    foreach(options ${options_list})
        # Create a unique name for the test variant
        if(options STREQUAL "default")
            set(option_suffix "default")
            set(pet_options "")
        else()
            string(REPLACE " " "_" option_suffix "${options}")
            string(REPLACE "--" "" option_suffix "${option_suffix}")
            set(pet_options "${options}")
        endif()

        set(variant_target "${test_target}_${option_suffix}")
        set(output_c "${output_c_base}.${option_suffix}")

        # Write a bash wrapper script to handle errors gracefully
        set(wrapper_script "${CMAKE_CURRENT_BINARY_DIR}/test_outputs/${test_group}_${test_name}_${option_suffix}_wrapper.sh")
        file(WRITE ${wrapper_script} "#!/bin/bash\n")
        file(APPEND ${wrapper_script} "set -e\n")
        file(APPEND ${wrapper_script} "cat $2 | $1 ${tree_option} ${pet_options} > $3 || { echo \"Error generating code\"; exit 1; }\n")
        file(APPEND ${wrapper_script} "$4 ${tree_option} $2 $3 || { echo \"Error checking code\"; exit 1; }\n")
        file(APPEND ${wrapper_script} "exit 0\n")

        # Make the script executable
        file(CHMOD ${wrapper_script} PERMISSIONS
             OWNER_READ OWNER_WRITE OWNER_EXECUTE
             GROUP_READ GROUP_EXECUTE
             WORLD_READ WORLD_EXECUTE)

        # Add test directly to CTest using the wrapper script
        add_test(
            NAME Codegen_Test_${test_group}_${test_name}_${option_suffix}
            COMMAND ${wrapper_script}
                    $<TARGET_FILE:pet_codegen>
                    ${source_file}
                    ${output_c}
                    $<TARGET_FILE:pet_check_code>
        )

        # Create a custom target for manual running via build system
        add_custom_target(${variant_target}
            COMMAND ${CMAKE_CTEST_COMMAND} -R "^Codegen_Test_${test_group}_${test_name}_${option_suffix}$" -V
            COMMENT "Running codegen test for ${test_name} with options '${pet_options}'"
            VERBATIM
        )

        # Add dependency to the general test target
        add_dependencies(${test_target} ${variant_target})
    endforeach()

    # Return the target name to the parent scope
    set(${test_name}_${test_group}_TARGET ${test_target} PARENT_SCOPE)
endfunction()

#--------------------------------------------------------------------
# Find and add tests - IMPORTANT: Use ABSOLUTE paths for globbing
#--------------------------------------------------------------------

# Find standard PET tests with absolute paths
file(GLOB PET_TEST_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.c")
foreach(test_file ${PET_TEST_FILES})
    add_pet_test(${test_file} "" "standard")
endforeach()

# Find autodetect tests with absolute paths
file(GLOB PET_AUTODETECT_TEST_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/autodetect/*.c")
foreach(test_file ${PET_AUTODETECT_TEST_FILES})
    add_pet_test(${test_file} "--autodetect" "autodetect")
endforeach()

# Find encapsulate tests with absolute paths
file(GLOB PET_ENCAPSULATE_TEST_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/encapsulate/*.c")
foreach(test_file ${PET_ENCAPSULATE_TEST_FILES})
    add_pet_test(${test_file} "--encapsulate-dynamic-control" "encapsulate")
endforeach()

# Find and add code generation tests with absolute paths
# The codegen inputs live in isl; the autotools driver read them from
# @ISL_SRCDIR@ for the same reason.
set(ISL_TEST_INPUTS ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/isl/test_inputs)

file(GLOB CODEGEN_ST_TEST_FILES CONFIGURE_DEPENDS
    "${ISL_TEST_INPUTS}/codegen/*.st"
    "${ISL_TEST_INPUTS}/codegen/cloog/*.st"
)
foreach(test_file ${CODEGEN_ST_TEST_FILES})
    add_codegen_test(${test_file})
endforeach()

file(GLOB CODEGEN_IN_TEST_FILES CONFIGURE_DEPENDS
    "${ISL_TEST_INPUTS}/codegen/*.in"
    "${ISL_TEST_INPUTS}/codegen/omega/*.in"
    "${ISL_TEST_INPUTS}/codegen/pldi2012/*.in"
)
foreach(test_file ${CODEGEN_IN_TEST_FILES})
    add_codegen_test(${test_file})
endforeach()

#--------------------------------------------------------------------
# Summarizing targets for convenience
#--------------------------------------------------------------------

# General target for running all PET tests
add_custom_target(pet_tests
    COMMAND ${CMAKE_CTEST_COMMAND} -R "^PET_Test_"
    COMMENT "Running all PET tests"
    VERBATIM
)

# General target for running different PET test groups
add_custom_target(pet_standard_tests
    COMMAND ${CMAKE_CTEST_COMMAND} -R "^PET_Test_standard_"
    COMMENT "Running standard PET tests"
    VERBATIM
)

add_custom_target(pet_autodetect_tests
    COMMAND ${CMAKE_CTEST_COMMAND} -R "^PET_Test_autodetect_"
    COMMENT "Running autodetect PET tests"
    VERBATIM
)

add_custom_target(pet_encapsulate_tests
    COMMAND ${CMAKE_CTEST_COMMAND} -R "^PET_Test_encapsulate_"
    COMMENT "Running encapsulate PET tests"
    VERBATIM
)

# General target for running all code generation tests
add_custom_target(codegen_tests
    COMMAND ${CMAKE_CTEST_COMMAND} -R "^Codegen_Test_"
    COMMENT "Running all codegen tests"
    VERBATIM
)

# General targets for different codegen test types
add_custom_target(codegen_st_tests
    COMMAND ${CMAKE_CTEST_COMMAND} -R "^Codegen_Test_.*_.*_default$"
    COMMENT "Running standard codegen tests"
    VERBATIM
)

add_custom_target(codegen_in_tests
    COMMAND ${CMAKE_CTEST_COMMAND} -R "^Codegen_Test_.*_.*_(separate|atomic|isl)"
    COMMENT "Running variant codegen tests"
    VERBATIM
)

endif()
