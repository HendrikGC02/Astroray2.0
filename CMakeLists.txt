# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.31)

# Runs the version checks that need no compiler now, and the cl check right
# after the first project() (0009 d2).
set(CMAKE_PROJECT_TOP_LEVEL_INCLUDES ${CMAKE_CURRENT_LIST_DIR}/cmake/toolchain.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/toolchain.cmake)

project(astroray LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static CRT and embedded debug info for every target (0009 d2/d4): no CRT DLLs
# to ship, no PDB files to track.
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/out/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/out/bin)

# No effect until the device probe (014) and device code (027).
option(ASTRORAY_GPU "Build the CUDA device backend" ON)

include(cmake/warnings.cmake)
include(cmake/sources.cmake)

add_executable(astroray_tests_cpu tests/cpu/main.cpp ${ASTRORAY_TEST_CPU_SOURCES})
astroray_target_warnings(astroray_tests_cpu)
target_include_directories(astroray_tests_cpu SYSTEM PRIVATE third_party/doctest)
