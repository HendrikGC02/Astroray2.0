# SPDX-License-Identifier: Apache-2.0
# Paths into the fetched-dependency cache for later find logic (0001 D9.2: no network at
# configure). Include it, then call astroray_dep_dir(<name> <version> <out-var>).
# Ref: 0009 d6; tools/fetch.ps1 deps fills the cache.
set(ASTRORAY_DEPS_CACHE ${CMAKE_SOURCE_DIR}/third_party/_cache CACHE PATH "fetched dependency cache")

function(astroray_dep_dir name version out)
  set(_dir ${ASTRORAY_DEPS_CACHE}/${name}-${version})
  if(NOT EXISTS ${_dir}/extracted.ok)
    message(FATAL_ERROR "dependency ${name} ${version} is missing from ${ASTRORAY_DEPS_CACHE}: run tools/fetch.ps1 deps")
  endif()
  set(${out} ${_dir} PARENT_SCOPE)
endfunction()

# Embree 4 (static, internal tasking, no TBB). Its headers are included only by
# src/device/cpu (0001 D5.2), so they stay private to the engine. The archives are listed
# explicitly because a plain imported path carries no inter-archive dependencies.
# Ref: 0001 D9.1; third_party/deps.toml; the archive's lib/cmake/embree-4.4.1/embree-targets.cmake.
if(TARGET astroray_engine)
  astroray_dep_dir(embree 4.4.1 _ar_embree_dir)
  set(_ar_embree_root ${_ar_embree_dir}/extracted)
  target_include_directories(astroray_engine SYSTEM PRIVATE ${_ar_embree_root}/include)
  target_compile_definitions(astroray_engine PRIVATE EMBREE_STATIC_LIB)
  foreach(_lib embree4 embree_sse42 embree_avx embree_avx2 embree_avx512 sys math simd lexers tasking)
    target_link_libraries(astroray_engine PRIVATE ${_ar_embree_root}/lib/${_lib}.lib)
  endforeach()
endif()
