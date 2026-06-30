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
# - ${output_prefix}_SYMS_EXTRA
# - ${output_prefix}_LINK_LIBS
function(import_subsan output_prefix)
  ######################## Headers #########################
  set(MSAN_HEADERS
    # orig/msan.h
    # orig/msan_allocator.h
    orig/msan_chained_origin_depot.h
    orig/msan_dl.h
    orig/msan_flags.h
    orig/msan_flags.inc
    orig/msan_interface_internal.h
    orig/msan_origin.h
    orig/msan_poisoning.h
    orig/msan_report.h
    # orig/msan_thread.h
  )

  set(XSAN_MSAN_HEADERS
    msan.h
    msan_hooks.h
    msan_interface_xsan.h
    msan_rtl_extra.h
    msan_thread.h
    ${MSAN_HEADERS}
  )

  set(MSAN_INCLUDE_DIRS
    .
    orig
  )

  ######################## Sources #########################
  set(MSAN_SOURCES
    # orig/msan.cpp
    # orig/msan_allocator.cpp
    orig/msan_chained_origin_depot.cpp
    orig/msan_dl.cpp
    # orig/msan_interceptors.cpp
    # orig/msan_linux.cpp
    orig/msan_report.cpp
    # orig/msan_thread.cpp
    orig/msan_poisoning.cpp
  )

  set(XSAN_MSAN_SOURCES
    msan.cpp
    msan_linux.cpp
    msan_rtl_extra.cpp
    ${MSAN_SOURCES}
  )

  set(MSAN_COND_SOURCES
    msan_interceptors.cpp
    msan_hooks.cpp
  )

  ######################## Flags #########################
  set(MSAN_RTL_CFLAGS ${SANITIZER_COMMON_CFLAGS})
  if(CMAKE_SYSTEM_NAME MATCHES "FreeBSD")
    append_list_if(COMPILER_RT_HAS_FTLS_MODEL_INITIAL_EXEC -ftls-model=initial-exec MSAN_RTL_CFLAGS)
  endif()
  append_rtti_flag(OFF MSAN_RTL_CFLAGS)
  if(NOT CMAKE_SYSTEM_NAME MATCHES "FreeBSD")
    append_list_if(COMPILER_RT_HAS_FPIE_FLAG -fPIE MSAN_RTL_CFLAGS)
  endif()
  # Prevent clang from generating libc calls.
  # append_list_if(COMPILER_RT_HAS_FFREESTANDING_FLAG -ffreestanding MSAN_RTL_CFLAGS)

  ######################## Output Variables #########################
  set(${output_prefix}_HEADERS
    ${XSAN_MSAN_HEADERS}
    PARENT_SCOPE
  )

  set(${output_prefix}_INCLUDE_DIRS
    ${MSAN_INCLUDE_DIRS}
    PARENT_SCOPE
  )

  set(${output_prefix}_SOURCES
    ${XSAN_MSAN_SOURCES}
    PARENT_SCOPE
  )

  set(${output_prefix}_COND_SOURCES
    ${MSAN_COND_SOURCES}
    PARENT_SCOPE
  )

  set(${output_prefix}_STATIC_SOURCES
    PARENT_SCOPE
  )

  set(${output_prefix}_CFLAGS
    ${MSAN_RTL_CFLAGS}
    PARENT_SCOPE
  )

  set(${output_prefix}_DYNAMIC_CFLAGS
    PARENT_SCOPE
  )

  set(${output_prefix}_COMMON_DEFINITIONS
    PARENT_SCOPE
  )

  set(${output_prefix}_DYNAMIC_DEFINITIONS
    PARENT_SCOPE
  )

  set(${output_prefix}_WRAPPED_SYMBOLS
    msan_wrapped_symbols.txt.in
    PARENT_SCOPE
  )

  set(${output_prefix}_SYMS_EXTRA
    msan.syms.extra
    PARENT_SCOPE
  )

  set(${output_prefix}_LINK_LIBS
    PARENT_SCOPE
  )
endfunction()
