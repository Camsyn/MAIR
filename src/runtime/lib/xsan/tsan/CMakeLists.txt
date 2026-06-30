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
  set(TSAN_HEADERS
    orig/tsan_defs.h
    orig/tsan_dense_alloc.h
    orig/tsan_fd.h
    orig/tsan_flags.h
    orig/tsan_flags.inc
    orig/tsan_ignoreset.h
    orig/tsan_ilist.h
    # orig/tsan_interceptors.h
    orig/tsan_interface.h
    # orig/tsan_interface.inc
    orig/tsan_interface_ann.h
    orig/tsan_interface_java.h
    orig/tsan_mman.h
    orig/tsan_mutexset.h
    # orig/tsan_platform.h
    orig/tsan_ppc_regs.h
    orig/tsan_report.h
    # orig/tsan_rtl.h
    orig/tsan_shadow.h
    orig/tsan_stack_trace.h
    orig/tsan_suppressions.h
    orig/tsan_symbolize.h
    orig/tsan_sync.h
    orig/tsan_trace.h
    orig/tsan_vector_clock.h
  )

  set(XSAN_TSAN_HEADERS
    tsan_hooks.h
    tsan_interceptors.h
    tsan_interface_xsan.h
    tsan_interface.inc
    tsan_platform.h
    tsan_rtl.h
    tsan_rtl_extra.h
    ${TSAN_HEADERS}
  )

  set(TSAN_INCLUDE_DIRS
    .
    orig
  )

  ######################## Sources #########################
  set(TSAN_SOURCES
    orig/tsan_debugging.cpp
    orig/tsan_external.cpp
    # orig/tsan_fd.cpp
    # orig/tsan_flags.cpp
    orig/tsan_ignoreset.cpp
    # orig/tsan_interceptors_memintrinsics.cpp
    # orig/tsan_interceptors_posix.cpp
    orig/tsan_interface.cpp
    orig/tsan_interface_ann.cpp
    orig/tsan_interface_atomic.cpp
    orig/tsan_interface_java.cpp
    orig/tsan_malloc_mac.cpp
    orig/tsan_md5.cpp
    # orig/tsan_mman.cpp
    orig/tsan_mutexset.cpp
    orig/tsan_report.cpp
    orig/tsan_rtl.cpp
    orig/tsan_rtl_access.cpp
    orig/tsan_rtl_mutex.cpp
    orig/tsan_rtl_proc.cpp
    # orig/tsan_rtl_report.cpp
    # orig/tsan_rtl_thread.cpp
    # orig/tsan_stack_trace.cpp
    orig/tsan_suppressions.cpp
    orig/tsan_symbolize.cpp
    # orig/tsan_sync.cpp
    orig/tsan_vector_clock.cpp
  )

  if(APPLE)
    list(APPEND TSAN_SOURCES
      # orig/tsan_interceptors_mac.cpp
      # orig/tsan_interceptors_mach_vm.cpp
      orig/tsan_platform_mac.cpp
      # orig/tsan_platform_posix.cpp
      tsan_platform_posix.cpp
    )
  elseif(UNIX)
    # Assume Linux
    list(APPEND TSAN_SOURCES
      # orig/tsan_platform_linux.cpp
      # orig/tsan_platform_posix.cpp
      tsan_platform_linux.cpp
      tsan_platform_posix.cpp
    )
  endif()

  if(COMPILER_RT_INTERCEPT_LIBDISPATCH)
    list(APPEND TSAN_SOURCES
      # TODO: provide uniform interceptors for libdispatch
      orig/tsan_interceptors_libdispatch.cpp
    )
  endif()


  set(TSAN_ASM_SOURCES)

  if (NOT APPLE)
    foreach(arch ${TSAN_SUPPORTED_ARCH})
      if(arch STREQUAL "x86_64")
        set(TSAN_ASM_SOURCES
          orig/tsan_rtl_amd64.S
        )
        # Go is not supported.
      elseif(arch STREQUAL "aarch64")
        set(TSAN_ASM_SOURCES
          orig/tsan_rtl_aarch64.S
        )
        # Go is not supported.
      elseif(arch MATCHES "loongarch64")
        set(TSAN_ASM_SOURCES
          orig/tsan_rtl_loongarch64.S
        )
      elseif(arch MATCHES "powerpc64|powerpc64le")
        set(TSAN_ASM_SOURCES
          orig/tsan_rtl_ppc64.S
        )
      elseif(arch MATCHES "mips64|mips64le")
        set(TSAN_ASM_SOURCES
          orig/tsan_rtl_mips64.S
        )
      elseif(arch MATCHES "riscv64")
        set(TSAN_ASM_SOURCES
          orig/tsan_rtl_riscv64.S
        )
      elseif(arch MATCHES "s390x")
        set(TSAN_ASM_SOURCES
          orig/tsan_rtl_s390x.S
        )
      else()
      endif()
    endforeach()
  endif()

  set(XSAN_TSAN_SOURCES
    tsan_fd.cpp
    tsan_flags.cpp
    tsan_init.cpp
    tsan_interceptors_memintrinsics.cpp
    tsan_mman.cpp
    tsan_rtl_extra.cpp
    tsan_rtl_thread.cpp
    tsan_stack_trace.cpp
    ${TSAN_SOURCES}
    ${TSAN_ASM_SOURCES}
  )

  set(TSAN_COND_SOURCES
    tsan_interceptors.cpp
    tsan_hooks.cpp
    tsan_rtl_report.cpp
    tsan_sync.cpp
  )

  ######################## Flags #########################
  set(TSAN_CFLAGS ${SANITIZER_COMMON_CFLAGS})
  # SANITIZER_COMMON_CFLAGS contains -fPIC, but it's performance-critical for
  # TSan runtime to be built with -fPIE to reduce the number of register spills.
  # On FreeBSD however it provokes linkage issue thus we disable it.
  if(NOT CMAKE_SYSTEM MATCHES "FreeBSD")
    # -fPIE reduces the number of register spills in TSan runtime.
    # This is performance-critical.
    append_list_if(COMPILER_RT_HAS_FPIE_FLAG -fPIE TSAN_CFLAGS)
  endif()
  append_rtti_flag(OFF TSAN_CFLAGS)

  if(COMPILER_RT_TSAN_DEBUG_OUTPUT)
    # Add extra debug information to TSan runtime. This configuration is rarely
    # used, but we need to support it so that debug output will not bitrot.
    list(APPEND TSAN_CFLAGS -DTSAN_DEBUG_OUTPUT=2)
  endif()
  set(TSAN_RTL_CFLAGS ${TSAN_CFLAGS})
  # -mssse3 enables SIMD instructions that are used in TSan runtime.
  append_list_if(COMPILER_RT_HAS_MSSE4_2_FLAG -msse4.2 TSAN_RTL_CFLAGS)
  # # compile-time: -Wframe-larger-than=530: limit frame size to 530. Maybe useless?
  # append_list_if(SANITIZER_LIMIT_FRAME_SIZE -Wframe-larger-than=530
  #                 TSAN_RTL_CFLAGS)
  # # compile-time: -Wglobal-constructors: warn about global constructors. Maybe useless?
  # append_list_if(COMPILER_RT_HAS_WGLOBAL_CONSTRUCTORS_FLAG -Wglobal-constructors
  #                 TSAN_RTL_CFLAGS)

  if(COMPILER_RT_INTERCEPT_LIBDISPATCH)
    # Intercept libdispatch functions in Apple platforms (Grand Central Dispatch).
    list(APPEND TSAN_RTL_CFLAGS ${COMPILER_RT_LIBDISPATCH_CFLAGS})
  endif()

  # Dynamic CFLAGS
  set(TSAN_RTL_DYNAMIC_CFLAGS ${TSAN_RTL_CFLAGS})
  list(REMOVE_ITEM TSAN_RTL_DYNAMIC_CFLAGS -fPIE)

  ######################## Definitions #########################
  set(TSAN_DYNAMIC_DEFINITIONS SANITIZER_SHARED)

  ######################## Link Libraries #########################
  set(TSAN_DYNAMIC_LINK_LIBS
    ${COMPILER_RT_UNWINDER_LINK_LIBS}
    ${SANITIZER_CXX_ABI_LIBRARIES}
    ${SANITIZER_COMMON_LINK_LIBS}
  )

  append_list_if(COMPILER_RT_HAS_LIBDL dl TSAN_DYNAMIC_LINK_LIBS)
  append_list_if(COMPILER_RT_HAS_LIBM m TSAN_DYNAMIC_LINK_LIBS)
  append_list_if(COMPILER_RT_HAS_LIBPTHREAD pthread TSAN_DYNAMIC_LINK_LIBS)

  ######################## Output Variables #########################
  set(${output_prefix}_HEADERS
    ${XSAN_TSAN_HEADERS}
    PARENT_SCOPE
  )

  set(${output_prefix}_INCLUDE_DIRS
    ${TSAN_INCLUDE_DIRS}
    PARENT_SCOPE
  )

  set(${output_prefix}_SOURCES
    ${XSAN_TSAN_SOURCES}
    PARENT_SCOPE
  )

  set(${output_prefix}_COND_SOURCES
    ${TSAN_COND_SOURCES}
    PARENT_SCOPE
  )

  set(${output_prefix}_STATIC_SOURCES
    PARENT_SCOPE
  )

  set(${output_prefix}_CFLAGS
    ${TSAN_RTL_CFLAGS}
    PARENT_SCOPE
  )

  set(${output_prefix}_DYNAMIC_CFLAGS
    ${TSAN_RTL_DYNAMIC_CFLAGS}
    PARENT_SCOPE
  )

  set(${output_prefix}_COMMON_DEFINITIONS
    PARENT_SCOPE
  )

  set(${output_prefix}_DYNAMIC_DEFINITIONS
    ${TSAN_DYNAMIC_DEFINITIONS}
    PARENT_SCOPE
  )

  set(${output_prefix}_WRAPPED_SYMBOLS
    tsan_wrapped_symbols.txt.in
    PARENT_SCOPE
  )

  set(${output_prefix}_SYMS_EXTRA
    tsan.syms.extra
    PARENT_SCOPE
  )

  set(${output_prefix}_LINK_LIBS
    ${TSAN_DYNAMIC_LINK_LIBS}
    PARENT_SCOPE
  )
endfunction()
