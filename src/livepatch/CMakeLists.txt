set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


# #############################################################################
# ###   Install LLVMSupport and LLVMDemangle if they are shared libraries  ####
# #############################################################################
# Please refer to to $LLVM_DIR/lib/cmake/llvm/LLVMExports.cmake
# to see the imported targets of LLVM.

# Shared livepatch infrastructure consumed by both compiler-side early patching
# and instrumentation-side late patching.
set(XSAN_LIVEPATCH_LLVM_DYLIB_VERSION_SCRIPT
    "${CMAKE_CURRENT_LIST_DIR}/simple_version_script.map.in")

get_target_property(_llvmsupport_type LLVMSupport TYPE)
if(TARGET LLVM)
  get_target_property(_llvm_dylib_type LLVM TYPE)
endif()

# If LLVMSupport is a shared library, user might get the error when using livepatch for gcc,
# which relies on LLVMSupport, as follows:
# gcc: error while loading shared libraries: libLLVMSupport.so.15:
# cannot open shared object file: No such file or directory
option(LINK_DYNAMIC_LLVMSUPPORT "Link dynamic LLVMSupport library" OFF)
message_green("_llvmsupport_type: ${_llvmsupport_type}")

if(_llvmsupport_type STREQUAL "SHARED_LIBRARY")
  set(LINK_DYNAMIC_LLVMSUPPORT ON CACHE BOOL "Link dynamic LLVMSupport library")

  # lib*-patch.so relies on libLLVMSupport.so, and libLLVMSupport.so relies on
  # libLLVMDemangle.so. Install both adjacent to XSan's runtime lib directory
  # and let DSO consumers reach them via $ORIGIN/../lib.
  get_target_property(LLVMSupport_LOCATION LLVMSupport LOCATION)
  get_target_property(LLVMDemangle_LOCATION LLVMDemangle LOCATION)

  install(FILES ${LLVMSupport_LOCATION} DESTINATION ${XSAN_INSTALL_RUNTIME_DIR}
          COMPONENT patch)
  install(FILES ${LLVMDemangle_LOCATION} DESTINATION ${XSAN_INSTALL_RUNTIME_DIR}
          COMPONENT patch)
endif()

if(LLVM_LINK_LLVM_DYLIB AND TARGET LLVM AND
   (_llvm_dylib_type STREQUAL "SHARED_LIBRARY" OR
    _llvm_dylib_type STREQUAL "MODULE_LIBRARY"))
  get_target_property(LLVM_DYLIB_LOCATION LLVM LOCATION)
  install(FILES ${LLVM_DYLIB_LOCATION} DESTINATION ${XSAN_INSTALL_RUNTIME_DIR}
          COMPONENT patch)
endif()

set(XSAN_LIVEPATCH_HEADERS
  ${CMAKE_CURRENT_SOURCE_DIR}/PatchCore.h
  ${CMAKE_CURRENT_SOURCE_DIR}/utils/ELFHelper.h
  ${CMAKE_CURRENT_SOURCE_DIR}/utils/elf/AbstractELFParser.h
  ${CMAKE_CURRENT_SOURCE_DIR}/utils/elf/ELFParserLLVM.h
  ${CMAKE_CURRENT_SOURCE_DIR}/utils/elf/ELFParserLibElf.h
  ${CMAKE_CURRENT_SOURCE_DIR}/utils/elf/ELFParserNoDep.h
)
set_property(GLOBAL PROPERTY XSAN_LIVEPATCH_HEADERS
  ${XSAN_LIVEPATCH_HEADERS}
)
set(XSAN_LIVEPATCH_HEADERS ${XSAN_LIVEPATCH_HEADERS} PARENT_SCOPE)

add_library(LivePatchCore STATIC
  PatchCore.cpp
  ${XSAN_LIVEPATCH_HEADERS}
)
add_library(livepatch-core ALIAS LivePatchCore)

