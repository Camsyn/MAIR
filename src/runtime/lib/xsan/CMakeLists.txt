function(_parse_xsan_comb target_name prefix comb suffix)
  # Check target_name is like .*clang_rt.xsan_<comb>-.*
  # E.g., output_dir_clang_rt.xsan_asan_tsan-cxx-x86_64
  string(REGEX MATCH "^(.*clang_rt\\.)(xsan(_[a-z]san)+)(.*)$" 
        _match "${target_name}")

  if(_match)
    set(${prefix} "${CMAKE_MATCH_1}" PARENT_SCOPE)
    set(${comb} "${CMAKE_MATCH_2}" PARENT_SCOPE)
    set(${suffix} "${CMAKE_MATCH_4}" PARENT_SCOPE)
  else()
    set(${prefix} "" PARENT_SCOPE)
    set(${comb} "" PARENT_SCOPE)
    set(${suffix} "" PARENT_SCOPE)
  endif()
endfunction()

# Override `get_compiler_rt_output_dir`@CompilerRTUtils.cmake only in this file,
# and rewrite XSan RT's output directory to ${compiler_rt_output_dir}/xsan-${comb}
# E.g., lib/x86_64-unknown-linux-gnu -> lib/x86_64-unknown-linux-gnu/xsan-asan-msan-tsan
function(get_compiler_rt_output_dir arch output_dir)
  if(LLVM_ENABLE_PER_TARGET_RUNTIME_DIR AND NOT APPLE)
    get_compiler_rt_target(${arch} target)
    set(_xsan_output_dir ${COMPILER_RT_OUTPUT_LIBRARY_DIR}/${target})
  else()
    set(_xsan_output_dir ${COMPILER_RT_OUTPUT_LIBRARY_DIR})
  endif()

  if (DEFINED target_comb AND target_comb)
    set(_xsan_output_dir "${_xsan_output_dir}/${target_comb}")
  else()
    _parse_xsan_comb("${output_dir}" _prefix _comb _suffix)
  
    if(DEFINED _comb AND _comb)
      # Add sub-directory <comb> to output_dir
      set(_xsan_output_dir "${_xsan_output_dir}/${_comb}")
    endif()
  endif()

  set(${output_dir} ${_xsan_output_dir} PARENT_SCOPE)
endfunction()

function(get_compiler_rt_install_dir arch install_dir)
  if(LLVM_ENABLE_PER_TARGET_RUNTIME_DIR AND NOT APPLE)
    get_compiler_rt_target(${arch} target)
    set(_xsan_install_dir ${COMPILER_RT_INSTALL_LIBRARY_DIR}/${target})
  else()
    set(_xsan_install_dir ${COMPILER_RT_INSTALL_LIBRARY_DIR})
  endif()

  if(DEFINED target_comb AND target_comb)
    set(_xsan_install_dir "${_xsan_install_dir}/${target_comb}")
  else()
    _parse_xsan_comb("${install_dir}" _prefix _comb _suffix)

    if(DEFINED _comb AND _comb)
      # Add sub-directory <comb> to output_dir
      set(_xsan_install_dir "${_xsan_install_dir}/${_comb}")
    endif()
  endif()

  set(${install_dir} ${_xsan_install_dir} PARENT_SCOPE)
endfunction()

function(add_xsan_runtime name type)
  add_compiler_rt_runtime(${name} ${type} ${ARGN})
  foreach(arch ${XSAN_SUPPORTED_ARCH})
    if(type STREQUAL "STATIC")
      set(libname "${name}-${arch}")
      set_output_name(output_name_${libname} ${name} ${arch})
    else()
      set(libname "${name}-dynamic-${arch}")
    endif()
    # Obtain OUTPUT_NAME
    get_target_property(_output_name ${libname} OUTPUT_NAME)
    _parse_xsan_comb("${_output_name}" _prefix _comb _suffix)
    # Remove comb from output_name
    # E.g., libclang_rt.xsan_asan_msan-x86_64.a -> libclang_rt.xsan-x86_64.a
    set(_output_name "${_prefix}xsan${_suffix}")
    set_target_properties(${libname} PROPERTIES
        OUTPUT_NAME ${_output_name}
        FOLDER "Compiler-RT/Runtimes")
  endforeach()
