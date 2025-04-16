# FindLLVMClang.cmake
# Finds LLVM and Clang installations intelligently
#
# Input variables:
#   LLVM_VERSION - Specific LLVM version to use (e.g. "11.1.0")
#                  This can be set directly or via PET_FORCE_LLVM_VERSION option
#
# Output variables:
#   LLVM_FOUND - True if LLVM was found
#   LLVM_INCLUDE_DIRS - LLVM include directories
#   LLVM_DEFINITIONS - LLVM compiler definitions
#   LLVM_PACKAGE_VERSION - LLVM version
#   LLVM_DIR - Location of LLVM CMake modules
#   Clang_FOUND - True if Clang was found  
#   CLANG_INCLUDE_DIRS - Clang include directories
#   CLANG_VERSION - Clang version
#   CLANG_LINKING_WORKS - True if LLVM/Clang libraries can be successfully linked against

# Common search paths for LLVM tools and configurations
set(COMMON_LLVM_BIN_PATHS 
    "/usr/bin"
    "/usr/local/bin"
    "/opt/local/bin"
    "/opt/homebrew/bin"
)

# Function to generate common LLVM directory paths based on version
function(generate_llvm_search_paths version out_paths)
    # Extract major version for path generation
    string(REGEX MATCH "^([0-9]+)" major_version "${version}")
    
    set(paths "")
    
    # Standard paths based on version patterns
    list(APPEND paths
        # Full version paths
        "/usr/lib/llvm-${version}/lib/cmake"
        "/usr/lib/llvm-${version}/cmake"
        "/usr/local/opt/llvm@${version}/lib/cmake"
        "/opt/homebrew/opt/llvm@${version}/lib/cmake"
        "/usr/lib/cmake/llvm-${version}"
        "/usr/local/lib/cmake/llvm-${version}"
        
        # Major version paths
        "/usr/lib/llvm-${major_version}/lib/cmake"
        "/usr/lib/llvm-${major_version}/cmake"
        "/usr/local/opt/llvm@${major_version}/lib/cmake"
        "/opt/homebrew/opt/llvm@${major_version}/lib/cmake"
        "/usr/lib/cmake/llvm-${major_version}"
        "/usr/local/lib/cmake/llvm-${major_version}"
    )
    
    set(${out_paths} ${paths} PARENT_SCOPE)
endfunction()

