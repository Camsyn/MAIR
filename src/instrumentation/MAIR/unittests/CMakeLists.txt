include(AddLLVM)

add_custom_target(MAIRUnitTests)
set_target_properties(MAIRUnitTests PROPERTIES FOLDER "Compiler-RT/Tests")

set(LLVM_LINK_COMPONENTS
  Analysis
  AsmParser
  Core
  TransformUtils
  Support
)

set(MAIR_GTEST_DIR ${LLVM_MAIN_SRC_DIR}/../third-party/unittest/googletest)
if(EXISTS ${MAIR_GTEST_DIR}/src/gtest-all.cc)
  add_llvm_executable(MAIRGraphTest
    MAIRGraphTest.cpp
    MAIRMaterializationTest.cpp
    CommonMAMotionPassTest.cpp
    LocationMergePassTest.cpp
    LoopInvariantObservationMotionPassTest.cpp
    LoopPeriodicObservationCombinePassTest.cpp
    RedundantObservationEliminationPassTest.cpp
    SimplifyGraphPassTest.cpp
    ${MAIR_GTEST_DIR}/src/gtest-all.cc
    ${MAIR_GTEST_DIR}/src/gtest_main.cc
    $<TARGET_OBJECTS:MAIRCore>
  )
  target_link_libraries(MAIRGraphTest PRIVATE ${LLVM_PTHREAD_LIB})
  add_dependencies(MAIRUnitTests MAIRGraphTest)
else()
  add_unittest(MAIRUnitTests MAIRGraphTest
    MAIRGraphTest.cpp
    MAIRMaterializationTest.cpp
    CommonMAMotionPassTest.cpp
    LocationMergePassTest.cpp
    LoopInvariantObservationMotionPassTest.cpp
    LoopPeriodicObservationCombinePassTest.cpp
    RedundantObservationEliminationPassTest.cpp
    SimplifyGraphPassTest.cpp
    $<TARGET_OBJECTS:MAIRCore>
  )
endif()

target_include_directories(MAIRGraphTest BEFORE PRIVATE
  ${CMAKE_SOURCE_DIR}/src/instrumentation
  ${LLVM_BINARY_DIR}/include
  ${LLVM_MAIN_INCLUDE_DIR}
  ${MAIR_GTEST_DIR}/include
  ${MAIR_GTEST_DIR}
)

add_custom_target(check-mair-unit
  COMMAND $<TARGET_FILE:MAIRGraphTest> --gtest_brief=1
  DEPENDS MAIRGraphTest
  USES_TERMINAL
)
set_target_properties(check-mair-unit PROPERTIES FOLDER "Compiler-RT/Tests")