target_include_directories(LivePatchCore
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/compiler/include
    ${CMAKE_SOURCE_DIR}/src/include
    ${LLVM_INCLUDE_DIRS}
  PRIVATE
    ${CMAKE_BINARY_DIR}/src/compiler
)

if(LLVM_LINK_LLVM_DYLIB)
  target_link_libraries(LivePatchCore PRIVATE LLVM)
else()
  # LLVMSupport covers the explicit utility dependencies here; unresolved LLVM
  # IR/transform symbols are satisfied by the host compiler process.
  target_link_libraries(LivePatchCore PRIVATE LLVMSupport)
endif()

set_target_properties(LivePatchCore PROPERTIES
  ARCHIVE_OUTPUT_DIRECTORY "${XSAN_OUTPUT_PATCHDIR}"
  POSITION_INDEPENDENT_CODE ON
)

function(xsan_livepatch_link_core target)
  if(NOT TARGET ${target})
    message(FATAL_ERROR "xsan_livepatch_link_core: unknown target '${target}'")
  endif()

  target_link_libraries(${target} PRIVATE LivePatchCore dl)
endfunction()

function(xsan_livepatch_append_shared_llvm_rpath target)
  if(NOT TARGET ${target})
    message(FATAL_ERROR
      "xsan_livepatch_append_shared_llvm_rpath: unknown target '${target}'")
  endif()

  get_target_property(_xsan_target_type ${target} TYPE)
  if(NOT LINK_DYNAMIC_LLVMSUPPORT OR
     NOT (_xsan_target_type STREQUAL "SHARED_LIBRARY" OR
          _xsan_target_type STREQUAL "MODULE_LIBRARY"))
    return()
  endif()

  get_target_property(_xsan_install_rpath ${target} INSTALL_RPATH)
  if(NOT _xsan_install_rpath)
    set(_xsan_install_rpath "")
  endif()

  list(FIND _xsan_install_rpath "\$ORIGIN/../lib" _xsan_rpath_index)
  if(_xsan_rpath_index EQUAL -1)
    list(APPEND _xsan_install_rpath "\$ORIGIN/../lib")
    set_target_properties(${target} PROPERTIES
      INSTALL_RPATH "${_xsan_install_rpath}"
    )
  endif()
endfunction()