# Function to find LLVM config executable for a specific version
function(find_llvm_config_for_full_version full_version out_path)
    # Extract major version number for fallback searches
    string(REGEX MATCH "^([0-9]+)" major_version "${full_version}")
    
    # Common locations for llvm-config
    set(search_paths ${COMMON_LLVM_BIN_PATHS})
    list(APPEND search_paths
        "/usr/lib/llvm-${major_version}/bin"
        "/usr/lib/llvm/${major_version}/bin"
        "/usr/lib/llvm-${full_version}/bin"
        "/usr/lib/llvm/${full_version}/bin"
    )
    
    # First try versioned llvm-config with full version
    find_program(LLVM_CONFIG_FULL llvm-config-${full_version} PATHS ${search_paths})
    
    if(LLVM_CONFIG_FULL)
        set(${out_path} ${LLVM_CONFIG_FULL} PARENT_SCOPE)
        return()
    endif()
    
    # Next try versioned llvm-config with major version
    find_program(LLVM_CONFIG_MAJOR llvm-config-${major_version} PATHS ${search_paths})
    
    if(LLVM_CONFIG_MAJOR)
        # Check if this actually matches our full version
        execute_process(
            COMMAND ${LLVM_CONFIG_MAJOR} --version
            OUTPUT_VARIABLE LLVM_FOUND_VERSION
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        
        # Compare with requested full version
        if("${LLVM_FOUND_VERSION}" VERSION_EQUAL "${full_version}")
            set(${out_path} ${LLVM_CONFIG_MAJOR} PARENT_SCOPE)
            return()
        endif()
    endif()
    
    # Finally try plain llvm-config and check its version
    find_program(LLVM_CONFIG_PLAIN llvm-config PATHS ${search_paths})
    
    if(LLVM_CONFIG_PLAIN)
        # Get version from llvm-config
        execute_process(
            COMMAND ${LLVM_CONFIG_PLAIN} --version
            OUTPUT_VARIABLE LLVM_FOUND_VERSION
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        
        # Compare with requested full version
        if("${LLVM_FOUND_VERSION}" VERSION_EQUAL "${full_version}")
            set(${out_path} ${LLVM_CONFIG_PLAIN} PARENT_SCOPE)
            return()
        endif()
    endif()
    
    # If we get here, we didn't find an llvm-config matching the full version
    set(${out_path} "" PARENT_SCOPE)
endfunction()

# Function to find highest available LLVM version
function(find_highest_llvm_version out_version out_config)
    # First try to find plain llvm-config
    find_program(LLVM_CONFIG_EXEC llvm-config PATHS ${COMMON_LLVM_BIN_PATHS})
    
    if(LLVM_CONFIG_EXEC)
        # Get version from llvm-config
        execute_process(
            COMMAND ${LLVM_CONFIG_EXEC} --version
            OUTPUT_VARIABLE LLVM_FOUND_VERSION
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        set(${out_version} ${LLVM_FOUND_VERSION} PARENT_SCOPE)
        set(${out_config} ${LLVM_CONFIG_EXEC} PARENT_SCOPE)
        return()
    endif()
    
    # If llvm-config not found, look for versioned variants
    set(highest_version "0")
    set(highest_config "")
    
    # Check common version range for major versions
    foreach(version RANGE 11 21)
        find_program(LLVM_CONFIG_${version} llvm-config-${version} PATHS ${COMMON_LLVM_BIN_PATHS})
        if(LLVM_CONFIG_${version})
            # Get the full version
            execute_process(
                COMMAND ${LLVM_CONFIG_${version}} --version
                OUTPUT_VARIABLE LLVM_VERSION_${version}
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
            
            if(${LLVM_VERSION_${version}} VERSION_GREATER ${highest_version})
                set(highest_version ${LLVM_VERSION_${version}})
                set(highest_config ${LLVM_CONFIG_${version}})
            endif()
        endif()
        unset(LLVM_CONFIG_${version} CACHE)
    endforeach()
    
    if(NOT ${highest_version} EQUAL "0")
        set(${out_version} ${highest_version} PARENT_SCOPE)
        set(${out_config} ${highest_config} PARENT_SCOPE)
    else()
        set(${out_version} "" PARENT_SCOPE)
        set(${out_config} "" PARENT_SCOPE)
    endif()
endfunction()

# Function to generate paths based on LLVM installation prefix
function(generate_prefix_paths prefix out_paths)
    set(prefix_paths
        "${prefix}/lib/cmake/llvm"
        "${prefix}/lib/cmake"
        "${prefix}/share/llvm/cmake"
        "${prefix}/lib/llvm/cmake"
    )
    set(${out_paths} ${prefix_paths} PARENT_SCOPE)
endfunction()

# Main LLVM/Clang detection logic
set(POTENTIAL_LLVM_PATHS "")

if(LLVM_VERSION)
    message(STATUS "Enforcing specific LLVM version: ${LLVM_VERSION}")
    # Find llvm-config for the specific full version
    find_llvm_config_for_full_version("${LLVM_VERSION}" LLVM_CONFIG_EXEC)
    
    if(NOT LLVM_CONFIG_EXEC)
        message(FATAL_ERROR "LLVM version ${LLVM_VERSION} was specified but could not be found. Please install LLVM ${LLVM_VERSION} or choose another version.")
    endif()
    
    # Get LLVM prefix and standard version-based paths
    execute_process(
        COMMAND ${LLVM_CONFIG_EXEC} --prefix
        OUTPUT_VARIABLE LLVM_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    
    # Generate paths based on version and prefix
    generate_llvm_search_paths(${LLVM_VERSION} VERSION_PATHS)
    generate_prefix_paths(${LLVM_PREFIX} PREFIX_PATHS)
    
    # Combine paths
    list(APPEND POTENTIAL_LLVM_PATHS ${PREFIX_PATHS} ${VERSION_PATHS})
    
else()
    # Auto-detect highest available version
    find_highest_llvm_version(DETECTED_LLVM_VERSION LLVM_CONFIG_EXEC)
    
    if(DETECTED_LLVM_VERSION)
        message(STATUS "Detected LLVM version: ${DETECTED_LLVM_VERSION}")
        
        # Generate paths based on detected version
        generate_llvm_search_paths(${DETECTED_LLVM_VERSION} VERSION_PATHS)
        list(APPEND POTENTIAL_LLVM_PATHS ${VERSION_PATHS})
        
        # Get LLVM prefix to add more potential paths
        if(LLVM_CONFIG_EXEC)
            execute_process(
                COMMAND ${LLVM_CONFIG_EXEC} --prefix
                OUTPUT_VARIABLE LLVM_PREFIX
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
            
            generate_prefix_paths(${LLVM_PREFIX} PREFIX_PATHS)
            list(APPEND POTENTIAL_LLVM_PATHS ${PREFIX_PATHS})
        endif()
    else()
        message(STATUS "No specific LLVM version detected, will try default paths")
    endif()
endif()

# First try with specific paths if available
if(POTENTIAL_LLVM_PATHS)
    find_package(LLVM CONFIG PATHS ${POTENTIAL_LLVM_PATHS} NO_DEFAULT_PATH)
    if(NOT LLVM_FOUND)
        message(STATUS "LLVM not found in version-specific paths, trying default locations")
    endif()
endif()

# If not found with specific paths, try default paths
if(NOT LLVM_FOUND)
    find_package(LLVM CONFIG)
endif()

# Handle LLVM detection result
if(LLVM_FOUND)
    message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}")
    
    # Verify version matches if a specific version was requested
    if(LLVM_VERSION)
        # Compare full versions with exact matching
        if(NOT "${LLVM_PACKAGE_VERSION}" VERSION_EQUAL "${LLVM_VERSION}")
            message(FATAL_ERROR "Found LLVM version ${LLVM_PACKAGE_VERSION} but version ${LLVM_VERSION} was requested. Please check your LLVM installation.")
        else()
            message(STATUS "Verified LLVM version ${LLVM_PACKAGE_VERSION} matches requested version ${LLVM_VERSION}")
        endif()
    endif()
    
    # Now find Clang using same paths as LLVM
    get_filename_component(LLVM_CMAKE_DIR "${LLVM_DIR}" DIRECTORY)
    find_package(Clang CONFIG REQUIRED HINTS "${LLVM_CMAKE_DIR}")

    if(Clang_FOUND)
        # Get Clang version if not already set by find_package
        if(NOT CLANG_VERSION OR CLANG_VERSION STREQUAL "")
            # Try to get version from Clang installation
            if(EXISTS "${Clang_DIR}/../../../include/clang/Basic/Version.h")
                file(STRINGS "${Clang_DIR}/../../../include/clang/Basic/Version.h" CLANG_VERSION_LINES
                     REGEX "^#define[ \t]+CLANG_VERSION[ \t]+.*")
                if(CLANG_VERSION_LINES)
                    string(REGEX REPLACE ".*CLANG_VERSION[ \t]+\"([0-9.]+).*\".*" "\\1" 
                           CLANG_VERSION "${CLANG_VERSION_LINES}")
                endif()
            endif()
            
            # If still not found, assume same as LLVM version
            if(NOT CLANG_VERSION OR CLANG_VERSION STREQUAL "")
                set(CLANG_VERSION "${LLVM_PACKAGE_VERSION}")
                message(STATUS "Could not determine Clang version, assuming same as LLVM: ${CLANG_VERSION}")
            endif()
        endif()
        
        message(STATUS "Found Clang ${CLANG_VERSION} at ${Clang_DIR}")
        
        # Strict verification that Clang version matches LLVM version
        if(NOT "${CLANG_VERSION}" VERSION_EQUAL "${LLVM_PACKAGE_VERSION}")
            message(FATAL_ERROR "Clang version (${CLANG_VERSION}) does not match LLVM version (${LLVM_PACKAGE_VERSION}). PET requires matching LLVM and Clang versions.")
        else()
            message(STATUS "Verified Clang version ${CLANG_VERSION} matches LLVM version ${LLVM_PACKAGE_VERSION}")
        endif()
    else()
        message(FATAL_ERROR "Found LLVM but could not find matching Clang")
    endif()
else()
    message(FATAL_ERROR "Could not find LLVM. Please install LLVM and Clang or specify LLVM_VERSION.")
endif()

# Only as a fallback if the variables are empty
if(NOT LLVM_LIBRARIES)
  # Use llvm-config to get the library name
  execute_process(
    COMMAND ${LLVM_CONFIG_EXEC} --libs
    OUTPUT_VARIABLE LLVM_LIBRARIES
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
endif()

# Similarly for Clang, but using a search for the actual library
find_library(CLANG_LIB clang PATHS ${LLVM_LIBRARY_DIRS})
if (NOT CLANG_LIB)
  message(FATAL_ERROR "Cannot find libclang in ${LLVM_LIBRARY_DIRS}")
endif()
find_library(CLANG_LIB_CPP clang-cpp PATHS ${LLVM_LIBRARY_DIRS})
if (NOT CLANG_LIB_CPP)
  message(FATAL_ERROR "Cannot find libclang-cpp in ${LLVM_LIBRARY_DIRS}")
endif()
set(CLANG_LIBRARIES ${CLANG_LIB} ${CLANG_LIB_CPP})

# Get LLVM prefix for configuration
execute_process(
    COMMAND ${LLVM_CONFIG_EXEC} --prefix
    OUTPUT_VARIABLE CLANG_PREFIX_VALUE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE LLVM_CONFIG_RESULT
)
if(LLVM_CONFIG_RESULT EQUAL 0 AND CLANG_PREFIX_VALUE)
    set(CLANG_PREFIX "${CLANG_PREFIX_VALUE}")
else()
    message(WARNING "Failed to get CLANG_PREFIX from llvm-config")
endif()

# SANITY CHECK: Test if we can successfully link against Clang/LLVM
if(LLVM_FOUND AND Clang_FOUND)
    # Default to not working until proven otherwise
    set(CLANG_LINKING_WORKS FALSE)
    
    # Create a unique directory for our test files to prevent interference with other tests
    set(CLANG_TEST_DIR "${CMAKE_CURRENT_BINARY_DIR}${CMAKE_FILES_DIRECTORY}/ClangTest")
    file(MAKE_DIRECTORY ${CLANG_TEST_DIR})
    
    # Write the test file to our dedicated directory
    set(CLANG_BASIC_TEST_FILE "${CLANG_TEST_DIR}/clang_basic_test.cpp")
    file(WRITE "${CLANG_BASIC_TEST_FILE}"
        "#include <clang/Basic/Version.h>\n"
        "#include <clang/Basic/Diagnostic.h>\n"
        "#include <iostream>\n"
        "int main() {\n"
        "  std::cout << \"Clang compiled successfully\" << std::endl;\n"
        "  // Just create a simple Clang object to test linking\n"
        "  clang::DiagnosticsEngine::Level level = clang::DiagnosticsEngine::Warning;\n"
        "  return 0;\n"
        "}\n")
    
    # Compile the test separately (try_compile sets up its own directory)
    message(STATUS "Attempting to compile and link a basic Clang program...")
    try_compile(CLANG_BASIC_COMPILE_WORKS ${CMAKE_CURRENT_BINARY_DIR}
        SOURCES ${CLANG_BASIC_TEST_FILE}
        CMAKE_FLAGS
            "-DINCLUDE_DIRECTORIES=${LLVM_INCLUDE_DIRS};${CLANG_INCLUDE_DIRS}"
            "-DCOMPILE_DEFINITIONS=${LLVM_DEFINITIONS}"
        LINK_LIBRARIES ${CLANG_LIBRARIES} ${LLVM_LIBRARIES}
        LINK_OPTIONS -L${LLVM_LIBRARY_DIRS} 
        OUTPUT_VARIABLE CLANG_BASIC_COMPILE_OUTPUT
    )
    
    if(CLANG_BASIC_COMPILE_WORKS)
        message(STATUS "Successfully compiled and linked basic Clang test program")
        set(CLANG_LINKING_WORKS TRUE)
        
        # If basic test succeeded, try a more complex test using some Clang features
        # Useful for finding more subtle compatibility issues
        set(CLANG_FEATURE_TEST_FILE "${CLANG_TEST_DIR}/clang_feature_test.cpp")
        file(WRITE "${CLANG_FEATURE_TEST_FILE}"
            "#include <clang/Basic/Version.h>\n"
            "#include <clang/Basic/DiagnosticOptions.h>\n"
            "#include <clang/Basic/Diagnostic.h>\n"
            "#include <iostream>\n"
            "int main() {\n"
            "  clang::DiagnosticOptions diagOpts;\n"
            "  clang::DiagnosticsEngine::Level level = clang::DiagnosticsEngine::Warning;\n"
            "  std::cout << \"Successfully initialized Clang diagnostic classes\" << std::endl;\n"
            "  return 0;\n"
            "}\n")
                
        try_compile(CLANG_FEATURE_COMPILE_WORKS ${CMAKE_CURRENT_BINARY_DIR}
            SOURCES ${CLANG_FEATURE_TEST_FILE}
            CMAKE_FLAGS
                "-DINCLUDE_DIRECTORIES=${LLVM_INCLUDE_DIRS};${CLANG_INCLUDE_DIRS}"
                "-DCOMPILE_DEFINITIONS=${LLVM_DEFINITIONS}"
            LINK_LIBRARIES ${CLANG_LIBRARIES} ${LLVM_LIBRARIES}
            LINK_OPTIONS -L${LLVM_LIBRARY_DIRS}
            OUTPUT_VARIABLE CLANG_FEATURE_COMPILE_OUTPUT
        )
            
        if(NOT CLANG_FEATURE_COMPILE_WORKS)
            message(WARNING "Extended Clang feature test failed to compile. This may indicate API incompatibilities:\n${CLANG_FEATURE_COMPILE_OUTPUT}")
        else()
            message(STATUS "Extended Clang feature test compiled successfully")
        endif()
    else()
        message(WARNING "Failed to compile Clang test program. This indicates linking problems with Clang/LLVM libraries:\n${CLANG_BASIC_COMPILE_OUTPUT}")
    endif()
    
    # Export the linking status to parent scope
    set(CLANG_LINKING_WORKS ${CLANG_LINKING_WORKS})
    
    # If the test fails, provide a more helpful error message
    if(NOT CLANG_LINKING_WORKS)
        message(FATAL_ERROR "
Found LLVM and Clang, but the basic sanity test failed. This usually indicates:
1. Missing or incompatible libraries
2. ABI incompatibility between the compiler and LLVM/Clang libraries
3. Missing dependencies for LLVM/Clang

Check that you have the correct versions of LLVM and Clang installed and that 
all necessary dependencies are available. You may need to set LD_LIBRARY_PATH 
or similar environment variables to help locate the libraries.

Common issues include:
- Missing development packages (e.g., libclang-dev)
- Incompatible C++ standard library versions
- Missing LLVM components")
    endif()
endif()

# For CMake's find_package system
# Export these variables to the calling scope
