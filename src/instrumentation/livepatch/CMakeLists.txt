# Instrumentation livepatch is intentionally different from compiler
# livepatches. Compiler livepatches are PRELOAD DSOs and can cover a symbol's
# full lifetime by combining dynamic symbol interposition with dynamic binary
# rewriting. This instrumentation livepatch is linked into XSanInstPass.so
# instead: it does not interpose dynamic symbols, and only temporarily takes
# over selected symbols via dynamic binary rewriting while a SubSanitizer is
# running.
set(instr_latepatch_impl_sources
  ${CMAKE_CURRENT_SOURCE_DIR}/record/Interception.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/registry/PassBuilderPipeline.cpp
)

if(XSAN_INSTRUMENTATION_VARIANT STREQUAL "llvm15")
  list(APPEND instr_latepatch_impl_sources
    ${CMAKE_CURRENT_SOURCE_DIR}/record/llvm15/Local.cpp
  )
endif()

add_library(instr-livepatch-objects OBJECT
  ${instr_latepatch_impl_sources}
)

set_target_properties(instr-livepatch-objects PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)

target_include_directories(instr-livepatch-objects PRIVATE
  ${CMAKE_SOURCE_DIR}/src/instrumentation
  ${CMAKE_SOURCE_DIR}/src/livepatch
  ${CMAKE_SOURCE_DIR}/src/include
  ${LLVM_INCLUDE_DIRS}
)

set(XSAN_INSTRUMENTATION_LIVEPATCH_OBJECTS
  $<TARGET_OBJECTS:instr-livepatch-objects>
  PARENT_SCOPE
)
