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
set_output_directory(mair-dump
  BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}
  LIBRARY_DIR ${CMAKE_CURRENT_BINARY_DIR}
)

target_include_directories(mair-dump BEFORE PRIVATE
  ${CMAKE_SOURCE_DIR}/src/instrumentation
  ${LLVM_BINARY_DIR}/include
  ${LLVM_MAIN_INCLUDE_DIR}
)
