set(MAIR_LIT_CONFIG_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(MAIR_LIT_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/TestCases)
set(MAIR_OPT_PATH ${XSAN_OUTPUT_BINDIR}/mair-opt)
set(MAIR_XCLANG_PATH ${XSAN_OUTPUT_BINDIR}/xclang)

configure_lit_site_cfg(
  ${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.py.in
  ${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg.py)

# The tsan/mair-race-*.c tests run TSan-instrumented programs.
set(MAIR_TEST_DEPS mair-opt xclang)
if(TARGET tsan)
  list(APPEND MAIR_TEST_DEPS tsan)
endif()

add_lit_testsuite(check-mair
  "Running MAIR tests"
  ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS ${MAIR_TEST_DEPS}
)
add_dependencies(check-mair check-mair-unit)
# The runtime tests in TestCases/asan link and run ASan binaries.
if(TARGET asan)
  add_dependencies(check-mair asan)
endif()

if(NOT TARGET check-all)
  add_custom_target(check-all)
endif()
add_dependencies(check-all check-mair)
