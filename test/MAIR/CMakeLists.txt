set(MAIR_LIT_CONFIG_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(MAIR_LIT_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/TestCases)
set(MAIR_OPT_PATH ${XSAN_OUTPUT_BINDIR}/mair-opt)

configure_lit_site_cfg(
  ${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.py.in
  ${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg.py)

add_lit_testsuite(check-mair
  "Running MAIR tests"
  ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS mair-opt
)

if(NOT TARGET check-all)
  add_custom_target(check-all)
endif()
add_dependencies(check-all check-mair)
