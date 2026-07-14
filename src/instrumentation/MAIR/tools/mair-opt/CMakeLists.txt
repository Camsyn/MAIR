include(AddLLVM)

set(LLVM_LINK_COMPONENTS
  Analysis
  Core
  IRReader
  TransformUtils
  Support
)

add_llvm_executable(mair-opt
  mair-opt.cpp
  $<TARGET_OBJECTS:MAIRCore>
)
add_dependencies(compiler mair-opt)

set_target_properties(mair-opt
  PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${XSAN_OUTPUT_BINDIR}"
)

target_include_directories(mair-opt BEFORE PRIVATE
  ${CMAKE_SOURCE_DIR}/src/instrumentation
  ${LLVM_BINARY_DIR}/include
  ${LLVM_MAIN_INCLUDE_DIR}
)

install(TARGETS mair-opt
  RUNTIME
  DESTINATION ${XSAN_INSTALL_BINDIR}
  COMPONENT compiler
)

install_symlink(
  LINK_NAME mair-opt
  TARGET_PATH ../${XSAN_INSTALL_BINDIR}/mair-opt
  WORK_DIR ${CMAKE_INSTALL_BINDIR}
  COMPONENT compiler
)
