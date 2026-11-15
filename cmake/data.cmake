# SPDX-License-Identifier: Apache-2.0
# Embeds every file listed in data/MANIFEST.toml into the binary as Win32 RCDATA, named by its
# path under data/ (0001 D8.4, 0003 d18). Link $<TARGET_OBJECTS:astroray_data> into a target to
# carry the data; src/host/data_resource.cpp finds it. The manifest is read with a regex here
# (CMake has no TOML parser): one `path = "..."` line per [[file]] entry.
# The release kernel modules (stage cubins, named K_<NAME>) are embedded the same way, into
# astroray_core only, so the CPU tests never depend on device code (0002 D14, ruling 13).
enable_language(RC)
set(_ar_manifest ${CMAKE_SOURCE_DIR}/data/MANIFEST.toml)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_ar_manifest})
file(STRINGS ${_ar_manifest} _ar_path_lines REGEX "^path = \"[^\"]+\"")
set(_ar_rc_content "")
set(_ar_data_files "")
foreach(_line IN LISTS _ar_path_lines)
  string(REGEX REPLACE "^path = \"([^\"]+)\".*" "\\1" _rel "${_line}")
  if(NOT EXISTS ${CMAKE_SOURCE_DIR}/data/${_rel})
    message(FATAL_ERROR "data/MANIFEST.toml lists ${_rel}, which does not exist")
  endif()
  # rc keeps the quotes of a quoted name as part of it, so the name is written bare.
  string(APPEND _ar_rc_content "${_rel} RCDATA \"${CMAKE_SOURCE_DIR}/data/${_rel}\"\n")
  list(APPEND _ar_data_files ${CMAKE_SOURCE_DIR}/data/${_rel})
endforeach()
if(NOT _ar_data_files)
  message(FATAL_ERROR "data/MANIFEST.toml lists no files")
endif()

# One generated .rc becomes one OBJECT library whose rebuild follows its payload files.
function(ar_embed_rcdata target rc content)
  file(GENERATE OUTPUT ${rc} CONTENT "${content}")
  add_library(${target} OBJECT ${rc})
  set_source_files_properties(${rc} PROPERTIES OBJECT_DEPENDS "${ARGN}")
endfunction()

# Data only: linked by the tests, which read the tables but never load kernels.
ar_embed_rcdata(astroray_data ${CMAKE_BINARY_DIR}/gen/astroray_data.rc "${_ar_rc_content}" ${_ar_data_files})

# Data plus every stage cubin: the one resource set of astroray_core. Hook and test modules
# are never listed (kernels.cmake keeps them out of ASTRORAY_STAGE_CUBINS).
set(_ar_core_rc_content "${_ar_rc_content}")
foreach(_cubin IN LISTS ASTRORAY_STAGE_CUBINS)
  get_filename_component(_n ${_cubin} NAME_WE)
  string(TOUPPER ${_n} _upper)
  string(APPEND _ar_core_rc_content "K_${_upper} RCDATA \"${_cubin}\"\n")
endforeach()
ar_embed_rcdata(astroray_core_resources ${CMAKE_BINARY_DIR}/gen/astroray_core.rc "${_ar_core_rc_content}"
  ${_ar_data_files} ${ASTRORAY_STAGE_CUBINS})
if(ASTRORAY_STAGE_CUBINS)
  add_dependencies(astroray_core_resources astroray_kernels_gpu)
endif()
