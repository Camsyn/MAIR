# Unlike clang, gcc does not expose its functionality on a library.
# That is to say, we should intercept the non-dynamic functions.

# We should intercept the following functions theoretically:
# - `error_at`: to suppress the error when enable 'incompatible' sanitizer.
# - `finish_options`: the caller of `error_at`, should not be intercepted.
# - `do_spec` : A driver function to run the spec command.
# - It is hard to intercept, because it is stripped.
# - As a result, we rewrite the link_command_spec used in `do_spec` to our own.

# Generate XSan GCC spec file
include(gcc_spec_gen.cmake)

include_directories(${CMAKE_CURRENT_BINARY_DIR})

add_dso_livepatch(gcc-patch
    SOURCES
      diagnostic.cpp
      gcc.cpp
)
