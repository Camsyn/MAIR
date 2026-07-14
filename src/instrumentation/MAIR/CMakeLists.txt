set(MAIR_SOURCES
  Analysis/LoopAnalysis.cpp
  Analysis/MAIRStatAnalysis.cpp
  Analysis/RedundantObservationAnalysis.cpp
  Transforms/CommonMAMotionPass.cpp
  Transforms/LocationMergePass.cpp
  Transforms/LoopInvariantObservationMotionPass.cpp
  Transforms/LoopRangeAggregationPass.cpp
  Transforms/RedundantObservationEliminationPass.cpp
  Transforms/SimplifyGraphPass.cpp
  MAIR.cpp
  MAIRBarrierAnalysis.cpp
  MAIRDump.cpp
  MAIRPasses.cpp
  Verifier.cpp
)

add_library(MAIRCore OBJECT ${MAIR_SOURCES})
set_target_properties(MAIRCore PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(MAIRCore PRIVATE -fno-rtti)
llvm_update_compile_flags(MAIRCore)
target_include_directories(MAIRCore BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${LLVM_BINARY_DIR}/include
  ${LLVM_MAIN_INCLUDE_DIR}
)

add_subdirectory(unittests)
add_subdirectory(tools/mair-opt)
