cmake_minimum_required(VERSION 3.22)
project(MAIR_SQLITE3_E2E C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT SQLITE3_SOURCE_DIR OR NOT SQLITE3_DRIVER)
  message(FATAL_ERROR "SQLITE3_SOURCE_DIR and SQLITE3_DRIVER are required")
endif()

find_package(Threads REQUIRED)
add_library(sqlite3_static STATIC ${SQLITE3_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3_static PUBLIC ${SQLITE3_SOURCE_DIR})
# Match evaluation/sqlite3's engine settings; build the amalgamation once.
target_compile_definitions(sqlite3_static PRIVATE
  SQLITE_THREADSAFE=1
  SQLITE_DEBUG=1
  SQLITE_MAX_LENGTH=128000000
  SQLITE_MAX_SQL_LENGTH=128000000
  SQLITE_MAX_MEMORY=25000000
  SQLITE_PRINTF_PRECISION_LIMIT=1048576
  SQLITE_MAX_PAGE_COUNT=16384)
target_link_libraries(sqlite3_static PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)

add_executable(sqlite3_fuzzer ${SQLITE3_DRIVER})
set_target_properties(sqlite3_fuzzer PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(sqlite3_fuzzer PRIVATE sqlite3_static ${FUZZER_RUNTIME})
add_executable(sqlite3_oracle ${CMAKE_CURRENT_LIST_DIR}/oracle.c)
target_link_libraries(sqlite3_oracle PRIVATE sqlite3_static)
