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

# Build livepatch as a DSO
add_dso_livepatch(clang-patch
    SOURCES
      BackendUtil.cpp
      CommonArgs.cpp
      PassBuilderPipelines.cpp
      SanitizerArgs.cpp
      ../clang-common/CommonArgs.cpp
)
