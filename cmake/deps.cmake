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
