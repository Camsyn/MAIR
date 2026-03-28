set(CMAKE_FIND_PACKAGE_SORT_ORDER NATURAL)
set(CMAKE_FIND_PACKAGE_SORT_DIRECTION DEC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_custom_target(livepatch)
add_dependencies(compiler livepatch)

# For "util/PatchHelper.h"
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Both patches of clang/gcc rely on LLVM.
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

include_directories(${LLVM_INCLUDE_DIRS})

# #############################################################################
# ###   Install LLVMSupport and LLVMDemangle if they are shared libraries  ####
# #############################################################################
# Please refer to to $LLVM_DIR/lib/cmake/llvm/LLVMExports.cmake
# to see the imported targets of LLVM.
get_target_property(_llvmsupport_type LLVMSupport TYPE)

# If LLVMSupport is a shared library, user might get the error when using livepatch for gcc,
# which relies on LLVMSupport, as follows:
# gcc: error while loading shared libraries: libLLVMSupport.so.15:
# cannot open shared object file: No such file or directory
option(LINK_DYNAMIC_LLVMSUPPORT "Link dynamic LLVMSupport library" OFF)
message_green("_llvmsupport_type: ${_llvmsupport_type}")

if(_llvmsupport_type STREQUAL "SHARED_LIBRARY")
    set(LINK_DYNAMIC_LLVMSUPPORT ON CACHE BOOL "Link dynamic LLVMSupport library")

    # lib*-patch.so relies on libLLVMSupport.so, and libLLVMSupport.so relies on libLLVMDemangle.so.
    # Both of libLLVMSupport.so and libLLVMDemangle.so have the RUNPATH `$ORIGIN/../lib`.
    # Therefore, we can set the RUNPATH of lib*-patch.so to `$ORIGIN/../lib` as well, and
    # install the LLVMSupport and LLVMDemangle to the patch/../lib directory.
    get_target_property(LLVMSupport_LOCATION LLVMSupport LOCATION)
    get_target_property(LLVMDemangle_LOCATION LLVMDemangle LOCATION)

    # Install the LLVMSupport and LLVMDemangle to the patch/../lib directory.
    install(FILES ${LLVMSupport_LOCATION} DESTINATION ${XSAN_INSTALL_RUNTIME_DIR} COMPONENT patch)
    install(FILES ${LLVMDemangle_LOCATION} DESTINATION ${XSAN_INSTALL_RUNTIME_DIR} COMPONENT patch)
endif()

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
