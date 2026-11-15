# SPDX-License-Identifier: Apache-2.0
# One registration rule for every task: adding a file under these patterns edits
# no CMake. Plain GLOB, not CONFIGURE_DEPENDS: that adds an always-run check and
# `ninja -n` would never say "no work to do" (0009 d5). tools/build.ps1 lists the
# same patterns into out/build/sources.txt and re-runs configure when they change.
# Later tasks extend both: src/device/cuda (014),
# src/kernel_cpu and the ar-f64 marker (018), src/kernel_gpu and tests/gpu/kernels (027),
# tests/gpu (028).
file(GLOB ASTRORAY_TEST_CPU_SOURCES
  ${CMAKE_SOURCE_DIR}/tests/cpu/test_*.cpp
  ${CMAKE_SOURCE_DIR}/tests/common/*.cpp)
# The engine library holds host and CPU device code; tests link it, never the DLL.
file(GLOB_RECURSE _ar_host_sources ${CMAKE_SOURCE_DIR}/src/host/*.cpp)
file(GLOB _ar_device_cpu_sources ${CMAKE_SOURCE_DIR}/src/device/cpu/*.cpp)
set(ASTRORAY_ENGINE_SOURCES ${_ar_host_sources} ${_ar_device_cpu_sources})
if(ASTRORAY_GPU)
  file(GLOB _ar_device_cuda_sources ${CMAKE_SOURCE_DIR}/src/device/cuda/*.cpp)
  list(APPEND ASTRORAY_ENGINE_SOURCES ${_ar_device_cuda_sources})
endif()
# GPU test executable: same registration rule as the CPU one (003).
file(GLOB ASTRORAY_TEST_GPU_SOURCES
  ${CMAKE_SOURCE_DIR}/tests/gpu/test_*.cpp
  ${CMAKE_SOURCE_DIR}/tests/common/*.cpp)
file(GLOB ASTRORAY_CAPI_SOURCES ${CMAKE_SOURCE_DIR}/src/capi/*.cpp)
# Device modules: kernels.cmake turns each into one cubin by its file-name prefix.
file(GLOB ASTRORAY_GPU_KERNEL_SOURCES ${CMAKE_SOURCE_DIR}/src/kernel_gpu/*.cu)
file(GLOB ASTRORAY_GPU_TEST_SOURCES ${CMAKE_SOURCE_DIR}/tests/gpu/kernels/*.cu)

# A test file with `// ar-f64` in its first 5 lines is compiled a second time
# with AR_REAL_DOUBLE into the same executable (0002 D3); the copy is a generated
# wrapper that defines the macro and includes the file. tools/build.ps1 writes
# the matching `f64 <file>` lines into sources.txt.
set(_ar_f64_sources)
foreach(_f IN LISTS ASTRORAY_TEST_CPU_SOURCES)
  file(STRINGS ${_f} _head LIMIT_COUNT 5)
  string(FIND "${_head}" "// ar-f64" _pos)
  if(NOT _pos EQUAL -1)
    get_filename_component(_n ${_f} NAME_WE)
    set(_w ${CMAKE_BINARY_DIR}/f64/${_n}_f64.cpp)
    file(GENERATE OUTPUT ${_w}
         CONTENT "#define AR_REAL_DOUBLE\n#include \"${_f}\"\n")
    list(APPEND _ar_f64_sources ${_w})
  endif()
endforeach()
list(APPEND ASTRORAY_TEST_CPU_SOURCES ${_ar_f64_sources})

include(${CMAKE_CURRENT_LIST_DIR}/kernels.cmake)
