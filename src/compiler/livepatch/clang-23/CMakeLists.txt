# Clang is well-designed, exposing lots of functionalities into libraries, e.g.,
# libclang-cpp.so.
#
# We should intercept the following dynamic functions:
# - `SanitizerArgs::SanitizerArgs`: to suppress the error when enable 'incompatible' sanitizer.
# - Could not just intercept the `Driver::Diag`, as we have to guarantee the functionality of
# the enabled sanitizers.
# - `EmitBackendOutput` : to disable the loading of passes.
# - `addSanitizerRuntimes` : to replace the link options of sanitizer runtimes.
# - `PassBuilder::build*Pipeline`:  (Optional) to reconstruct the pass pipeline.

# Find Clang only for obtaining its include directories
# find_package(PackageName REQUIRED CONFIG) would search PackageNameConfig.cmake
# We can define env var PackageName_DIR to specify the directory to search PackageNameConfig.cmake
find_package(Clang REQUIRED CONFIG
    # We use LLVM-15, so search the Clang-15 directly under the LLVM-15 directory.
    # This HINTS could be overridden by env var Clang_DIR.
    HINTS ${LLVM_BINARY_DIR}
)

include_directories(${CLANG_INCLUDE_DIRS})
include_directories(${CMAKE_CURRENT_BINARY_DIR})

# To shared libaray
add_library(clang-patch MODULE
    BackendUtil.cpp
    CommonArgs.cpp
    PassBuilderPipelines.cpp
    SanitizerArgs.cpp
    ../utils/PatchHelper.cpp
)
add_dependencies(livepatch clang-patch)

target_link_libraries(clang-patch PRIVATE dl)

if(LLVM_LINK_LLVM_DYLIB)
    if(NOT CLANG_LINK_CLANG_DYLIB)
        message(FATAL_ERROR "Cannot set CLANG_LINK_CLANG_DYLIB=OFF when "
            "LLVM_LINK_LLVM_DYLIB=ON")
    endif()

    target_compile_definitions(clang-patch PRIVATE -DCLANG_PATCH_LINK_DYLIB)

    if(NOT DEFINED LLVM_SHLIB_SYMBOL_VERSION)
        # "Symbol version prefix for libLLVM.so and libclang-cpp.so"
        set(LLVM_SHLIB_SYMBOL_VERSION "LLVM_${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}")
    endif()

    message(STATUS "LLVM_SHLIB_SYMBOL_VERSION: ${LLVM_SHLIB_SYMBOL_VERSION}")
    configure_file(simple_version_script.map.in simple_version_script.map)

    # ## Seems we don't need to explicitly link clang-cpp and LLVM libraries
    # ## because clang-patch will be injected into the clang process as a dynamic library
    # ## The clang process has already loaded these libraries, so we can use them directly
    # target_link_libraries(clang-patch PRIVATE clang-cpp LLVM)

    # To livepatch static-linked clang, we need to link LLVM libraries explicitly
    target_link_libraries(clang-patch PRIVATE LLVM)
    target_link_options(clang-patch PRIVATE -Wl,--version-script,${CMAKE_CURRENT_BINARY_DIR}/simple_version_script.map)
else()
    if(CLANG_LINK_CLANG_DYLIB)
        message(FATAL_ERROR "Cannot set CLANG_LINK_CLANG_DYLIB=ON when "
            "LLVM_LINK_LLVM_DYLIB=OFF")
    endif()

    # ## Seems we don't need to explicitly link clang-cpp and LLVM libraries
    # ## because clang-patch will be injected into the clang process as a dynamic library
    # ## The clang process has already loaded these libraries, so we can use them directly
    # target_link_libraries(clang-patch PRIVATE clangBasic LLVMSupport LLVMOption)

    # To livepatch static-linked clang, we need to link LLVM libraries explicitly
    target_link_libraries(clang-patch PRIVATE LLVMSupport)

    if(${LINK_DYNAMIC_LLVMSUPPORT})
        # You can use `readelf -d <ELF> | grep 'RPATH\|RUNPATH'` to check the RUNPATH.
        # Privilege: RPATH > LD_LIBRARIES_DIR > RUNPATH
        # The libLLVMSupport.so has the RUNPATH `$ORIGIN/../lib`,
        # When installing the patch, set `-rpath=$ORIGIN/../lib` to the patch, so that
        # the clang's livepatch can search the LLVMSupport in the lib directory.
        set_target_properties(clang-patch
            PROPERTIES
            # When build, we have different RPATH.
            INSTALL_RPATH "\$ORIGIN/../lib"
        )
    endif()
endif()

set_target_properties(clang-patch
    PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${XSAN_OUTPUT_PATCHDIR}"
)

install(TARGETS clang-patch
    LIBRARY
    DESTINATION ${XSAN_INSTALL_PATCHDIR}
    COMPONENT patch
)
