# Aligned with LLVM 20
cmake_minimum_required(VERSION 3.20.0)

# Setup project version
# Major Version 0: Initial Setup, see the main branch for details
# Major Version 1: Support XSan@(ASan + TSan + UBSan)
# Major Version 2: Support XSan@(ASan + TSan + UBSan + MSan)
set(PROJECT_VERSION_MAJOR 3)
set(PROJECT_VERSION_MINOR 8)
set(PROJECT_VERSION_PATCH 0)
set(PROJECT_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}")

file(WRITE "${CMAKE_BINARY_DIR}/version.txt" "${PROJECT_VERSION}\n")

# Cmake will check compilers in `project(...)`
project(xsan VERSION ${PROJECT_VERSION})

# To compile *.S in runtime
enable_language(C CXX ASM)

# export compile_commands.json for clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS 1)

# Reading the configuration file.
set(CONFIG_FILE_PATH "${CMAKE_SOURCE_DIR}/xsan_config.cmake")

# To find *.cmake
list(INSERT CMAKE_MODULE_PATH 0
  "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/cmake"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/cmake/Modules"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/cmake"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/cmake/Modules"
)


include(XSanUtils) # introduce message_green()... function
# This include also performs `find_package(LLVM REQUIRED CONFIG)`
include(ConfigLLVMCompiler) # ensure we use clang-15 and LLVM-15 to build XSan
include(AllSupportedArchDefs)


if(EXISTS ${CONFIG_FILE_PATH})
  include(${CONFIG_FILE_PATH})
else()
  message(STATUS "Configuration file not found, using default settings.")

  # If the config file does not exist, define the list here with defaults
  # This ensures the loop runs even without the config file
  set(XSAN_SANITIZERS TSAN ASAN UBSAN MSAN)
endif()

# Now, define options and add definitions in a loop for each sanitizer
# The option() command will respect values loaded from the included config file,
# or use the default OFF value if not set in the config file.
# Now we only use in compiler.
set(XSAN_DEFINITIONS_TO_ADD "")
foreach(san IN LISTS XSAN_SANITIZERS)
  # Construct the variable name for the option (e.g., XSAN_CONTAINS_TSAN)
  set(SANITIZER_OPTION_VAR "XSAN_CONTAINS_${san}")

  # Define the CMake option for this sanitizer.
  # The option() command will use the value from xsan_config.cmake if set,
  # otherwise it will use the default (OFF) and create a cache variable.
  if(NOT DEFINED ${SANITIZER_OPTION_VAR})
    option(${SANITIZER_OPTION_VAR} "Enable ${san} globally in XSan" ON)
  endif()

  # Print the status of the option
  message(STATUS "${SANITIZER_OPTION_VAR} is set to ${${SANITIZER_OPTION_VAR}}")

  # Check if the option is enabled and add the corresponding definition
  if(${${SANITIZER_OPTION_VAR}}) # Note the double ${} for the condition check
    set(MACRO_VALUE 1)
    message(STATUS "Defining preprocessor macro -D${SANITIZER_OPTION_VAR}=1")
  else()
    set(MACRO_VALUE 0)
    message(STATUS "Defining preprocessor macro -D${SANITIZER_OPTION_VAR}=0")
  endif()

  list(APPEND XSAN_DEFINITIONS_TO_ADD "-D${SANITIZER_OPTION_VAR}=${MACRO_VALUE}")
