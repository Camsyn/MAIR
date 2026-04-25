set(CMAKE_FIND_PACKAGE_SORT_ORDER NATURAL)
set(CMAKE_FIND_PACKAGE_SORT_DIRECTION DEC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_custom_target(livepatch)
add_dependencies(compiler livepatch)

# For compiler-side consumer sources under this directory.
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CMAKE_SOURCE_DIR}/src/livepatch)

# Both patches of clang/gcc rely on LLVM.
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

include_directories(${LLVM_INCLUDE_DIRS})

# If the dynsym locates in the main executable, the symbol with the same name
# will be masked. Therefore, we need to use -Bsymbolic-functions to ensure the
# symbol is not masked.
# -Bsymbolic: make the refernces to internal dyn symbols to be resolved to the
# relative address.
add_link_options(-Wl,-Bsymbolic-functions)

if(NOT DEFINED XSAN_LIVEPATCH_VARIANT)
  message(FATAL_ERROR "XSAN_LIVEPATCH_VARIANT is not set")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${XSAN_LIVEPATCH_VARIANT}/CMakeLists.txt")
  message(STATUS "Using clang livepatch sources from ${XSAN_LIVEPATCH_VARIANT}")
  add_subdirectory(${XSAN_LIVEPATCH_VARIANT})
else()
  message(FATAL_ERROR "Missing clang livepatch sources for ${XSAN_LIVEPATCH_VARIANT}")
endif()

# In theory, the hot patch of gcc does not limit the version of gcc
add_subdirectory(gcc)
