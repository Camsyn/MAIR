# Include the part of LLVM's CMake libraries that defines
# `add_llvm_pass_plugin`.
include(AddLLVM)

# Use LLVM's preprocessor definitions, include directories, and library search
# paths.
add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
link_directories(${LLVM_LIBRARY_DIRS})

# -Wl,-Bsymbolic : force local symbols to be used in shared library
add_link_options(-Wl,-Bsymbolic)

if(NOT DEFINED XSAN_INSTRUMENTATION_VARIANT)
  message(FATAL_ERROR "XSAN_INSTRUMENTATION_VARIANT is not set")
endif()

set(XSAN_PASS_COMMON
  AttributeTaggingPass.cpp
  Instrumentation.cpp
  PassRegistry.cpp
  UbsanInstTagging.cpp
  Analysis/ActiveMopAnalysis.cpp
  Analysis/MopRecurrenceReducer.cpp
  Analysis/TsanMopAnalysis.cpp
  Utils/ValueUtils.cpp
  Utils/Logging.cpp
  Utils/MetaDataUtils.cpp
  Utils/Options.cpp
  Utils/UbsanUtils.cpp
)

set(XSAN_PASS_VERSIONED_DIR ${XSAN_INSTRUMENTATION_VARIANT})
set(XSAN_PASS_VERSIONED
  ${XSAN_PASS_VERSIONED_DIR}/AddressSanitizer.cpp
  ${XSAN_PASS_VERSIONED_DIR}/MemorySanitizer.cpp
  ${XSAN_PASS_VERSIONED_DIR}/SanitizerCoverage.cpp
  ${XSAN_PASS_VERSIONED_DIR}/ThreadSanitizer.cpp
)

message(STATUS "Using instrumentation sources from ${XSAN_PASS_VERSIONED_DIR}")

# TODO: all pass into 1 so
add_llvm_pass_plugin(ASanInstPass
  PARTIAL_SOURCES_INTENDED # Skip checking for else files
  ${XSAN_PASS_VERSIONED_DIR}/AddressSanitizer.cpp
  ${XSAN_PASS_COMMON}
)

add_llvm_pass_plugin(TSanInstPass
  PARTIAL_SOURCES_INTENDED # Skip checking for else files
  ${XSAN_PASS_VERSIONED_DIR}/ThreadSanitizer.cpp
  ${XSAN_PASS_COMMON}
)

add_llvm_pass_plugin(CovSanInstPass
  PARTIAL_SOURCES_INTENDED # Skip checking for else files
  ${XSAN_PASS_VERSIONED_DIR}/SanitizerCoverage.cpp
)

add_llvm_pass_plugin(MSanInstPass
  PARTIAL_SOURCES_INTENDED # Skip checking for else files
  ${XSAN_PASS_VERSIONED_DIR}/MemorySanitizer.cpp
  ${XSAN_PASS_COMMON}
)

add_llvm_pass_plugin(XSanInstPass
  PARTIAL_SOURCES_INTENDED # Skip checking for else files
  ${XSAN_PASS_VERSIONED_DIR}/ThreadSanitizer.cpp
  ${XSAN_PASS_VERSIONED_DIR}/AddressSanitizer.cpp
  ${XSAN_PASS_VERSIONED_DIR}/MemorySanitizer.cpp
  XSanDeSanPass.cpp
  XSanDeSanRuntime.cpp
  XSanDeSanDeSan.cpp
  XSanDeSanReSan.cpp
  XSanDeSanState.cpp
  XSanitizerCompositor.cpp
  ${XSAN_PASS_COMMON}
)

add_llvm_pass_plugin(TestPass
  PARTIAL_SOURCES_INTENDED # Skip checking for else files
  TestPass.cpp
  ${XSAN_PASS_COMMON}
)

foreach(_xsan_pass_target
    ASanInstPass
    TSanInstPass
    CovSanInstPass
    MSanInstPass
    XSanInstPass
    TestPass)
  target_include_directories(${_xsan_pass_target} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${XSAN_PASS_VERSIONED_DIR}
  )
  target_compile_definitions(${_xsan_pass_target} PRIVATE
    XSAN_LLVM_MAJOR=${LLVM_VERSION_MAJOR}
  )
endforeach()

target_compile_definitions(XSanInstPass PRIVATE XSAN_PASS)

set_target_properties(ASanInstPass
  TSanInstPass
  CovSanInstPass
  MSanInstPass
  XSanInstPass
  TestPass
  PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${XSAN_OUTPUT_PASSDIR}"
)

install(TARGETS ASanInstPass TSanInstPass CovSanInstPass MSanInstPass XSanInstPass
  LIBRARY
  DESTINATION ${XSAN_INSTALL_PASSDIR}
  COMPONENT pass
)
