# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.31)

# Runs the version checks that need no compiler now, and the cl check right
# after the first project() (0009 d2).
set(CMAKE_PROJECT_TOP_LEVEL_INCLUDES ${CMAKE_CURRENT_LIST_DIR}/cmake/toolchain.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/toolchain.cmake)

# C is for the generated layout table, which MSVC's C front end sizes (0001 D2.7).
project(astroray VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static CRT and embedded debug info for every target (0009 d2/d4): no CRT DLLs
# to ship, no PDB files to track.
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/out/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/out/bin)

# GPU builds link static cudart for the device probe (host compiler only; device
# code starts in 027). The cpu preset never looks for CUDA.
option(ASTRORAY_GPU "Build the CUDA device backend" ON)

include(cmake/warnings.cmake)
include(cmake/sources.cmake)

# Loose kernel modules may be loaded only in development builds (0009 d5): off by
# default, on only in the dev preset.
option(ASTRORAY_DEV_KERNEL_DIR "Allow ASTRORAY_KERNEL_DIR kernel reload (dev preset only)" OFF)

# The header grammar check is a build step: a violation fails the build (0001 D2.10).
# The stamp output keeps `ninja -n` clean once the check has passed.
find_program(ASTRORAY_PYTHON NAMES python python3 REQUIRED)
set(ASTRORAY_ABI_STAMP ${CMAKE_BINARY_DIR}/abigen.stamp)
add_custom_command(
  OUTPUT ${ASTRORAY_ABI_STAMP}
  COMMAND ${ASTRORAY_PYTHON} -I ${CMAKE_SOURCE_DIR}/tools/abigen.py --check ${CMAKE_SOURCE_DIR}/include/astroray/astroray.h
  COMMAND ${CMAKE_COMMAND} -E touch ${ASTRORAY_ABI_STAMP}
  DEPENDS ${CMAKE_SOURCE_DIR}/include/astroray/astroray.h ${CMAKE_SOURCE_DIR}/tools/abigen.py
  COMMENT "abigen --check astroray.h"
  VERBATIM)
add_custom_target(astroray_abigen ALL DEPENDS ${ASTRORAY_ABI_STAMP})

# Generated ABI files: layout table, hash define and _abi.py (0001 D1.3, D2.7).
set(ASTRORAY_GEN_DIR ${CMAKE_BINARY_DIR}/gen)
add_custom_command(
  OUTPUT ${ASTRORAY_GEN_DIR}/astroray_layout.c ${ASTRORAY_GEN_DIR}/abi_hash.h ${ASTRORAY_GEN_DIR}/_abi.py
  COMMAND ${ASTRORAY_PYTHON} -I ${CMAKE_SOURCE_DIR}/tools/abigen.py --emit ${CMAKE_SOURCE_DIR}/include/astroray/astroray.h ${ASTRORAY_GEN_DIR}
  DEPENDS ${CMAKE_SOURCE_DIR}/include/astroray/astroray.h ${CMAKE_SOURCE_DIR}/tools/abigen.py
  COMMENT "abigen --emit astroray.h"
  VERBATIM)

# Opcode table outputs: _ops.py and coverage.csv; a volume-reachable op without
# an interval rule fails the build (0005 item 30, 0006 D7).
set(ASTRORAY_OPSGEN_STAMP ${CMAKE_BINARY_DIR}/opsgen.stamp)
add_custom_command(
  OUTPUT ${ASTRORAY_OPSGEN_STAMP}
  COMMAND ${ASTRORAY_PYTHON} -I ${CMAKE_SOURCE_DIR}/tools/opsgen.py --root ${CMAKE_SOURCE_DIR} --out ${ASTRORAY_GEN_DIR}
  COMMAND ${CMAKE_COMMAND} -E touch ${ASTRORAY_OPSGEN_STAMP}
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/opsgen.py ${CMAKE_SOURCE_DIR}/src/kernel/vm/ops.def
          ${CMAKE_SOURCE_DIR}/blender/astroray/materials/lowering_table.py
          ${CMAKE_SOURCE_DIR}/tests/data/shader_node_types_5_3.txt
  COMMENT "opsgen"
  VERBATIM)
