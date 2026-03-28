# -------------------------------------------------------------------
# 1. Check LLVM version
# -------------------------------------------------------------------

find_package(LLVM REQUIRED CONFIG)

if(NOT LLVM_FOUND)
  message(FATAL_ERROR
    "find_package(LLVM) failed, please ensure LLVM_DIR points to the cmake config directory of a supported LLVM release:\n"
    "  - LLVM 15\n"
    "  - LLVM 22+\n"
    "  e.g. -DLLVM_DIR=/path/to/llvm/lib/cmake/llvm"
  )
endif()

set(XSAN_LLVM_MAJOR "${LLVM_VERSION_MAJOR}")
if(NOT XSAN_LLVM_MAJOR)
  string(REGEX REPLACE "^([0-9]+).*" "\\1" XSAN_LLVM_MAJOR "${LLVM_PACKAGE_VERSION}")
endif()

if(XSAN_LLVM_MAJOR EQUAL 15)
  set(XSAN_INSTRUMENTATION_VARIANT "llvm15")
  set(XSAN_LIVEPATCH_VARIANT "clang-15")
elseif(XSAN_LLVM_MAJOR GREATER_EQUAL 22)
  set(XSAN_INSTRUMENTATION_VARIANT "llvm23")
  set(XSAN_LIVEPATCH_VARIANT "clang-23")
else()
  message(FATAL_ERROR
    "Detected LLVM version ${LLVM_PACKAGE_VERSION}, but XSan currently supports LLVM 15 and LLVM 22+.\n"
    "Please specify a supported LLVM via -DLLVM_DIR=/path/to/llvm/lib/cmake/llvm"
  )
endif()

message_green("Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")
message_green("Using instrumentation backend ${XSAN_INSTRUMENTATION_VARIANT}")
message_green("Using clang livepatch backend ${XSAN_LIVEPATCH_VARIANT}")

set(XSAN_INSTRUMENTATION_VARIANT "${XSAN_INSTRUMENTATION_VARIANT}" CACHE INTERNAL
    "Instrumentation backend selected from LLVM version")
set(XSAN_LIVEPATCH_VARIANT "${XSAN_LIVEPATCH_VARIANT}" CACHE INTERNAL
    "Clang livepatch backend selected from LLVM version")

# -------------------------------------------------------------------
# 2. Ensure the build compiler matches the selected LLVM major
# -------------------------------------------------------------------
function(derive_clangxx clang_bin OUT_CXX_PATH_VAR)
  get_filename_component(CLANG_DIR "${clang_bin}" DIRECTORY)
  get_filename_component(CLANG_FILENAME "${clang_bin}" NAME)

  if(NOT CLANG_FILENAME MATCHES "^clang")
    message(WARNING "Input path '${clang_bin}' does not appear to be a clang compiler.")
    set(${OUT_CXX_PATH_VAR} "" PARENT_SCOPE)
    return()
  endif()

  string(REGEX REPLACE "^clang" "clang++" CLANG_PP_FILENAME "${CLANG_FILENAME}")

  if(CLANG_DIR STREQUAL "")
    set(CANDIDATE_CXX_PATH "${CLANG_PP_FILENAME}")
  else()
    set(CANDIDATE_CXX_PATH "${CLANG_DIR}/${CLANG_PP_FILENAME}")
  endif()

  if(EXISTS "${CANDIDATE_CXX_PATH}")
    set(${OUT_CXX_PATH_VAR} "${CANDIDATE_CXX_PATH}" PARENT_SCOPE)
  else()
    set(${OUT_CXX_PATH_VAR} "" PARENT_SCOPE)
  endif()
endfunction()

function(find_and_verify_clang_version required_major out_var)
  set(_candidates
    ${LLVM_TOOLS_BINARY_DIR}/clang
    clang-${required_major}
    clang
  )

  foreach(_cmd IN LISTS _candidates)
    if("${_cmd}" STREQUAL "")
      continue()
    endif()

    message_green("\tChecking ${_cmd} ...")
    unset(_path CACHE)
    find_program(_path NAMES ${_cmd})

    if(NOT _path)
      if(IS_ABSOLUTE "${_cmd}" AND EXISTS "${_cmd}" AND NOT IS_DIRECTORY "${_cmd}")
        set(_path "${_cmd}")
      endif()
    endif()

    if(NOT _path)
      continue()
    endif()

    execute_process(
      COMMAND "${_path}" --version
      OUTPUT_VARIABLE _ver_out
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    string(REGEX MATCH "([Vv]ersion[[:space:]]+)?${required_major}([\\.[:digit:]]*)" _match "${_ver_out}")
    if(NOT _match)
      continue()
    endif()

    derive_clangxx("${_path}" _clang_cxx_path)
    if(_clang_cxx_path STREQUAL "")
      continue()
    endif()

    set(${out_var} "${_path}" PARENT_SCOPE)
    return()
  endforeach()

  set(${out_var} "" PARENT_SCOPE)
endfunction()

if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang"
  OR NOT CMAKE_C_COMPILER_VERSION MATCHES "^${XSAN_LLVM_MAJOR}(\\.|$)")
  message_green("Current C compiler: ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
  message_green("Need Clang ${XSAN_LLVM_MAJOR}, trying to find it automatically...")
  set(_clang_path "")
  find_and_verify_clang_version(${XSAN_LLVM_MAJOR} _clang_path)

  if(NOT "${_clang_path}" STREQUAL "")
    derive_clangxx("${_clang_path}" _clang_cxx_path)
    set(CMAKE_C_COMPILER "${_clang_path}" CACHE STRING "C compiler" FORCE)
    set(CMAKE_CXX_COMPILER "${_clang_cxx_path}" CACHE STRING "C++ compiler" FORCE)
    message_green("Found Clang${XSAN_LLVM_MAJOR}: ${_clang_path}, please switch to it")
    message_green("Found Clang++${XSAN_LLVM_MAJOR}: ${_clang_cxx_path}, please switch to it")
    message(FATAL_ERROR
      "Please run the following CMake command to specify clang-${XSAN_LLVM_MAJOR} as the compiler:\n"
      "  cmake -DCMAKE_C_COMPILER=${_clang_path} \\\n"
      "        -DCMAKE_CXX_COMPILER=${_clang_cxx_path} \\\n"
      "        <other options>\n"
      "or set CC/CXX environment variables before cmake configure:\n"
      "  CC=${_clang_path} \\\n"
      "  CXX=${_clang_cxx_path} \\\n"
      "  cmake <other options>\n"
    )
  else()
    message(FATAL_ERROR
      "Cannot find Clang-${XSAN_LLVM_MAJOR} compiler.\n"
      "Please specify it via CC/CXX or CMAKE_C_COMPILER/CMAKE_CXX_COMPILER.\n"
    )
  endif()
endif()
