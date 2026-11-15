# SPDX-License-Identifier: Apache-2.0
# Toolchain pins, 0009 d2. Included twice: first from CMakeLists.txt before
# project() (tool versions), then by CMake right after project() (compiler).
if(NOT CMAKE_CXX_COMPILER_VERSION)
  if(CMAKE_VERSION VERSION_LESS 3.31)
    message(FATAL_ERROR "CMake >= 3.31 required, found ${CMAKE_VERSION}")
  endif()
  execute_process(COMMAND ninja --version OUTPUT_VARIABLE _ar_ninja_ver
                  OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _ar_ninja_rc)
  if(NOT _ar_ninja_rc EQUAL 0 OR NOT _ar_ninja_ver MATCHES "^1\\.13\\.")
    message(FATAL_ERROR "Ninja 1.13.x required, found '${_ar_ninja_ver}'")
  endif()
elseif("$ENV{CI}" STREQUAL "true")
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19.44)
    message(FATAL_ERROR "cl >= 19.44 required on CI, found ${CMAKE_CXX_COMPILER_VERSION}")
  endif()
  message(STATUS "cl ${CMAKE_CXX_COMPILER_VERSION}")
elseif(NOT CMAKE_CXX_COMPILER_VERSION MATCHES "^19\\.44\\.")
  message(FATAL_ERROR "cl 19.44.* required, found ${CMAKE_CXX_COMPILER_VERSION}")
endif()
