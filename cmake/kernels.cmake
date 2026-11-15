# SPDX-License-Identifier: Apache-2.0
# CPU side of the kernel build. Ref: docs/decisions/0002-kernel-architecture.md
# D1, D3, D12. The GPU rules arrive with task 027.

add_library(astroray_kernel INTERFACE)
target_include_directories(astroray_kernel INTERFACE ${CMAKE_SOURCE_DIR}/src/kernel)

# Production CPU flags (D12): no exceptions, no RTTI, precise FP, AVX2, and no
# /fp:contract so an FMA appears only where the code writes ar::fma. Test
# executables do not use this: doctest needs exceptions.
function(ar_kernel_cpu_flags target)
  astroray_target_warnings(${target})
  # CMake's default /EHsc is overridden on purpose; cl prints D9025 for it (a
  # command-line note, not a /WX error).
  target_compile_options(${target} PRIVATE /EHs-c- /GR- /fp:precise /arch:AVX2 /O2)
  target_link_libraries(${target} PUBLIC astroray_kernel)
endfunction()

# Test targets reach the kernel headers without CMakeLists.txt naming them: the
# executable is created after this file is included, so link at end of scope.
cmake_language(DEFER CALL target_link_libraries astroray_tests_cpu PRIVATE astroray_kernel)

# One .cpp per stage (task 043 adds the first). The f64 library is the unshipped
# test reference of D3.
file(GLOB _ar_kernel_cpu_sources ${CMAKE_SOURCE_DIR}/src/kernel_cpu/*.cpp)
if(_ar_kernel_cpu_sources)
  add_library(astroray_kernels_cpu STATIC ${_ar_kernel_cpu_sources})
  ar_kernel_cpu_flags(astroray_kernels_cpu)
  add_library(astroray_kernels_cpu_f64 STATIC ${_ar_kernel_cpu_sources})
  ar_kernel_cpu_flags(astroray_kernels_cpu_f64)
  target_compile_definitions(astroray_kernels_cpu_f64 PUBLIC AR_REAL_DOUBLE)
endif()
