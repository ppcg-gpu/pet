cmake_minimum_required(VERSION 3.15)

project(pet LANGUAGES C CXX)

option(PET_ENABLE_DEBUG_HOOKS "Enable debug hooks that pause on crash for debugger attachment" OFF)
option(PET_FORCE_LLVM_VERSION "Force a specific LLVM version to be used (e.g. 15)" "")

# Add custom modules path
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Include CMake modules
include(ExternalProject)
include(GNUInstallDirs)
include(CheckCXXSourceCompiles)
include(CheckIncludeFileCXX)
include(CheckSymbolExists)
include(CMakePackageConfigHelpers)

# Find dependencies using our custom modules
# Pass the forced LLVM version to the FindLLVMClang module if specified
if(PET_FORCE_LLVM_VERSION)
  set(LLVM_VERSION ${PET_FORCE_LLVM_VERSION})
  message(STATUS "Enforcing LLVM version: ${PET_FORCE_LLVM_VERSION}")
endif()

# Find dependencies using our custom modules
find_package(GMP REQUIRED)
find_package(LLVMClang REQUIRED)
find_package(LibYAML REQUIRED)

# Set up include directories and definitions for LLVM/Clang
include_directories(${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})
link_directories(${LLVM_LIBRARY_DIRS})

ExternalProject_Add(isl
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/isl
    CONFIGURE_COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/isl/configure --prefix=<INSTALL_DIR>
    BUILD_COMMAND make
    INSTALL_COMMAND make install
    BUILD_BYPRODUCTS <INSTALL_DIR>/lib/libisl.a
)

# Get the install directory property and set paths correctly
ExternalProject_Get_Property(isl install_dir)
set(ISL_INCLUDE_DIRS ${install_dir}/include)
include_directories(${ISL_INCLUDE_DIRS})

# Set the correct path to the ISL library
set(ISL_LIBRARIES ${install_dir}/lib/libisl.a)
set(ISL_DEPENDS isl)

set(HAVE_ISL_BUILDDIR TRUE)
set(ISL_SRCDIR ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/isl)
set(ISL_BUILDDIR ${CMAKE_CURRENT_BINARY_DIR}/isl-prefix/src/isl-build)

set(EXTRA_BIN_PROGRAMS pet)
set(EXTRA_NOINST_PROGRAMS pet_scop_cmp)

# Include our custom Clang feature detection module
include(CheckClangFeatures)

# Check for various Clang features and generate config.h
check_clang_features()

# Set package information for config.h
set(PACKAGE "pet")
set(PACKAGE_BUGREPORT "isl-development@googlegroups.com")
set(PACKAGE_NAME "pet")
set(PACKAGE_STRING "pet ${PROJECT_VERSION}")
set(PACKAGE_TARNAME "pet")
set(PACKAGE_URL "")
set(PACKAGE_VERSION "${PROJECT_VERSION}")
set(VERSION "${PROJECT_VERSION}")

# Configure config.h from template
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/config.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/config.h
  @ONLY
)

# Configure all.c if needed for ISL bindings
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/all.c.in
  ${CMAKE_CURRENT_BINARY_DIR}/all.c
  @ONLY
)

# Optional isl.py generation
if(EXISTS "${ISL_BUILDDIR}/interface/extract_interface${CMAKE_EXECUTABLE_SUFFIX}")
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/isl.py
    COMMAND ${CMAKE_COMMAND} -E cat ${CMAKE_CURRENT_SOURCE_DIR}/interface/isl.py.top > ${CMAKE_CURRENT_BINARY_DIR}/isl.py
    COMMAND ${ISL_BUILDDIR}/interface/extract_interface${CMAKE_EXECUTABLE_SUFFIX} --language=python ${CLANG_CXXFLAGS} "${ISL_SRCDIR}/all.h" >> ${CMAKE_CURRENT_BINARY_DIR}/isl.py
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/interface/isl.py.top
    COMMENT "Generating isl.py"
    VERBATIM
  )
  add_custom_target(isl_py ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/isl.py)

  # Create dummy library that captures the dependencies on all headers
  add_library(dep STATIC ${CMAKE_CURRENT_BINARY_DIR}/all.c)
  target_include_directories(dep PRIVATE ${ISL_INCLUDE_DIRS})
  target_compile_options(dep PRIVATE ${CLANG_CXXFLAGS})
  add_dependencies(isl_py dep)