function(add_dso_livepatch target)
  set(_xsan_options)
  set(_xsan_one_value_args)
  set(_xsan_multi_value_args
    SOURCES
    EXTRA_DEPS
    EXTRA_SHARED_LINK_LIBS
    COMPILE_DEFINITIONS
    LINK_OPTIONS
  )
  cmake_parse_arguments(XSAN_DSO
    "${_xsan_options}"
    "${_xsan_one_value_args}"
    "${_xsan_multi_value_args}"
    ${ARGN}
  )

  if(NOT XSAN_DSO_SOURCES)
    message(FATAL_ERROR "add_dso_livepatch(${target} ...): missing SOURCES")
  endif()

  # All compiler-side livepatches are DSOs injected into the host compiler
  # process, so default to MODULE here instead of asking each caller to repeat
  # `add_library(... MODULE ...)`.
  add_library(${target} MODULE ${XSAN_DSO_SOURCES})
  if(TARGET livepatch)
    add_dependencies(livepatch ${target})
  endif()

  xsan_livepatch_link_core(${target})
  xsan_livepatch_append_shared_llvm_rpath(${target})

  if(XSAN_DSO_COMPILE_DEFINITIONS)
    target_compile_definitions(${target} PRIVATE ${XSAN_DSO_COMPILE_DEFINITIONS})
  endif()
  if(XSAN_DSO_LINK_OPTIONS)
    target_link_options(${target} PRIVATE ${XSAN_DSO_LINK_OPTIONS})
  endif()

  if(target STREQUAL "clang-patch")
    # Clang driver patches have an extra invariant beyond generic LLVM
    # dependency handling: when LLVM is linked as a shared dylib, clang's own
    # CMake contract also requires CLANG_LINK_CLANG_DYLIB to be enabled.
    #
    # GCC livepatches also use LLVM utility code through LivePatchCore, but
    # they patch GCC process symbols rather than versioned clang/libLLVM
    # driver symbols. Therefore GCC only needs the generic LLVM/LLVMSupport
    # dependency and RPATH handling below; do not apply clang's dylib guard or
    # LLVM symbol-version script to gcc-patch.
    if(LLVM_LINK_LLVM_DYLIB)
      if(NOT CLANG_LINK_CLANG_DYLIB)
        message(FATAL_ERROR
          "Cannot set CLANG_LINK_CLANG_DYLIB=OFF when LLVM_LINK_LLVM_DYLIB=ON")
      endif()
      target_compile_definitions(${target} PRIVATE -DCLANG_PATCH_LINK_DYLIB)

      # When clang/LLVM is built as dylibs, their exported C++ symbols may be
      # ELF-versioned under a namespace such as LLVM_15.0 or LLVM_23.0. The
      # clang livepatch DSO exports replacement/interceptor symbols that must
      # satisfy lookups for those same versioned symbols. Without this version
      # script, libclang-patch.so would export unversioned symbols, which can
      # fail to interpose or resolve against a versioned clang/libLLVM host.
      #
      # GCC livepatches do not target clang/libLLVM symbols; they only use LLVM
      # utility code internally through LivePatchCore, so they should not get
      # this LLVM symbol-version namespace.
      if(NOT DEFINED LLVM_SHLIB_SYMBOL_VERSION)
        set(LLVM_SHLIB_SYMBOL_VERSION
            "LLVM_${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}")
      endif()

      message(STATUS "LLVM_SHLIB_SYMBOL_VERSION: ${LLVM_SHLIB_SYMBOL_VERSION}")
      set(_xsan_version_script_output
        ${CMAKE_CURRENT_BINARY_DIR}/simple_version_script.map)
      configure_file(${XSAN_LIVEPATCH_LLVM_DYLIB_VERSION_SCRIPT}
                     ${_xsan_version_script_output} @ONLY)
      target_link_options(${target} PRIVATE
        -Wl,--version-script,${_xsan_version_script_output})
    elseif(CLANG_LINK_CLANG_DYLIB)
      message(FATAL_ERROR
        "Cannot set CLANG_LINK_CLANG_DYLIB=ON when LLVM_LINK_LLVM_DYLIB=OFF")
    endif()
  endif()

  if(LLVM_LINK_LLVM_DYLIB)
    set(_xsan_shared_link_libs LLVM)
    list(APPEND _xsan_shared_link_libs ${XSAN_DSO_EXTRA_SHARED_LINK_LIBS})
    # For shared-LLVM toolchains, linking against the imported `LLVM` dylib is
    # enough. When that dylib is actually a .so, the install step above places
    # it next to XSan's runtime lib directory, and patch DSOs reach it through
    # $ORIGIN/../lib.
    target_link_libraries(${target} PRIVATE ${_xsan_shared_link_libs})
  else()
    # For static-LLVM toolchains, LLVMSupport is the common minimum
    # dependency shared by compiler-side livepatches. Callers only need to
    # specify additional dependency libraries required by their own intercepted
    # symbols; the libraries passed through EXTRA_DEPS are not necessarily
    # static libraries themselves.
    set(_xsan_static_link_libs LLVMSupport)
    list(APPEND _xsan_static_link_libs ${XSAN_DSO_EXTRA_DEPS})
    target_link_libraries(${target} PRIVATE ${_xsan_static_link_libs})
  endif()

  set_target_properties(${target} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${XSAN_OUTPUT_PATCHDIR}"
  )

  install(TARGETS ${target}
    LIBRARY
    DESTINATION ${XSAN_INSTALL_PATCHDIR}
    COMPONENT patch
  )
endfunction()

install(TARGETS LivePatchCore
  ARCHIVE
  DESTINATION ${XSAN_INSTALL_PATCHDIR}
  COMPONENT patch
)