endforeach()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(FATAL_ERROR "
No build type selected. You need to pass -DCMAKE_BUILD_TYPE=<type> in order to configure LLVM.
Available options are:
  * -DCMAKE_BUILD_TYPE=Release - For an optimized build with no assertions or debug info.
  * -DCMAKE_BUILD_TYPE=Debug - For an unoptimized build with assertions and debug info.
  * -DCMAKE_BUILD_TYPE=RelWithDebInfo - For an optimized build with no assertions but with debug info.
  * -DCMAKE_BUILD_TYPE=MinSizeRel - For a build optimized for size instead of speed.
Learn more about these options in our documentation at https://llvm.org/docs/CMake.html#cmake-build-type
")
endif()

# Check the type if one of the four supported build types
if(NOT CMAKE_BUILD_TYPE MATCHES "^(Release|Debug|RelWithDebInfo|MinSizeRel)$")
  message(FATAL_ERROR "
Invalid build type: ${CMAKE_BUILD_TYPE}. You need to pass -DCMAKE_BUILD_TYPE=<type> in order to configure LLVM.
Available options are:
  * -DCMAKE_BUILD_TYPE=Release - For an optimized build with no assertions or debug info.
  * -DCMAKE_BUILD_TYPE=Debug - For an unoptimized build with assertions and debug info.
  * -DCMAKE_BUILD_TYPE=RelWithDebInfo - For an optimized build with no assertions but with debug info.
  * -DCMAKE_BUILD_TYPE=MinSizeRel - For a build optimized for size instead of speed.
Learn more about these options in our documentation at https://llvm.org/docs/CMake.html#cmake-build-type
")
endif()

message_green("C compiler: ${CMAKE_C_COMPILER}")
message_green("C++ compiler: ${CMAKE_CXX_COMPILER}")

# Set C standard to C17
set(CMAKE_C_STANDARD 17 CACHE STRING "C standard to conform to")
set(CMAKE_C_STANDARD_REQUIRED YES)

# Exactly, LLVM 15 just requires C++14, but we need to compile the runtime and instrumentation
# that requires C++17.
set(CMAKE_CXX_STANDARD 17 CACHE STRING "C++ standard to conform to")
set(CMAKE_CXX_STANDARD_REQUIRED YES)


include(CheckLinkerFlag)
set(FUSE_LD_LLD_FLAG "-fuse-ld=lld")
message(STATUS "Checking if CXX compiler supports ${FUSE_LD_LLD_FLAG}")
check_linker_flag(CXX "${FUSE_LD_LLD_FLAG}" CXX_SUPPORT_LLD)

# Preferring lld linker over system linker
if(CXX_SUPPORT_LLD)
  message_green("CC supports ${FUSE_LD_LLD_FLAG}, use lld as linker")
  # Fix dynsym corruption issue in libclang_rt.tsan.so
  # NOTE: CMAKE_LINKER is useless to control the linker as follows:
  #     https://gitlab.kitware.com/cmake/cmake/-/issues/24990
  # If we want to use lld to link *.o to *.so, we should use LINKER_FLAGS
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fuse-ld=lld")
else()
  message(WARNING "lld not found, use system default linker (ld).")
endif()

# Notify user to specify the LLVM_PATH, otherwise we cannot
# enable the full support of XSAN.
# LLVM_PATH could be set by environment variable or CMake variable.
if(NOT LLVM_PATH)
  message(WARNING "LLVM_PATH is not set. Please set it to your LLVM source directory.")
else()
  include(HandleOutOfTreeLLVM)
  set(LLVM_THIRD_PARTY_DIR ${LLVM_PATH}/third-party)
endif()

# ------------------------ Configure XSan build/install/package ------------------------
include(XsanBuildInstallPack)
# ---------------------------------------------------------------------------------------

# TODO: de-hardcode this
set(XSAN_COMMON_LIT_CONFIGURE_PATH
  ${CMAKE_BINARY_DIR}/test/lit.common.configured)


foreach(arch ${ALL_XSAN_SUPPORTED_ARCH})
  get_triple(${arch} XSAN_${arch}_TRIPLE)
  message_green("XSAN_${arch}_TRIPLE: ${XSAN_${arch}_TRIPLE}")
endforeach()

set(XSAN_HOST_TRIPLE ${XSAN_${CMAKE_HOST_SYSTEM_PROCESSOR}_TRIPLE})

add_subdirectory(src)

# Now this directory is added from /src/runtime/test/CMakeLists.txt,
# in order to include the relevant variables.
# add_subdirectory(test)
