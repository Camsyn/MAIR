include(AddLLVM)

add_custom_target(MAIRUnitTests)
set_target_properties(MAIRUnitTests PROPERTIES FOLDER "Compiler-RT/Tests")

set(LLVM_LINK_COMPONENTS
  Analysis
  AsmParser
  Core
  Support
)

add_unittest(MAIRUnitTests MAIRGraphTest
  MAIRGraphTest.cpp
  $<TARGET_OBJECTS:MAIRCore>
)

target_include_directories(MAIRGraphTest BEFORE PRIVATE
  ${CMAKE_SOURCE_DIR}/src/instrumentation
  ${LLVM_BINARY_DIR}/include
  ${LLVM_MAIN_INCLUDE_DIR}
)
