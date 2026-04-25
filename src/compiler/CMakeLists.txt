include_directories(include)
include_directories(${CMAKE_CURRENT_BINARY_DIR})
configure_file(config_compile.h.in config_compile.h)

add_custom_target(compiler)

# ################ For clang_wrapper.c #################
add_executable(xclang clang_wrapper.c)
add_dependencies(compiler xclang)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  # Suppress warning: `u8* to char*`, which is common in clang_wrapper.c
  target_compile_options(xclang PRIVATE -Wno-pointer-sign)
  target_compile_definitions(xclang PRIVATE ${XSAN_DEFINITIONS_TO_ADD})
endif()

# Set output directory
set_target_properties(xclang
  PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${XSAN_OUTPUT_BINDIR}"
)

add_symlink(
  TRIGGER_TARGET xclang
  SOURCE xclang
  LINK_NAME xclang++
  OUT_DIR ${XSAN_OUTPUT_BINDIR}
  INSTALL_DIR ${XSAN_INSTALL_BINDIR}
)

# ################ For gcc_wrapper.c  #################
add_executable(xgcc gcc_wrapper.c)
add_dependencies(compiler xgcc)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  # Suppress warning: `u8* to char*`, which is common in gcc_wrapper.c
  target_compile_options(xgcc PRIVATE -Wno-pointer-sign)
  target_compile_definitions(xgcc PRIVATE ${XSAN_DEFINITIONS_TO_ADD})
endif()

set_target_properties(xgcc
  PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${XSAN_OUTPUT_BINDIR}"
)

add_symlink(
  TRIGGER_TARGET xgcc
  SOURCE xgcc
  LINK_NAME xg++
  OUT_DIR ${XSAN_OUTPUT_BINDIR}
  INSTALL_DIR ${XSAN_INSTALL_BINDIR}
)

# ################ For unsupported sanitizers in gcc #################
set(GCC_UNSUPPORTED_SANITIZERS ${XSAN_SANITIZERS})
list(REMOVE_ITEM GCC_UNSUPPORTED_SANITIZERS ${GCC_SUPPORT_SANITIZERS})

foreach(unsupported_san IN LISTS GCC_UNSUPPORTED_SANITIZERS)
  target_compile_definitions(xgcc PRIVATE "COMPILER_UNSUPPORT_${unsupported_san}=1")
endforeach()

# ################ For unsupported sanitizers in clang #################
set(CLANG_UNSUPPORTED_SANITIZERS ${XSAN_SANITIZERS})
list(REMOVE_ITEM CLANG_UNSUPPORTED_SANITIZERS ${CLANG_SUPPORT_SANITIZERS})

foreach(unsupported_san IN LISTS CLANG_UNSUPPORTED_SANITIZERS)
  target_compile_definitions(xclang PRIVATE "COMPILER_UNSUPPORT_${unsupported_san}=1")
endforeach()

# ################ For install #################

# --- Step 1: put executable files to ${XSAN_INSTALL_BINDIR} ---
# Install from targets
install(TARGETS xclang xgcc
  RUNTIME # files required by runtime, i.e., executable/DLL
  DESTINATION ${XSAN_INSTALL_BINDIR}
  COMPONENT compiler
)

# --- Step 2: create symlink from ${CMAKE_INSTALL_BINDIR} to ${XSAN_INSTALL_BINDIR} ---
# Create symlink, e.g., bin/xclang -> ../lib/xsan/xclang
# Define a list of all executable names that need symlinks
set(XSAN_EXECUTABLES xclang xgcc xclang++ xg++)

foreach(EXE_NAME IN LISTS XSAN_EXECUTABLES)
  install_symlink(
    LINK_NAME ${EXE_NAME}
    TARGET_PATH ../${XSAN_INSTALL_BINDIR}/${EXE_NAME}
    WORK_DIR ${CMAKE_INSTALL_BINDIR}
    COMPONENT compiler
  )
endforeach()

# ################ For LivePatch #################
if(${XSAN_USE_LIVEPATCH})
  add_subdirectory(livepatch)
endif()