endif()

# Define library sources
set(PET_SOURCES
  aff.c
  array.c
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
  add_definitions(-DPET_ENABLE_DEBUG_HOOKS)
  list(APPEND PET_SOURCES debug_hooks.c)
endif()

# Create pet library
add_library(pet ${PET_SOURCES})
set_target_properties(pet PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
)

target_include_directories(pet
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${ISL_INCLUDE_DIRS}
)

if(ISL_DEPENDS)
  add_dependencies(pet ${ISL_DEPENDS})
endif()

# Apply LLVM/Clang flags
target_compile_options(pet PRIVATE ${CLANG_CXXFLAGS})
target_link_libraries(pet PUBLIC ${ISL_LIBRARIES} ${CLANG_LIBRARIES} ${LLVM_LIBRARIES} GMP::gmp)

# pet executable
add_executable(pet_exe
  dummy.cc
  emit.c
  main.c
)
set_target_properties(pet_exe PROPERTIES OUTPUT_NAME pet)
target_include_directories(pet_exe PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
  ${LIBYAML_INCLUDE_DIRS}
)
target_compile_options(pet_exe PRIVATE ${CLANG_CXXFLAGS})
target_link_libraries(pet_exe PRIVATE
  pet
  ${ISL_LIBRARIES}
  ${LIBYAML_LIBRARIES}
)
target_link_options(pet_exe PRIVATE ${CLANG_LDFLAGS})

# pet_scop_cmp executable
add_executable(pet_scop_cmp
  dummy.cc
  parse.c
  pet_scop_cmp.c
)
target_include_directories(pet_scop_cmp PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
  ${LIBYAML_INCLUDE_DIRS}
)
target_compile_options(pet_scop_cmp PRIVATE ${CLANG_CXXFLAGS})
target_link_libraries(pet_scop_cmp PRIVATE
  pet
  ${ISL_LIBRARIES}
  ${LIBYAML_LIBRARIES}
)
target_link_options(pet_scop_cmp PRIVATE ${CLANG_LDFLAGS})

# pet_codegen executable
add_executable(pet_codegen
  dummy.cc
  pet_codegen.c
)
target_include_directories(pet_codegen PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
)
target_compile_options(pet_codegen PRIVATE ${CLANG_CXXFLAGS})
target_link_libraries(pet_codegen PRIVATE pet ${ISL_LIBRARIES})
target_link_options(pet_codegen PRIVATE ${CLANG_LDFLAGS})

# pet_check_code executable
add_executable(pet_check_code
  dummy.cc
  pet_check_code.c
)
target_include_directories(pet_check_code PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
)
target_compile_options(pet_check_code PRIVATE ${CLANG_CXXFLAGS})
target_link_libraries(pet_check_code PRIVATE pet ${ISL_LIBRARIES})
target_link_options(pet_check_code PRIVATE ${CLANG_LDFLAGS})

# pet_loopback executable
add_executable(pet_loopback
  dummy.cc
  pet_loopback.c
)
target_include_directories(pet_loopback PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
)
target_compile_options(pet_loopback PRIVATE ${CLANG_CXXFLAGS})
target_link_libraries(pet_loopback PRIVATE pet ${ISL_LIBRARIES})
target_link_options(pet_loopback PRIVATE ${CLANG_LDFLAGS})

# Configure test scripts
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/pet_test.sh.in
  ${CMAKE_CURRENT_BINARY_DIR}/pet_test.sh
  @ONLY
)
file(CHMOD ${CMAKE_CURRENT_BINARY_DIR}/pet_test.sh PERMISSIONS
  OWNER_READ OWNER_WRITE OWNER_EXECUTE
  GROUP_READ GROUP_EXECUTE
  WORLD_READ WORLD_EXECUTE
)

# Configure codegen test script
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/codegen_test.sh.in
  ${CMAKE_CURRENT_BINARY_DIR}/codegen_test.sh
  @ONLY
)
file(CHMOD ${CMAKE_CURRENT_BINARY_DIR}/codegen_test.sh PERMISSIONS
  OWNER_READ OWNER_WRITE OWNER_EXECUTE
  GROUP_READ GROUP_EXECUTE
  WORLD_READ WORLD_EXECUTE
)