add_custom_target(astroray_opsgen ALL DEPENDS ${ASTRORAY_OPSGEN_STAMP})

# Build identity. The command depends on HEAD, the current branch's ref and the
# index, so a commit relinks; the generator rewrites the source only when its
# content changed, which keeps `ninja -n` clean afterwards (0009 d5).
find_package(Git REQUIRED)
function(_ar_git_path out what)
  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --git-path ${what}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE p OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "git rev-parse --git-path ${what} failed")
  endif()
  cmake_path(ABSOLUTE_PATH p BASE_DIRECTORY ${CMAKE_SOURCE_DIR} NORMALIZE)
  set(${out} ${p} PARENT_SCOPE)
endfunction()
_ar_git_path(_ar_git_head HEAD)
_ar_git_path(_ar_git_index index)
set(_ar_git_deps ${_ar_git_head} ${_ar_git_index})
execute_process(COMMAND ${GIT_EXECUTABLE} symbolic-ref -q HEAD
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE _ar_branch_ref OUTPUT_STRIP_TRAILING_WHITESPACE)
if(_ar_branch_ref)
  _ar_git_path(_ar_ref_file ${_ar_branch_ref})
  if(EXISTS ${_ar_ref_file})
    list(APPEND _ar_git_deps ${_ar_ref_file})
  else()
    _ar_git_path(_ar_packed packed-refs)
    if(EXISTS ${_ar_packed})
      list(APPEND _ar_git_deps ${_ar_packed})
    endif()
  endif()
endif()
# A branch switch changes HEAD: reconfigure so the new branch's ref is tracked.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_ar_git_head})
set(ASTRORAY_BUILDINFO_CPP ${ASTRORAY_GEN_DIR}/build_info.cpp)
add_custom_command(
  OUTPUT ${ASTRORAY_BUILDINFO_CPP}
  COMMAND ${CMAKE_COMMAND} -DOUT=${ASTRORAY_BUILDINFO_CPP} -DGIT=${GIT_EXECUTABLE} -DSRC=${CMAKE_SOURCE_DIR}
          -DVERSION=${PROJECT_VERSION} -DBUILD_TYPE=${CMAKE_BUILD_TYPE} -P ${CMAKE_SOURCE_DIR}/cmake/buildinfo.cmake
  DEPENDS ${_ar_git_deps} ${CMAKE_SOURCE_DIR}/cmake/buildinfo.cmake
  COMMENT "build_info.cpp"
  VERBATIM)

