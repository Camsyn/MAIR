include(AddLLVM)

set(LLVM_LINK_COMPONENTS
  Analysis
  Core
  IRReader
  Support
)

add_llvm_executable(mair-dump
  mair-dump.cpp
  $<TARGET_OBJECTS:MAIRCore>
)
add_dependencies(compiler mair-dump)

set_target_properties(mair-dump
  PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${XSAN_OUTPUT_BINDIR}"
)

target_include_directories(mair-dump BEFORE PRIVATE
  ${CMAKE_SOURCE_DIR}/src/instrumentation
  ${LLVM_BINARY_DIR}/include
  ${LLVM_MAIN_INCLUDE_DIR}
)

install(TARGETS mair-dump
  RUNTIME
  DESTINATION ${XSAN_INSTALL_BINDIR}
  COMPONENT compiler
)

install_symlink(
  LINK_NAME mair-dump
  TARGET_PATH ../${XSAN_INSTALL_BINDIR}/mair-dump
  WORK_DIR ${CMAKE_INSTALL_BINDIR}
  COMPONENT compiler
)
