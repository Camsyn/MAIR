# Use green color to notify that test building is in progress.
message_green("Building tests for Xsan")
# This file is modified from src/runtime/test/asan/CMakeLists.txt
set(XSAN_TEST_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(SANITIZER_COMMON_LIT_TEST_DEPS)

set(UBSAN_SUPPORTED_ARCH ${XSAN_SUPPORTED_ARCH})
set(ASAN_SUPPORTED_ARCH ${XSAN_SUPPORTED_ARCH})
set(TSAN_SUPPORTED_ARCH ${XSAN_SUPPORTED_ARCH})
set(MSAN_SUPPORTED_ARCH ${XSAN_SUPPORTED_ARCH})

# To make `get_test_cc_for_arch` happy
# Now we inherite all variables from src/runtime/test/CMakeLists.txt,
#  and thus no need to set this variable again.
# set(COMPILER_RT_SUPPORTED_ARCH ${XSAN_SUPPORTED_ARCH})

list(INSERT CMAKE_MODULE_PATH 0
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake"
)

# Some tests rely on profile, e.g., asan_and_llvm_coverage_test.cpp
if(COMPILER_RT_BUILD_PROFILE AND COMPILER_RT_HAS_PROFILE)
  list(APPEND SANITIZER_COMMON_LIT_TEST_DEPS profile)
endif()

# When ANDROID, we build tests with the host compiler (i.e. CMAKE_C_COMPILER),
# and run tests with tools from the host toolchain.
if(NOT ANDROID)
  # Tests use C++ standard library headers.
  if (TARGET cxx-headers OR HAVE_LIBCXX)
    list(APPEND SANITIZER_COMMON_LIT_TEST_DEPS cxx-headers)
  endif()
endif()

set(XSAN_LIT_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set(XSAN_TESTSUITES)
set(XSAN_DYNAMIC_TESTSUITES)

# Before Windows 8 (CMAKE_SYSTEM_VERSION 6.2), reserving large regions of shadow
# memory allocated physical memory for page tables, which made it very
# unreliable. Remove the XSan tests from check-all in this configuration.
set(SHADOW_MAPPING_UNRELIABLE FALSE)
if(OS_NAME MATCHES "Windows" AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND
    CMAKE_SYSTEM_VERSION LESS 6.2)
  set(SHADOW_MAPPING_UNRELIABLE TRUE)
  message(WARNING "Disabling XSan tests because they are unreliable on Windows 7 and earlier")
endif()

macro(get_bits_for_arch arch bits)
  if (${arch} MATCHES "x86_64|powerpc64|powerpc64le|aarch64|arm64|mips64|mips64el|s390x|sparcv9|riscv64")
    set(${bits} 64)
  elseif (${arch} MATCHES "i386|arm|mips|mipsel|sparc")
    set(${bits} 32)
  else()
    message(FATAL_ERROR "Unknown target architecture: ${arch}")
  endif()
endmacro()

set(XSAN_TEST_DEPS ${SANITIZER_COMMON_LIT_TEST_DEPS})
# testing XSan depends on umbrella target xsan.
# If you want to test original sanitizers, as the test does not
# depend on the original sanitizers, you should run
#     cmake --build build -j 
# to build all targets.
list(APPEND XSAN_TEST_DEPS xsan)
set(XSAN_DYNAMIC_TEST_DEPS ${XSAN_TEST_DEPS})

## TODO: support different architectures
set(XSAN_TEST_ARCH ${XSAN_SUPPORTED_ARCH})
if(APPLE)
  darwin_filter_host_archs(XSAN_SUPPORTED_ARCH XSAN_TEST_ARCH)
endif()
list(REMOVE_ITEM XSAN_TEST_ARCH sparc sparcv9)
if(OS_NAME MATCHES "SunOS")
  list(REMOVE_ITEM XSAN_TEST_ARCH x86_64)
endif()

set(XCLANG_PATH "${XSAN_OUTPUT_BINDIR}/xclang" CACHE PATH "Path to XSan's xclang wrapper")
set(XSAN_TEST_TARGET_CC ${XCLANG_PATH})

# Force xclang as the default test compiler.
set(COMPILER_RT_TEST_COMPILER "${XCLANG_PATH}" CACHE PATH "Compiler to use for testing" FORCE)
set(COMPILER_RT_TEST_COMPILER_ID "Clang" CACHE STRING "Test compiler ID" FORCE)


if (SHADOW_MAPPING_UNRELIABLE)
  set(exclude_from_check_all.g "EXCLUDE_FROM_CHECK_ALL")
else()
  set(exclude_from_check_all.g "")
endif()


set(XSAN_LIT_TEST_TARGETS)



# Run sanitizer tests only if we're sure that clang would produce
# working binaries.
# compiler_rt_test_runtime(interception)

# compiler_rt_test_runtime(lsan)
# CFI tests require diagnostic mode, which is implemented in UBSan.
# compiler_rt_test_runtime(ubsan cfi)
# compiler_rt_test_runtime(sanitizer_common)

# OpenBSD not supporting asan, cannot run the tests
add_subdirectory(asan)
add_subdirectory(tsan)
add_subdirectory(ubsan)
add_subdirectory(msan)
add_subdirectory(xsan)
add_subdirectory(MAIR)



if(NOT TARGET check-all)
  add_custom_target(check-all)
endif()
message(STATUS "check all depends on ${XSAN_LIT_TEST_TARGETS}")
add_dependencies(check-all ${XSAN_LIT_TEST_TARGETS})


if(NOT TARGET check)
  add_custom_target(check DEPENDS check-all)
endif()


# if (QSYM_BACKEND)
#   set(SYM_TEST_FILECHECK_ARGS "--check-prefix=QSYM --check-prefix=ANY")
# else()
#   set(SYM_TEST_FILECHECK_ARGS "--check-prefix=SIMPLE --check-prefix=ANY")
# endif()

# if (${LLVM_VERSION_MAJOR} VERSION_GREATER_EQUAL 14)
#   # FileCheck used to be fine with unused prefixes when more than one prefix was
#   # defined. This changed in LLVM version 14, requiring the new option
#   # "--allow-unused-prefixes" (added in LLVM 13) to restore the old behavior.
#   set(SYM_TEST_FILECHECK_ARGS "${SYM_TEST_FILECHECK_ARGS} --allow-unused-prefixes")
# endif()

# configure_file("lit.site.cfg.in" "lit.site.cfg")

# add_custom_target(check
#   lit
#   --verbose
#   --path=${LLVM_TOOLS_BINARY_DIR}
#   ${CMAKE_CURRENT_BINARY_DIR}
#   COMMENT "Testing the system..."
#   USES_TERMINAL)

# add_dependencies(check SymRuntime Symbolize)
# if (TARGET SymRuntime32)
#   add_dependencies(check SymRuntime32)
# endif()
