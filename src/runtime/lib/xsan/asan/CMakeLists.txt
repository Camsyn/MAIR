# output variables:
# - ${output_prefix}_HEADERS
# - ${output_prefix}_INCLUDE_DIRS
# - ${output_prefix}_SOURCES
# - ${output_prefix}_COND_SOURCES (sources using the XSAN_CONTAINS_* condition)
# - ${output_prefix}_STATIC_SOURCES
# - ${output_prefix}_CFLAGS
# - ${output_prefix}_DYNAMIC_CFLAGS
# - ${output_prefix}_COMMON_DEFINITIONS
# - ${output_prefix}_DYNAMIC_DEFINITIONS
# - ${output_prefix}_WRAPPED_SYMBOLS
# - ${output_prefix}_LINK_LIBS
function(import_subsan output_prefix)
  ######################## Headers #########################
  set(ASAN_HEADERS
    orig/asan_activation.h
    orig/asan_activation_flags.inc
    orig/asan_descriptions.h
    orig/asan_errors.h
    orig/asan_fake_stack.h
    orig/asan_flags.h
    orig/asan_flags.inc
    orig/asan_init_version.h
    orig/asan_interceptors.h
    orig/asan_interceptors_memintrinsics.h
    orig/asan_interface.inc
    orig/asan_interface_internal.h
    orig/asan_internal.h
    # orig/asan_mapping.h
    orig/asan_poisoning.h
    orig/asan_premap_shadow.h
    orig/asan_report.h
    orig/asan_scariness_score.h
    orig/asan_stack.h
    orig/asan_stats.h
    orig/asan_suppressions.h
    # orig/asan_thread.h
  )

  set(XSAN_ASAN_HEADERS
    asan_allocator.h
    asan_hooks.h
    asan_interface_xsan.h
    asan_mapping.h
    asan_thread.h
    ${ASAN_HEADERS}
  )

  set(ASAN_INCLUDE_DIRS
    .
    orig
  )

  ######################## Sources #########################
  set(ASAN_SOURCES
    # orig/asan_allocator.cpp
    orig/asan_activation.cpp
    orig/asan_debugging.cpp
    orig/asan_descriptions.cpp
    orig/asan_errors.cpp
    orig/asan_fake_stack.cpp
    # orig/asan_flags.cpp
    orig/asan_fuchsia.cpp
    # orig/asan_globals.cpp
    orig/asan_globals_win.cpp
    # orig/asan_interceptors.cpp
    # orig/asan_interceptors_memintrinsics.cpp
    # orig/asan_malloc_linux.cpp
    # orig/asan_malloc_mac.cpp
    orig/asan_linux.cpp
    orig/asan_mac.cpp
    orig/asan_malloc_win.cpp
    orig/asan_memory_profile.cpp
    # orig/asan_poisoning.cpp
    orig/asan_posix.cpp
    orig/asan_premap_shadow.cpp
    orig/asan_report.cpp
    # orig/asan_rtl.cpp
    orig/asan_shadow_setup.cpp
    # orig/asan_stack.cpp
    orig/asan_stats.cpp
    orig/asan_suppressions.cpp
    # orig/asan_thread.cpp
    orig/asan_win.cpp
  )

  set(XSAN_ASAN_SOURCES
    asan_flags.cpp
    asan_globals.cpp
    asan_interceptors_memintrinsics.cpp
    asan_interceptors.cpp
    asan_poisoning.cpp
    asan_rtl.cpp
    asan_stack.cpp
    asan_thread.cpp
    ${ASAN_SOURCES}
  )

  set(ASAN_COND_SOURCES
    asan_allocator.cpp
    asan_hooks.cpp
    asan_rtl_extra.cpp
  )

  # Note: orig/asan_rtl_static.cpp, orig/asan_rtl_x86_64.S are intentionally
  # NOT included here. The standard libclang_rt.asan_static.a is always
  # co-linked when XSan uses ASan, and it already provides these symbols.
  # Including them here would cause duplicate symbol errors at link time.
  set(ASAN_STATIC_SOURCES)

  ######################## Flags #########################
  set(ASAN_CFLAGS ${SANITIZER_COMMON_CFLAGS})

  append_list_if(MSVC /Zl ASAN_CFLAGS)

  append_rtti_flag(OFF ASAN_CFLAGS)

  # Silence warnings in system headers with MSVC.
  if(NOT CLANG_CL)
    append_list_if(COMPILER_RT_HAS_EXTERNAL_FLAG "/experimental:external;/external:W0;/external:anglebrackets" ASAN_CFLAGS)
  endif()

  # Too many existing bugs, needs cleanup.
  append_list_if(COMPILER_RT_HAS_WNO_FORMAT -Wno-format ASAN_CFLAGS)

  set(ASAN_DYNAMIC_LINK_FLAGS ${SANITIZER_COMMON_LINK_FLAGS})

  if(ANDROID)
  # Put most Sanitizer shared libraries in the global group. For more details, see
  # android-changes-for-ndk-developers.md#changes-to-library-search-order
    if (COMPILER_RT_HAS_Z_GLOBAL)
      list(APPEND ASAN_DYNAMIC_LINK_FLAGS -Wl,-z,global)
    endif()
  endif()

  set(ASAN_DYNAMIC_CFLAGS ${ASAN_CFLAGS})
  append_list_if(COMPILER_RT_HAS_FTLS_MODEL_INITIAL_EXEC -ftls-model=initial-exec ASAN_DYNAMIC_CFLAGS)

  # LLVM turns /OPT:ICF back on when LLVM_ENABLE_PDBs is set
  # we _REALLY_ need to turn it back off for ASAN, because the way
  # asan emulates weak functions from DLLs requires NOICF
  append_list_if(MSVC "LINKER:/DEBUG;LINKER:/OPT:NOICF" ASAN_DYNAMIC_LINK_FLAGS)

  ######################## Definitions #########################
  set(ASAN_COMMON_DEFINITIONS "")
  set(ASAN_DYNAMIC_DEFINITIONS ${ASAN_COMMON_DEFINITIONS} ASAN_DYNAMIC=1)
  append_list_if(WIN32 INTERCEPTION_DYNAMIC_CRT ASAN_DYNAMIC_DEFINITIONS)

  ######################## Link Libraries #########################
  set(ASAN_DYNAMIC_LIBS
    ${COMPILER_RT_UNWINDER_LINK_LIBS}
    ${SANITIZER_CXX_ABI_LIBRARIES}
    ${SANITIZER_COMMON_LINK_LIBS}
  )

  append_list_if(COMPILER_RT_HAS_LIBDL dl ASAN_DYNAMIC_LIBS)
  append_list_if(COMPILER_RT_HAS_LIBRT rt ASAN_DYNAMIC_LIBS)
  append_list_if(COMPILER_RT_HAS_LIBM m ASAN_DYNAMIC_LIBS)
  append_list_if(COMPILER_RT_HAS_LIBPTHREAD pthread ASAN_DYNAMIC_LIBS)
  append_list_if(COMPILER_RT_HAS_LIBLOG log ASAN_DYNAMIC_LIBS)
  append_list_if(MINGW "${MINGW_LIBRARIES}" ASAN_DYNAMIC_LIBS)

  ######################## Output Variables #########################
  set(${output_prefix}_HEADERS
    ${XSAN_ASAN_HEADERS}
    PARENT_SCOPE
  )

  set(${output_prefix}_INCLUDE_DIRS
    ${ASAN_INCLUDE_DIRS}
    PARENT_SCOPE
  )

  set(${output_prefix}_SOURCES
    ${XSAN_ASAN_SOURCES}
    PARENT_SCOPE
  )

  set(${output_prefix}_COND_SOURCES
    ${ASAN_COND_SOURCES}
    PARENT_SCOPE
  )

  set(${output_prefix}_STATIC_SOURCES
    ${ASAN_STATIC_SOURCES}
    PARENT_SCOPE
  )

  set(${output_prefix}_CFLAGS
    ${ASAN_CFLAGS}
    PARENT_SCOPE
  )

  set(${output_prefix}_DYNAMIC_CFLAGS
    ${ASAN_DYNAMIC_CFLAGS}
    PARENT_SCOPE
  )

  set(${output_prefix}_COMMON_DEFINITIONS
    ${ASAN_COMMON_DEFINITIONS}
    PARENT_SCOPE
  )

  set(${output_prefix}_DYNAMIC_DEFINITIONS
    ${ASAN_DYNAMIC_DEFINITIONS}
    PARENT_SCOPE
  )

  set(${output_prefix}_WRAPPED_SYMBOLS
    asan_wrapped_symbols.txt.in
    PARENT_SCOPE
  )

  set(${output_prefix}_LINK_LIBS
    ${ASAN_DYNAMIC_LIBS}
    PARENT_SCOPE
  )
endfunction()
