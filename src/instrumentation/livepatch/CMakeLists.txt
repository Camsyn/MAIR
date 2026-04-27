# Instrumentation livepatch is intentionally different from compiler
# livepatches. Compiler livepatches are PRELOAD DSOs and can cover a symbol's
# full lifetime by combining dynamic symbol interposition with dynamic binary
# rewriting. This instrumentation livepatch is linked into XSanInstPass.so
# instead: it does not interpose dynamic symbols, and only temporarily takes
# over selected symbols via dynamic binary rewriting while a SubSanitizer is
# running.
set(instr_latepatch_impl_sources
  ${CMAKE_CURRENT_SOURCE_DIR}/DeSanInterceptors.cpp
)

if(XSAN_INSTRUMENTATION_VARIANT STREQUAL "llvm15")
  list(APPEND instr_latepatch_impl_sources
    ${CMAKE_CURRENT_SOURCE_DIR}/llvm15/Local.cpp
  )
endif()

add_library(instr-livepatch-objects OBJECT
  ${instr_latepatch_impl_sources}
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

function(xsan_instrumentation_enable_livepatch target)
  xsan_livepatch_link_core(${target})
  xsan_livepatch_append_shared_llvm_rpath(${target})
endfunction()