add_library(astroray_buildinfo STATIC ${ASTRORAY_BUILDINFO_CPP})
astroray_target_warnings(astroray_buildinfo)
target_include_directories(astroray_buildinfo PUBLIC ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
if(ASTRORAY_DEV_KERNEL_DIR)
  target_compile_definitions(astroray_buildinfo PUBLIC AR_DEV_KERNEL_DIR)
endif()

# Engine: created when the first host or CPU device source exists (0009 d3).
set(_ar_engine_libs astroray_buildinfo)
if(ASTRORAY_ENGINE_SOURCES)
  add_library(astroray_engine STATIC ${ASTRORAY_ENGINE_SOURCES})
  astroray_target_warnings(astroray_engine)
  target_link_libraries(astroray_engine PUBLIC astroray_buildinfo)
  if(ASTRORAY_GPU)
    find_package(CUDAToolkit 12.8 REQUIRED)
    target_compile_definitions(astroray_engine PUBLIC ASTRORAY_GPU)
    target_link_libraries(astroray_engine PRIVATE CUDA::cudart_static)
  endif()
  list(APPEND _ar_engine_libs astroray_engine)
endif()

# The one DLL Python loads: capi + engine + the generated layout table.
add_library(astroray_core SHARED ${ASTRORAY_CAPI_SOURCES} ${ASTRORAY_GEN_DIR}/astroray_layout.c ${ASTRORAY_GEN_DIR}/abi_hash.h)
astroray_target_warnings(astroray_core)
# _Alignof needs C11 mode.
set_source_files_properties(${ASTRORAY_GEN_DIR}/astroray_layout.c PROPERTIES COMPILE_OPTIONS "/std:c11")
target_compile_definitions(astroray_core PRIVATE AR_BUILDING_CORE)
target_include_directories(astroray_core PRIVATE ${ASTRORAY_GEN_DIR})
target_link_libraries(astroray_core PRIVATE ${_ar_engine_libs})

# Family sizes for the statistical harness, read from tools/budgets.toml [family*]
# (0009 d15). file(GENERATE) rewrites the header only when the content changed.
file(READ ${CMAKE_SOURCE_DIR}/tools/budgets.toml _ar_budgets_toml)
foreach(_k GATE:gate_statistical FULL:full_statistical ANALYTIC:analytic_scenes)
  string(REPLACE ":" ";" _kv ${_k})
  list(GET _kv 0 _name)
  list(GET _kv 1 _key)
  if(NOT _ar_budgets_toml MATCHES "(^|\n)${_key} *= *([0-9]+)")
    message(FATAL_ERROR "tools/budgets.toml has no '${_key} = <n>' family size")
  endif()
  string(APPEND _ar_families_h "#define AR_FAMILY_${_name} ${CMAKE_MATCH_2}\n")
endforeach()
file(GENERATE OUTPUT ${ASTRORAY_GEN_DIR}/families.h CONTENT "// generated from tools/budgets.toml\n#pragma once\n${_ar_families_h}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tools/budgets.toml)

add_executable(astroray_tests_cpu tests/cpu/main.cpp ${ASTRORAY_TEST_CPU_SOURCES})
target_include_directories(astroray_tests_cpu PRIVATE ${CMAKE_SOURCE_DIR}/tests ${ASTRORAY_GEN_DIR})
astroray_target_warnings(astroray_tests_cpu)
target_include_directories(astroray_tests_cpu SYSTEM PRIVATE third_party/doctest)
target_link_libraries(astroray_tests_cpu PRIVATE ${_ar_engine_libs})
add_dependencies(astroray_tests_cpu astroray_opsgen)

# Installable Blender extension zip (0007 d45, 0001 D10.2): stage the addon, the
# generated bindings and the DLL, then let Blender build the zip. Not in ALL: it
# needs the Blender install (ASTRORAY_BLENDER). The zip name is Blender's own.
set(ASTRORAY_EXT_STAGE ${CMAKE_SOURCE_DIR}/out/extension/astroray)
set(ASTRORAY_EXT_ZIP ${CMAKE_SOURCE_DIR}/out/extension/astroray-${PROJECT_VERSION}-windows_x64.zip)
file(GLOB_RECURSE _ar_ext_src ${CMAKE_SOURCE_DIR}/blender/astroray/*)
add_custom_command(
  OUTPUT ${ASTRORAY_EXT_ZIP}
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${ASTRORAY_EXT_STAGE}
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/blender/astroray ${ASTRORAY_EXT_STAGE}
  COMMAND ${CMAKE_COMMAND} -E copy ${ASTRORAY_GEN_DIR}/_abi.py ${ASTRORAY_GEN_DIR}/_ops.py ${ASTRORAY_EXT_STAGE}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${ASTRORAY_EXT_STAGE}/bin
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:astroray_core> ${ASTRORAY_EXT_STAGE}/bin/astroray_core.dll
  COMMAND pwsh -NoProfile -Command ". '${CMAKE_SOURCE_DIR}/tools/lib/blender.ps1'; exit (Invoke-ArBlenderCommand -Args 'extension','build','--split-platforms','--source-dir','${ASTRORAY_EXT_STAGE}','--output-dir','${CMAKE_SOURCE_DIR}/out/extension')"
  DEPENDS astroray_core astroray_opsgen ${_ar_ext_src} ${ASTRORAY_GEN_DIR}/_abi.py
  COMMENT "astroray_extension: blender extension build"
  VERBATIM)
add_custom_target(astroray_extension DEPENDS ${ASTRORAY_EXT_ZIP})