endfunction()

########## Set the sources for the XSan runtime library ##########

# Build for the XSan runtime support library.

# we now cannot support disable ASan, we can raise a cmake warning when detecting XSAN_CONTAINS_ASAN is OFF, and then set it to ON.
if(DEFINED XSAN_CONTAINS_ASAN AND NOT XSAN_CONTAINS_ASAN)
  message(WARNING
    "XSAN_CONTAINS_ASAN is OFF, but disabling ASan is currently not supported.\n"
    "ASan will be forcibly enabled. Support for disabling it may be added in the future."
  )
  set(XSAN_CONTAINS_ASAN ON CACHE BOOL "Enable AddressSanitizer (ASan) globally" FORCE)
endif()

# Import sub-sanitizer under `dir`
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
macro(import_subsan_wrapper dir output_prefix)
  unset(import_subsan)
  add_subdirectory(${dir})
  # imported from the subdirectory
  import_subsan(${output_prefix})

  transform_list_in_place(${output_prefix}_HEADERS PREPEND "${dir}/")
  transform_list_in_place(${output_prefix}_INCLUDE_DIRS PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/")
  transform_list_in_place(${output_prefix}_SOURCES PREPEND "${dir}/")
  transform_list_in_place(${output_prefix}_COND_SOURCES PREPEND "${dir}/")
  transform_list_in_place(${output_prefix}_STATIC_SOURCES PREPEND "${dir}/")
  transform_list_in_place(${output_prefix}_WRAPPED_SYMBOLS PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/")
  transform_list_in_place(${output_prefix}_SYMS_EXTRA PREPEND "${dir}/")
endmacro()

######################## Headers #########################

SET(XSAN_HEADERS
  xsan_activation.h
  xsan_allocator.h
  xsan_attribute.h
  xsan_flags.h
  xsan_hooks.h
  xsan_hooks_default.h
  xsan_hooks_dispatch.h
  xsan_hooks_gen.h
  xsan_hooks_types.h
  xsan_interceptors.h
  xsan_interceptors_memintrinsics.h
  xsan_interface_internal.h
  xsan_internal.h
  xsan_stack.h
  xsan_stack_interface.h
  xsan_thread.h
)

set(XSAN_INCLUDE_DIRS
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../
)

######################## Sources #########################

set(XSAN_SOURCES)

# sources using the XSAN_CONTAINS_* condition
set(XSAN_COND_SOURCES
  xsan_activation.cpp
  xsan_allocator.cpp
  xsan_disability_dummy.cpp
  xsan_flags.cpp
  xsan_hooks.cpp
  xsan_interceptors.cpp
  xsan_interceptors_memintrinsics.cpp
  xsan_interface.cpp
  xsan_linux.cpp
  xsan_malloc_linux.cpp
  xsan_posix.cpp
  xsan_rtl.cpp
  xsan_stack.cpp
  xsan_thread.cpp
)

if (NOT WIN32 AND NOT APPLE)
  list(APPEND XSAN_SOURCES
    xsan_interceptors_vfork.S # FIXME: TSan uses fork to replace vfork. Should we do the same?
    # asan/orig/asan_interceptors_vfork.S
  )
endif()

set(XSAN_CXX_SOURCES
  xsan_new_delete.cpp
)

set(XSAN_STATIC_SOURCES
  xsan_rtl_static.cpp
)

set(XSAN_PREINIT_SOURCES
  xsan_preinit.cpp
)

######################## Flags #########################

set(XSAN_CFLAGS ${SANITIZER_COMMON_CFLAGS})

append_rtti_flag(OFF XSAN_CFLAGS)

# Silence warnings in system headers with MSVC.
if(NOT CLANG_CL)
  append_list_if(COMPILER_RT_HAS_EXTERNAL_FLAG "/experimental:external;/external:W0;/external:anglebrackets" XSAN_CFLAGS)
endif()

# -mssse3 enables SIMD instructions that are used in TSan runtime.
append_list_if(COMPILER_RT_HAS_MSSE4_2_FLAG -msse4.2 XSAN_CFLAGS)

# Too many existing bugs, needs cleanup.
append_list_if(COMPILER_RT_HAS_WNO_FORMAT -Wno-format XSAN_CFLAGS)

set(XSAN_DYNAMIC_CFLAGS ${SANITIZER_COMMON_CFLAGS})
append_list_if(COMPILER_RT_HAS_FTLS_MODEL_INITIAL_EXEC -ftls-model=initial-exec XSAN_DYNAMIC_CFLAGS)
list(REMOVE_ITEM XSAN_DYNAMIC_CFLAGS -fPIE)

######################## Definitions #########################

set(XSAN_COMMON_DEFINITIONS)
set(XSAN_DYNAMIC_DEFINITIONS XSAN_DYNAMIC=1)

######################## Link Libraries #########################

set(XSAN_WRAPPED_SYMBOLS ${CMAKE_CURRENT_SOURCE_DIR}/xsan_wrapped_symbols.txt.in)
set(XSAN_SYMS_EXTRA xsan.syms.extra)
set(XSAN_LINK_LIBS)

#################### Import Sub-san ####################

set(INCLUDE_DIRS ${XSAN_INCLUDE_DIRS})

foreach(San IN LISTS XSAN_DELEGATED_SANITIZERS)
  string(TOLOWER "${San}" san)
  string(TOUPPER "${San}" SAN)
  import_subsan_wrapper(${san} ${SAN})
  list(APPEND INCLUDE_DIRS ${${SAN}_INCLUDE_DIRS})
endforeach()

include_directories(${INCLUDE_DIRS})

# Located in src/cmake/AddWrapSyms.cmake, to use create_wrap_symbols_target
include(AddWrapSyms)

if(MSVC)
  # asan on windows only supports the release dll version of the runtimes, in the interest of
  # only having one asan dll to support/test. Having asan statically linked
  # with the runtime might be possible, but it multiplies the number of scenerios to test.
  # the program USING sanitizers can use whatever version of the runtime it wants to.
  set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
endif()

######################## LDFLAGS #########################

set(XSAN_COMMON_LINK_FLAGS)
set(XSAN_COMMON_DYNAMIC_LINK_FLAGS ${SANITIZER_COMMON_LINK_FLAGS})

if(ANDROID)
  # Put most Sanitizer shared libraries in the global group. For more details, see
  # android-changes-for-ndk-developers.md#changes-to-library-search-order
  if (COMPILER_RT_HAS_Z_GLOBAL)
    list(APPEND XSAN_COMMON_DYNAMIC_LINK_FLAGS -Wl,-z,global)
  endif()
endif()
# LLVM turns /OPT:ICF back on when LLVM_ENABLE_PDBs is set
# we _REALLY_ need to turn it back off for ASAN, because the way
# asan emulates weak functions from DLLs requires NOICF
append_list_if(MSVC "LINKER:/DEBUG;LINKER:/OPT:NOICF" XSAN_COMMON_DYNAMIC_LINK_FLAGS)

################## Compilation setting from source files to object files ###############

# Compile common libraries. (preinit)
if(NOT APPLE)
  add_compiler_rt_object_libraries(RTXsan_preinit
    ARCHS ${XSAN_SUPPORTED_ARCH}
    SOURCES ${XSAN_PREINIT_SOURCES}
    ADDITIONAL_HEADERS ${XSAN_HEADERS}
    CFLAGS ${XSAN_CFLAGS}
    DEFS ${XSAN_COMMON_DEFINITIONS})

  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/dummy.cpp "")
  add_compiler_rt_object_libraries(RTXsan_dynamic_version_script_dummy
    ARCHS ${XSAN_SUPPORTED_ARCH}
    SOURCES ${CMAKE_CURRENT_BINARY_DIR}/dummy.cpp
    CFLAGS ${XSAN_DYNAMIC_CFLAGS}
    DEFS ${XSAN_DYNAMIC_DEFINITIONS})
endif()

# Compile individual parts of each sanitizer. (main lib, dynamic, static)
# Conditional parts are compiled when generating certain combinations.
set(XSAN_OBJ_SAN_LIST ${XSAN_DELEGATED_SANITIZERS} Xsan)
foreach(San IN LISTS XSAN_OBJ_SAN_LIST)
  string(TOLOWER "${San}" san)
  string(TOUPPER "${San}" SAN)

  add_compiler_rt_object_libraries(RTXsan_dynamic_${san}
    OS ${SANITIZER_COMMON_SUPPORTED_OS}
    ARCHS ${XSAN_SUPPORTED_ARCH}
    SOURCES ${${SAN}_SOURCES}
    ADDITIONAL_HEADERS ${${SAN}_HEADERS}
    CFLAGS ${${SAN}_DYNAMIC_CFLAGS}
    DEFS ${${SAN}_DYNAMIC_DEFINITIONS})

  if(NOT APPLE)
    add_compiler_rt_object_libraries(RTXsan_${san}
      ARCHS ${XSAN_SUPPORTED_ARCH}
      SOURCES ${${SAN}_SOURCES}
      ADDITIONAL_HEADERS ${${SAN}_HEADERS}
      CFLAGS ${${SAN}_CFLAGS}
      DEFS ${${SAN}_COMMON_DEFINITIONS})
    if(NOT "${${SAN}_STATIC_SOURCES}" STREQUAL "")
      add_compiler_rt_object_libraries(RTXsan_static_${san}
        ARCHS ${XSAN_SUPPORTED_ARCH}
        SOURCES ${${SAN}_STATIC_SOURCES}
        ADDITIONAL_HEADERS ${${SAN}_HEADERS}
        CFLAGS ${${SAN}_CFLAGS}
        DEFS ${${SAN}_COMMON_DEFINITIONS})
    endif()
  endif()
endforeach()

################## Linking setting from object files to the static XSan runtime library ###############

# Build XSan runtimes shipped with Clang.
add_compiler_rt_component(xsan)

if(NOT APPLE)
  set(XSAN_COMMON_RUNTIME_OBJECT_LIBS
    RTInterception
    RTSanitizerCommon
    RTSanitizerCommonLibc
    RTSanitizerCommonCoverage
    RTSanitizerCommonSymbolizer
    RTSanitizerCommonSymbolizerInternal
    RTLSanCommon
    RTUbsan)

  #---------- Generate common libraries. (preinit) ----------
  if (NOT WIN32)
    add_compiler_rt_runtime(clang_rt.xsan-preinit
        STATIC
        ARCHS ${XSAN_SUPPORTED_ARCH}
        OBJECT_LIBS RTXsan_preinit
        CFLAGS ${XSAN_CFLAGS}
        LINK_FLAGS ${XSAN_COMMON_LINK_FLAGS}
        PARENT_TARGET xsan)
  endif()

  # ----------- Generate RTL related to each XSan combination ----------

  # TODO: Support power set
  set(XSAN_STATIC_RTL_COMB ${XSAN_DELEGATED_SANITIZERS})
  set(XSAN_DYNAMIC_RTL_COMB ${XSAN_DELEGATED_SANITIZERS})
  # NOTE: Asan and Msan are explicitly excluded from the rtl combinations below
  # because their combined use case is not yet stable or is pending further validation.
  # This is a temporary measure and should be revisited in the future.
  list(REMOVE_ITEM XSAN_DYNAMIC_RTL_COMB Msan)
  list(REMOVE_ITEM XSAN_DYNAMIC_RTL_COMB Asan)
  list(REMOVE_ITEM XSAN_STATIC_RTL_COMB Asan)
  list_powerset("${XSAN_DYNAMIC_RTL_COMB}" DYNAMIC_POWER_SET)
  list_powerset("${XSAN_STATIC_RTL_COMB}" STATIC_POWER_SET)
  set(XSAN_STATIC_COMBINATIONS "${STATIC_POWER_SET}")
  set(XSAN_DYNAMIC_COMBINATIONS "${DYNAMIC_POWER_SET}")

  ################ Generate static libraries ##################################
  foreach(comb IN LISTS XSAN_STATIC_COMBINATIONS)
    set(XSAN_OBJ_LIST)
    set(XSAN_CXX_OBJ_LIST)
    set(XSAN_STATIC_OBJ_LIST)
    set(XSAN_CFLAGS_LIST)
    set(XSAN_WRAPPED_SYMBOLS_LIST)
    set(XSAN_SYMS_EXTRA_LIST)
    string(REPLACE "XSAN_SUBSET_" "" comb_suffix ${comb})
    string(TOLOWER "${comb_suffix}" comb_suffix)

    set(target_comb "xsan_${comb_suffix}")
    add_compiler_rt_component(${target_comb})
    add_dependencies(xsan ${target_comb})

    # Collect XSAN_CONTAINS_XXX of this combination
    create_xsan_combination_definitions(
        ${comb}        
        "${XSAN_STATIC_RTL_COMB}"   
        XSAN_DEFINITIONS_LIST
    )

    set(obj_san_list ${${comb}} Xsan)
    # Collect information of this combination
    foreach(San IN LISTS obj_san_list)
      string(TOLOWER "${San}" san)
      string(TOUPPER "${San}" SAN)
      list(APPEND XSAN_OBJ_LIST RTXsan_${san})
      if(NOT "${${SAN}_STATIC_SOURCES}" STREQUAL "")
        list(APPEND XSAN_STATIC_OBJ_LIST RTXsan_static_${san})
      endif()
      list(APPEND XSAN_CFLAGS_LIST ${${SAN}_CFLAGS})
      list(APPEND XSAN_WRAPPED_SYMBOLS_LIST ${${SAN}_WRAPPED_SYMBOLS})
      list(APPEND XSAN_SYMS_EXTRA_LIST ${${SAN}_SYMS_EXTRA})

      if(NOT "${${SAN}_COND_SOURCES}" STREQUAL "")
        # Compile parts using the XSAN_CONTAINS_* condition
        add_compiler_rt_object_libraries(RTXsan_${comb_suffix}_cond_${san}
          ARCHS ${XSAN_SUPPORTED_ARCH}
          SOURCES ${${SAN}_COND_SOURCES}
          ADDITIONAL_HEADERS ${${SAN}_HEADERS}
          CFLAGS ${${SAN}_CFLAGS}
          DEFS ${${SAN}_COMMON_DEFINITIONS} ${XSAN_DEFINITIONS_LIST})
        list(APPEND XSAN_OBJ_LIST RTXsan_${comb_suffix}_cond_${san})
      endif()

      if(NOT "${${SAN}_CXX_SOURCES}" STREQUAL "")
        add_compiler_rt_object_libraries(RTXsan_${comb_suffix}_cxx_${san}
          ARCHS ${XSAN_SUPPORTED_ARCH}
          SOURCES ${${SAN}_CXX_SOURCES}
          ADDITIONAL_HEADERS ${${SAN}_HEADERS}
          CFLAGS ${${SAN}_CFLAGS}
          DEFS ${${SAN}_COMMON_DEFINITIONS} ${XSAN_DEFINITIONS_LIST})
        list(APPEND XSAN_CXX_OBJ_LIST RTXsan_${comb_suffix}_cxx_${san})
      endif()
    endforeach()

    # message(STATUS "comb: ${${comb}}")
    # message(STATUS "XSAN_OBJ_LIST: ${XSAN_OBJ_LIST}")
    # message(STATUS "XSAN_STATIC_OBJ_LIST: ${XSAN_STATIC_OBJ_LIST}")
    # message(STATUS "XSAN_CFLAGS_LIST: ${XSAN_CFLAGS_LIST}")
    # message(STATUS "XSAN_WRAPPED_SYMBOLS_LIST: ${XSAN_WRAPPED_SYMBOLS_LIST}")

    # Add the following task as a custom target:
    # Generate wrapped symbols file, which contains all symbols that need to be
    # wrapped by _wrap=<symbol> linker option.
    set(wrap_symbol_target "generate_wrap_symbols_${comb_suffix}")
    create_wrap_symbols_target(
      ${wrap_symbol_target}
      NAME "xsan_wrapped_symbols.txt"
      OUT_SUBDIR "${target_comb}"
      IN ${XSAN_WRAPPED_SYMBOLS_LIST}
    )

    set(XSAN_COMMON_DEPS ${wrap_symbol_target})
    set(XSAN_LINK_FLAGS
      ${XSAN_COMMON_LINK_FLAGS}
      "-Wl,@${output_file}"
    )

    set(libname "clang_rt.${target_comb}")

    # Generate combinations of sanitizers. (main lib, cxx, static)
    if (NOT WIN32)
      add_xsan_runtime(${libname}
          STATIC
          ARCHS ${XSAN_SUPPORTED_ARCH}
          OBJECT_LIBS RTXsan_preinit
                      ${XSAN_OBJ_LIST}
                      ${XSAN_COMMON_RUNTIME_OBJECT_LIBS}
          CFLAGS ${XSAN_CFLAGS_LIST}
          LINK_FLAGS ${XSAN_LINK_FLAGS}
          DEPS ${XSAN_COMMON_DEPS}
          PARENT_TARGET ${target_comb})

      add_xsan_runtime(${libname}_cxx
          STATIC
          ARCHS ${XSAN_SUPPORTED_ARCH}
          OBJECT_LIBS ${XSAN_CXX_OBJ_LIST}
                      RTUbsan_cxx
          CFLAGS ${XSAN_CFLAGS}
          LINK_FLAGS ${XSAN_COMMON_LINK_FLAGS}
          PARENT_TARGET ${target_comb})

      add_xsan_runtime(${libname}_static
          STATIC
          ARCHS ${XSAN_SUPPORTED_ARCH}
          OBJECT_LIBS ${XSAN_STATIC_OBJ_LIST}
          CFLAGS ${XSAN_CFLAGS_LIST}
          LINK_FLAGS ${XSAN_LINK_FLAGS}
          DEPS ${XSAN_COMMON_DEPS}
          PARENT_TARGET ${target_comb})
    endif()

    # Generate *.syms for non-hidden symbols exports in DSO compilation
    foreach(arch ${XSAN_SUPPORTED_ARCH})
      if(SANITIZER_USE_SYMBOLS AND NOT ${arch} STREQUAL "i386")
        add_sanitizer_rt_symbols(${libname}
          ARCHS ${arch}
          EXTRA ${XSAN_SYMS_EXTRA_LIST})
        add_sanitizer_rt_symbols(${libname}_cxx
          ARCHS ${arch}
          EXTRA ${XSAN_SYMS_EXTRA_LIST})
        add_dependencies(${target_comb}
          ${libname}-${arch}-symbols
          ${libname}_cxx-${arch}-symbols)
      endif()
    endforeach()

    # ----------------- Generate/Install SymLinks -----------------

    # TODO: Currently, the implementation of XSan RTL heavily depends on the
    # functionality of ASan RTL, so the component of ASan cannot be removed.
    # Therefore, we symlink the 'ASan-free' RTL to the ASan version, and
    # leave the real removal of ASan RTL for future implementation.
    string(REGEX REPLACE "_asan" "" symlink_comb "${target_comb}")

    # output and install SymLink: share/xsan_*  ->  share/xsan_asan_*
    add_symlink(
      TRIGGER_TARGET ${wrap_symbol_target}
      SOURCE ${target_comb}
      LINK_NAME ${symlink_comb}
      OUT_DIR ${XSAN_OUTPUT_DATADIR}
      INSTALL_DIR ${XSAN_INSTALL_DATADIR}
    )

    foreach(arch ${XSAN_SUPPORTED_ARCH})

      set(_target_comb ${target_comb})
      unset(target_comb)

      # Get output directory, considering the combination
      get_compiler_rt_output_dir(${arch} out_dir)

      # Get install directory, not considering the combination
      get_compiler_rt_install_dir(${arch} install_dir)

      set(target_comb ${_target_comb})

      # output and install SymLink: lib/*/xsan_* -> lib/*/xsan_asan_*
      add_symlink(
        TRIGGER_TARGET ${target_comb}
        SOURCE ${target_comb}
        LINK_NAME ${symlink_comb}
        OUT_DIR ${out_dir}
        INSTALL_DIR ${install_dir}
      )

      # Set symbol link for clang_rt.xsan-preinit.
      # SymLink:   lib/*/xsan*/libclang_rt.xsan-preinit.a
      #         -> lib/*/libclang_rt.xsan-preinit.a
      add_target_symlink(
        SOURCE_TARGET clang_rt.xsan-preinit-${arch}
        REL_DEST_DIR ${target_comb}
        INSTALL_DIR ${install_dir}
      )
    endforeach()
  endforeach()

  ################ Generate dynamic libraries ##################################
  foreach(comb IN LISTS XSAN_DYNAMIC_COMBINATIONS)
    set(XSAN_HEADER_LIST)
    set(XSAN_DYNAMIC_OBJ_LIST)
    set(XSAN_DYNAMIC_CFLAGS_LIST)
    set(XSAN_LINK_LIBS_LIST)
    set(XSAN_SYMS_EXTRA_LIST)

    set(obj_san_list ${${comb}} Xsan)
    string(REPLACE "XSAN_SUBSET_" "" comb_suffix ${comb})
    string(TOLOWER "${comb_suffix}" comb_suffix)
    set(target_comb "xsan_${comb_suffix}")
    # Collect XSAN_CONTAINS_XXX of this combination
    create_xsan_combination_definitions(
        ${comb}        
        "${XSAN_DYNAMIC_RTL_COMB}"   
        XSAN_DEFINITIONS_LIST
    )
    # Collect information of this combination
    foreach(San IN LISTS obj_san_list)
      string(TOLOWER "${San}" san)
      string(TOUPPER "${San}" SAN)
      list(APPEND XSAN_HEADER_LIST ${${SAN}_HEADERS})
      list(APPEND XSAN_DYNAMIC_OBJ_LIST RTXsan_dynamic_${san})
      list(APPEND XSAN_DYNAMIC_CFLAGS_LIST ${${SAN}_DYNAMIC_CFLAGS})
      list(APPEND XSAN_LINK_LIBS_LIST ${${SAN}_LINK_LIBS})
      list(APPEND XSAN_SYMS_EXTRA_LIST ${${SAN}_SYMS_EXTRA})

      if(NOT "${${SAN}_COND_SOURCES}" STREQUAL "")
        # Compile parts using the XSAN_CONTAINS_* condition
        add_compiler_rt_object_libraries(RTXsan_${comb_suffix}_dynamic_cond_${san}
          OS ${SANITIZER_COMMON_SUPPORTED_OS}
          ARCHS ${XSAN_SUPPORTED_ARCH}
          SOURCES ${${SAN}_COND_SOURCES}
          ADDITIONAL_HEADERS ${${SAN}_HEADERS}
          CFLAGS ${${SAN}_DYNAMIC_CFLAGS}
          DEFS ${${SAN}_DYNAMIC_DEFINITIONS} ${XSAN_DEFINITIONS_LIST})
        list(APPEND XSAN_DYNAMIC_OBJ_LIST RTXsan_${comb_suffix}_dynamic_cond_${san})
      endif()

      if(NOT "${${SAN}_CXX_SOURCES}" STREQUAL "")
        add_compiler_rt_object_libraries(RTXsan_${comb_suffix}_cxx_dynamic_${san}
          OS ${SANITIZER_COMMON_SUPPORTED_OS}
          ARCHS ${XSAN_SUPPORTED_ARCH}
          SOURCES ${${SAN}_CXX_SOURCES}
          ADDITIONAL_HEADERS ${${SAN}_HEADERS}
          CFLAGS ${${SAN}_DYNAMIC_CFLAGS}
          DEFS ${${SAN}_DYNAMIC_DEFINITIONS} ${XSAN_DEFINITIONS_LIST})
        list(APPEND XSAN_DYNAMIC_OBJ_LIST RTXsan_${comb_suffix}_cxx_dynamic_${san})
      endif()
    endforeach()

    # message(STATUS "comb: ${${comb}}")
    # message(STATUS "XSAN_HEADER_LIST: ${XSAN_HEADER_LIST}")
    # message(STATUS "XSAN_DYNAMIC_OBJ_LIST: ${XSAN_DYNAMIC_OBJ_LIST}")
    # message(STATUS "XSAN_DYNAMIC_CFLAGS_LIST: ${XSAN_DYNAMIC_CFLAGS_LIST}")
    # message(STATUS "XSAN_LINK_LIBS_LIST: ${XSAN_LINK_LIBS_LIST}")

    # TODO: generate the wrapped symbols file when it doesn't exist.

    set(output_file "${XSAN_OUTPUT_DATADIR}/${target_comb}/xsan_wrapped_symbols.txt")
    set(XSAN_DYNAMIC_LINK_FLAGS
      ${XSAN_COMMON_DYNAMIC_LINK_FLAGS}
      "-Wl,@${output_file}"
    )

    set(libname "clang_rt.${target_comb}")

    ################ Dump the extern symbols in dynamic libraries ###############
    foreach(arch ${XSAN_SUPPORTED_ARCH})
      if (COMPILER_RT_HAS_VERSION_SCRIPT)
        if(WIN32)
          set(SANITIZER_RT_VERSION_LIST_LIBS ${libname}-${arch})
        else()
          set(SANITIZER_RT_VERSION_LIST_LIBS
            ${libname}-${arch}
            ${libname}_cxx-${arch})
        endif()
        add_sanitizer_rt_version_list(${libname}-dynamic-${arch}
                                      LIBS ${SANITIZER_RT_VERSION_LIST_LIBS}
                                      EXTRA ${XSAN_SYMS_EXTRA_LIST})
        set(VERSION_SCRIPT_FLAG
          -Wl,--version-script,${CMAKE_CURRENT_BINARY_DIR}/${libname}-dynamic-${arch}.vers)
        # The Solaris 11.4 linker supports a subset of GNU ld version scripts,
        # but requires a special option to enable it.
        if (COMPILER_RT_HAS_GNU_VERSION_SCRIPT_COMPAT)
            list(APPEND VERSION_SCRIPT_FLAG -Wl,-z,gnu-version-script-compat)
        endif()
        set_property(SOURCE
          ${CMAKE_CURRENT_BINARY_DIR}/dummy.cpp
          APPEND PROPERTY
          OBJECT_DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${libname}-dynamic-${arch}.vers)
      else()
        set(VERSION_SCRIPT_FLAG)
      endif()

      # Generate combinations of sanitizers. (dynamic)
      add_xsan_runtime(${libname}
        SHARED
        ARCHS ${arch}
        OBJECT_LIBS ${XSAN_COMMON_RUNTIME_OBJECT_LIBS}
                    ${XSAN_DYNAMIC_OBJ_LIST}
                    # The only purpose of RTAsan_dynamic_version_script_dummy is to
                    # carry a dependency of the shared runtime on the version script.
                    # Replacing it with a straightforward
                    # add_dependencies(clang_rt.asan-dynamic-${arch} clang_rt.asan-dynamic-${arch}-version-list)
                    # generates an order-only dependency in ninja.
                    RTXsan_dynamic_version_script_dummy
                    RTUbsan_cxx
        ADDITIONAL_HEADERS ${XSAN_HEADER_LIST}
        CFLAGS ${XSAN_DYNAMIC_CFLAGS_LIST}
        LINK_FLAGS ${XSAN_DYNAMIC_LINK_FLAGS}
                  ${VERSION_SCRIPT_FLAG}
        LINK_LIBS ${XSAN_LINK_LIBS_LIST}
        PARENT_TARGET ${target_comb})
    endforeach()
  endforeach()
endif()


# generate xsan_hooks_gen.h automatically
set(CONFIG_FILE_PATH "${CMAKE_SOURCE_DIR}/src/runtime/lib/xsan/xsan_hooks_gen.cmake")

if(EXISTS ${CONFIG_FILE_PATH})
  include(${CONFIG_FILE_PATH})
else()
  message(STATUS "Configuration file not found, can not auto-generate the xsan_hooks_gen.h file.")
endif()

# Unset target_comb variable to avoid conflicts in other sanitizers
# Because our overrided `get_compiler_rt_install_dir`/`get_compiler_rt_output_dir`
# will use this variable to control whether to add sub-directory <comb>.
unset(target_comb)
