cmake_minimum_required(VERSION 3.13.4)

# include_directories(dfsan_rt)

include_directories(include)

add_subdirectory(runtime)
add_subdirectory(compiler)
add_subdirectory(instrumentation)

foreach(_xsan_clang_driver_target compiler xclang)
  if(TARGET ${_xsan_clang_driver_target})
    add_dependencies(${_xsan_clang_driver_target}
      ASanInstPass
      TSanInstPass
      CovSanInstPass
      MSanInstPass
      XSanInstPass)
  endif()
endforeach()