# Installation
install(TARGETS pet
  EXPORT PETTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES include/pet.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(TARGETS pet_exe pet_scop_cmp
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(TARGETS pet_codegen pet_check_code pet_loopback
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Adding PET unit tests for ctest
enable_testing()

# Create a directory for tests if it doesn't exist yet
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test_outputs")

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

    # Set properties to preserve output on failure
    set_tests_properties(PET_Test_${test_group}_${test_name} PROPERTIES
        WILL_FAIL FALSE
        SKIP_RETURN_CODE 77
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
    # Get the filename without extension and directory name
    get_filename_component(test_name ${source_file} NAME_WE)
    get_filename_component(test_dir ${source_file} DIRECTORY)
    get_filename_component(dir_name ${test_dir} NAME)
    get_filename_component(ext ${source_file} EXT)

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

    # Output C file
    set(output_c_base "${CMAKE_CURRENT_BINARY_DIR}/test_outputs/${test_group}_${test_name}.test.c")

    # List of options for testing
    set(options_list
        ""
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
        if(options STREQUAL "")
            set(option_suffix "default")
        else()
            string(REPLACE " " "_" option_suffix "${options}")
            string(REPLACE "--" "" option_suffix "${option_suffix}")
        endif()

        set(variant_target "${test_target}_${option_suffix}")
        set(output_c "${output_c_base}.${option_suffix}")

        # Write a bash wrapper script to handle errors gracefully
        set(wrapper_script "${CMAKE_CURRENT_BINARY_DIR}/test_outputs/${test_group}_${test_name}_${option_suffix}_wrapper.sh")
        file(WRITE ${wrapper_script} "#!/bin/bash\n")
        file(APPEND ${wrapper_script} "set -e\n")
        file(APPEND ${wrapper_script} "cat $2 | $1 ${tree_option} ${options} > $3 || { echo \"Error generating code\"; exit 1; }\n")
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

        # Set properties to preserve output on failure
        set_tests_properties(Codegen_Test_${test_group}_${test_name}_${option_suffix} PROPERTIES
            WILL_FAIL FALSE
            SKIP_RETURN_CODE 77
        )

        # Create a custom target for manual running via build system
        add_custom_target(${variant_target}
            COMMAND ${CMAKE_CTEST_COMMAND} -R "^Codegen_Test_${test_group}_${test_name}_${option_suffix}$" -V
            COMMENT "Running codegen test for ${test_name} with options '${options}'"
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

set_property(GLOBAL PROPERTY TEST_NAMES "")  # Clear global test list

# Find standard PET tests with absolute paths
file(GLOB PET_TEST_FILES "${PROJECT_SOURCE_DIR}/tests/*.c")
foreach(test_file ${PET_TEST_FILES})
    add_pet_test(${test_file} "" "standard")
endforeach()

# Find autodetect tests with absolute paths
file(GLOB PET_AUTODETECT_TEST_FILES "${PROJECT_SOURCE_DIR}/tests/autodetect/*.c")
foreach(test_file ${PET_AUTODETECT_TEST_FILES})
    add_pet_test(${test_file} "--autodetect" "autodetect")
endforeach()

# Find encapsulate tests with absolute paths
file(GLOB PET_ENCAPSULATE_TEST_FILES "${PROJECT_SOURCE_DIR}/tests/encapsulate/*.c")
foreach(test_file ${PET_ENCAPSULATE_TEST_FILES})
    add_pet_test(${test_file} "--encapsulate-dynamic-control" "encapsulate")
endforeach()

# Find and add code generation tests with absolute paths
file(GLOB CODEGEN_ST_TEST_FILES
    "${PROJECT_SOURCE_DIR}/test_inputs/codegen/*.st"
    "${PROJECT_SOURCE_DIR}/test_inputs/codegen/cloog/*.st"
)
foreach(test_file ${CODEGEN_ST_TEST_FILES})
    add_codegen_test(${test_file})
endforeach()

file(GLOB CODEGEN_IN_TEST_FILES
    "${PROJECT_SOURCE_DIR}/test_inputs/codegen/*.in"
    "${PROJECT_SOURCE_DIR}/test_inputs/codegen/omega/*.in"
    "${PROJECT_SOURCE_DIR}/test_inputs/codegen/pldi2012/*.in"
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

# Target for running all tests
add_custom_target(all_tests
    COMMAND ${CMAKE_CTEST_COMMAND}
    COMMENT "Running all tests"
    VERBATIM
)
