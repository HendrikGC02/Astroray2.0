# SPDX-License-Identifier: Apache-2.0
# One registration rule for every task: adding a file under these patterns edits
# no CMake. Plain GLOB, not CONFIGURE_DEPENDS: that adds an always-run check and
# `ninja -n` would never say "no work to do" (0009 d5). tools/build.ps1 lists the
# same patterns into out/build/sources.txt and re-runs configure when they change.
# Later tasks extend both: src/capi (013), src/device/cuda (014),
# src/kernel_cpu and the ar-f64 marker (018), src/kernel_gpu, tests/gpu (027, 028).
file(GLOB ASTRORAY_TEST_CPU_SOURCES
  ${CMAKE_SOURCE_DIR}/tests/cpu/test_*.cpp
  ${CMAKE_SOURCE_DIR}/tests/common/*.cpp)
file(GLOB_RECURSE _ar_host_sources ${CMAKE_SOURCE_DIR}/src/host/*.cpp)
file(GLOB _ar_device_cpu_sources ${CMAKE_SOURCE_DIR}/src/device/cpu/*.cpp)
list(APPEND ASTRORAY_TEST_CPU_SOURCES ${_ar_host_sources} ${_ar_device_cpu_sources})
