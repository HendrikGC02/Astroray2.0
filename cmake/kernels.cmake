# SPDX-License-Identifier: Apache-2.0
# Kernel build rules. Ref: docs/decisions/0002-kernel-architecture.md D1, D3, D10,
# D12, D14. Part 1 is the CPU side, part 2 the GPU side (nvcc, one cubin per stage TU).

# Script mode (cmake -DAR_NVCC_LOG=<log> -P kernels.cmake -- <nvcc> <args>...): runs one nvcc
# call, saves its output (the ptxas -v lines) to the log and shows it only on failure.
if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
  set(_cmd)
  set(_seen_sep FALSE)
  math(EXPR _last "${CMAKE_ARGC} - 1")
  foreach(_i RANGE 0 ${_last})
    if(_seen_sep)
      list(APPEND _cmd "${CMAKE_ARGV${_i}}")
    elseif("${CMAKE_ARGV${_i}}" STREQUAL "--")
      set(_seen_sep TRUE)
    endif()
  endforeach()
  execute_process(COMMAND ${_cmd} RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
  file(WRITE "${AR_NVCC_LOG}" "${_out}")
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "nvcc failed (exit ${_rc}): ${_cmd}
${_out}")
  endif()
  return()
endif()

# ---- part 1: CPU ----

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
  # The CPU tests run the stages through CpuDevice, so they link the float library. Deferred
  # for the same reason as astroray_kernel above; the f64 library is linked by tests that need it.
  cmake_language(DEFER CALL target_link_libraries astroray_tests_cpu PRIVATE astroray_kernels_cpu)
endif()

# ---- part 2: GPU (0002 D14: nvcc only, module images, one cubin per stage TU) ----
# Every device build is an explicit custom command; CMake's CUDA language is enabled only so
# that CUDAToolkit_NVCC_EXECUTABLE is the compiler it selected for the pinned toolchain.
if(ASTRORAY_GPU)
  enable_language(CUDA)
  find_package(CUDAToolkit 12.8 REQUIRED)
  if(NOT CUDAToolkit_NVCC_EXECUTABLE)
    message(FATAL_ERROR "ASTRORAY_GPU is ON but CMake found no nvcc")
  endif()
  # PROVISIONAL (0009 d7): spike A ran -j8 cleanly; a measurement task records peak RAM.
  set_property(GLOBAL APPEND PROPERTY JOB_POOLS nvcc=8)

  # film_resolve accumulates the fp64 moments of 0008 d3, the one other FP64 kernel.
  set(AR_GPU_F64_MODULES advance_geodesic_f64 film_resolve film_present)
  # Gate hooks (src/kernel_gpu/test_*.cu, -DAR_GATE_HOOKS) are on exactly where the engine's own
  # AR_GATE_HOOKS is (CMakeLists.txt: every build type but Release), so a hook the host reads always
  # finds its module and a shipping build has neither. A plain variable, not a cache option: an old
  # cache entry cannot switch it off. Hook modules are never embedded (data.cmake).
  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(AR_GATE_HOOKS OFF)
  else()
    set(AR_GATE_HOOKS ON)
  endif()

  # Production line of 0002 D10 and D12: no fast math, no rdc, correctly rounded divide and
  # sqrt, denormals kept, warnings are errors, and any double-precision instruction in a
  # kernel is a ptxas warning (D1; `advance_geodesic_f64` opts out in a later task).
  # `ptxas --help` of CUDA 12.8 spells the double-use flag --warn-on-double-precision-use
  # (alias -warn-double-usage); ar_gpu_variant adds it to every module not listed in
  # AR_GPU_F64_MODULES, the one opt-out of D1.
  set(_ar_nvcc_base
    -ccbin ${CMAKE_CXX_COMPILER}
    -cubin -gencode arch=compute_120,code=sm_120
    -O3 -std=c++17
    -prec-div=true -prec-sqrt=true -ftz=false
    -Werror all-warnings
    -Xptxas -v
    -I ${CMAKE_SOURCE_DIR}/src/kernel)
  if(AR_GATE_HOOKS)
    list(APPEND _ar_nvcc_base -DAR_GATE_HOOKS)
  endif()

  set(_ar_gpu_all ${ASTRORAY_GPU_KERNEL_SOURCES})
  set(_ar_gpu_tests ${ASTRORAY_GPU_TEST_SOURCES})
  set(_ar_gpu_stages)
  set(_ar_gpu_probes)
  set(_ar_gpu_hooks)
  set(_ar_gpu_optix)
  foreach(_f IN LISTS _ar_gpu_all)
    get_filename_component(_n ${_f} NAME)
    if(_n MATCHES "^optix_")  # PTX modules for OptiX, built below
      list(APPEND _ar_gpu_optix ${_f})
    elseif(_n MATCHES "^probe_")
      list(APPEND _ar_gpu_probes ${_f})
    elseif(_n MATCHES "^test_")
      if(AR_GATE_HOOKS)
        list(APPEND _ar_gpu_hooks ${_f})
      endif()
    else()
      list(APPEND _ar_gpu_stages ${_f})
    endif()
  endforeach()
  set(_ar_gpu_prod ${_ar_gpu_stages} ${_ar_gpu_hooks} ${_ar_gpu_tests})
  set(_ar_gpu_seen)
  foreach(_f IN LISTS _ar_gpu_prod _ar_gpu_probes)
    get_filename_component(_n ${_f} NAME_WE)
    if(_n IN_LIST _ar_gpu_seen)
      message(FATAL_ERROR "two device sources are named ${_n}: module names are file names")
    endif()
    list(APPEND _ar_gpu_seen ${_n})
  endforeach()

  # One cubin per source: <dir>/<name>.cubin and <name>.ptxas.txt (the ptxas -v output).
  # DEPFILE carries the headers, so an edit to a kernel header rebuilds exactly its users.
  # Sets <target>_OUTPUTS to every file the variant produces.
  function(ar_gpu_variant target dir fmad)
    cmake_parse_arguments(V "" "COPY_TO" "DEFS;SOURCES" ${ARGN})
    set(_outs)
    foreach(_src IN LISTS V_SOURCES)
      get_filename_component(_n ${_src} NAME_WE)
      set(_cubin ${CMAKE_BINARY_DIR}/${dir}/${_n}.cubin)
      set(_log ${CMAKE_BINARY_DIR}/${dir}/${_n}.ptxas.txt)
      set(_f64 -Xptxas --warn-on-double-precision-use)
      if(_n IN_LIST AR_GPU_F64_MODULES)
        set(_f64)
      endif()
      add_custom_command(
        OUTPUT ${_cubin} ${_log}
        COMMAND ${CMAKE_COMMAND} -DAR_NVCC_LOG=${_log} -P ${CMAKE_CURRENT_FUNCTION_LIST_FILE} --
                ${CUDAToolkit_NVCC_EXECUTABLE} ${_ar_nvcc_base} ${_f64} --fmad=${fmad} ${V_DEFS}
                -MD -MF ${CMAKE_BINARY_DIR}/${dir}/${_n}.d -o ${_cubin} ${_src}
        DEPENDS ${_src}
        DEPFILE ${CMAKE_BINARY_DIR}/${dir}/${_n}.d
        JOB_POOL nvcc
        COMMENT "nvcc ${dir}/${_n}.cubin"
        VERBATIM)
      list(APPEND _outs ${_cubin} ${_log})
      if(V_COPY_TO)
        # Development builds load loose modules from here (0009 d5); a release writes none.
        set(_loose ${V_COPY_TO}/${_n}.cubin)
        add_custom_command(OUTPUT ${_loose}
          COMMAND ${CMAKE_COMMAND} -E make_directory ${V_COPY_TO}
          COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_cubin} ${_loose}
          DEPENDS ${_cubin} VERBATIM)
        list(APPEND _outs ${_loose})
      endif()
    endforeach()
    set(${target}_OUTPUTS ${_outs} PARENT_SCOPE)
  endfunction()

  set(_ar_loose)
  if(ASTRORAY_DEV_KERNEL_DIR)
    set(_ar_loose COPY_TO ${CMAKE_SOURCE_DIR}/out/bin/kernels)
  endif()
  # Production: stages are embedded in astroray_core (cmake/data.cmake); hook and test modules
  # are built, and copied loose in dev builds, but never embedded.
  ar_gpu_variant(astroray_kernels_gpu kernels true SOURCES ${_ar_gpu_prod} ${_ar_loose})
  add_custom_target(astroray_kernels_gpu ALL DEPENDS ${astroray_kernels_gpu_OUTPUTS})
  set(ASTRORAY_STAGE_CUBINS)
  foreach(_f IN LISTS _ar_gpu_stages)
    get_filename_component(_n ${_f} NAME_WE)
    list(APPEND ASTRORAY_STAGE_CUBINS ${CMAKE_BINARY_DIR}/kernels/${_n}.cubin)
  endforeach()

  # OptiX modules (0002 D11, D14): PTX, one per src/kernel_gpu/optix_*.cu, same flags as the cubins
  # except -ptx and no ptxas options. The driver compiles them, so there is no ptxas -v log;
  # `check full` reads their registers from Nsight Compute. The PTX is embedded as K_<NAME>
  # beside the stage cubins (data.cmake names a resource by the file-name stem). OptiX headers
  # are an include path here only, never target-wide (0001 D5.2).
  set(_ar_optix_outs)
  if(_ar_gpu_optix)
    include(${CMAKE_CURRENT_LIST_DIR}/deps.cmake)
    astroray_dep_dir(optix-headers ${ASTRORAY_OPTIX_VERSION} _ar_optix_dir)
    file(GLOB _ar_optix_inc ${_ar_optix_dir}/extracted/*/include)
    list(LENGTH _ar_optix_inc _ar_optix_inc_n)
    if(NOT _ar_optix_inc_n EQUAL 1)
      message(FATAL_ERROR "optix-headers ${ASTRORAY_OPTIX_VERSION}: expected one include directory, found ${_ar_optix_inc_n}")
    endif()
    foreach(_src IN LISTS _ar_gpu_optix)
      get_filename_component(_n ${_src} NAME_WE)
      set(_ptx ${CMAKE_BINARY_DIR}/optix/${_n}.ptx)
      add_custom_command(
        OUTPUT ${_ptx}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/optix
        COMMAND ${CMAKE_COMMAND} -DAR_NVCC_LOG=${CMAKE_BINARY_DIR}/optix/${_n}.nvcc.txt -P ${CMAKE_CURRENT_LIST_FILE} --
                ${CUDAToolkit_NVCC_EXECUTABLE} -ccbin ${CMAKE_CXX_COMPILER} -ptx
                -gencode arch=compute_120,code=compute_120 -O3 -std=c++17
                -prec-div=true -prec-sqrt=true -ftz=false --fmad=true -Werror all-warnings
                -I ${CMAKE_SOURCE_DIR}/src/kernel -isystem ${_ar_optix_inc}
                -MD -MF ${CMAKE_BINARY_DIR}/optix/${_n}.d -o ${_ptx} ${_src}
        DEPENDS ${_src}
        DEPFILE ${CMAKE_BINARY_DIR}/optix/${_n}.d
        JOB_POOL nvcc
        COMMENT "nvcc optix/${_n}.ptx"
        VERBATIM)
      list(APPEND _ar_optix_outs ${_ptx})
      list(APPEND ASTRORAY_STAGE_CUBINS ${_ptx})
      if(ASTRORAY_DEV_KERNEL_DIR)
        set(_loose ${CMAKE_SOURCE_DIR}/out/bin/kernels/${_n}.ptx)
        add_custom_command(OUTPUT ${_loose}
          COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/out/bin/kernels
          COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_ptx} ${_loose}
          DEPENDS ${_ptx} VERBATIM)
        list(APPEND _ar_optix_outs ${_loose})
      endif()
    endforeach()
  endif()

  add_custom_target(astroray_kernels_optix ALL DEPENDS ${_ar_optix_outs})

  # Agreement build (0002 D12): the same sources with FMA contraction off.
  ar_gpu_variant(astroray_kernels_gpu_nofma kernels_nofma false SOURCES ${_ar_gpu_prod})
  add_custom_target(astroray_kernels_gpu_nofma ALL DEPENDS ${astroray_kernels_gpu_nofma_OUTPUTS})

  # Probe build (0002 D10): no launch bounds, so the register count shows uncapped growth.
  # Built by `check full` only.
  ar_gpu_variant(astroray_kernels_gpu_probe kernels_probe true DEFS -DAR_LAUNCH_BOUNDS=0
    SOURCES ${_ar_gpu_stages} ${_ar_gpu_probes})
  # Gate-only probe modules (src/kernel_gpu/probe_*.cu) a second time with the production flags and
  # launch bounds, so kernelgate checks their `gated = "full"` rows on the capped cubin (0002 D10,
  # ruling 17). Same target as the probe build: never in the dev or gate build, never embedded.
  ar_gpu_variant(astroray_kernels_gpu_probe_capped kernels_probe_capped true SOURCES ${_ar_gpu_probes})
  add_custom_target(astroray_kernels_gpu_probe
    DEPENDS ${astroray_kernels_gpu_probe_OUTPUTS} ${astroray_kernels_gpu_probe_capped_OUTPUTS})
endif()
