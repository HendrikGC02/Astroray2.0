# SPDX-License-Identifier: Apache-2.0
# Embeds every file listed in data/MANIFEST.toml into the binary as Win32 RCDATA, named by its
# path under data/ (0001 D8.4, 0003 d18). Link $<TARGET_OBJECTS:astroray_data> into a target to
# carry the data; src/host/data_resource.cpp finds it. The manifest is read with a regex here
# (CMake has no TOML parser): one `path = "..."` line per [[file]] entry.
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
set(_ar_data_rc ${CMAKE_BINARY_DIR}/gen/astroray_data.rc)
file(GENERATE OUTPUT ${_ar_data_rc} CONTENT "${_ar_rc_content}")
add_library(astroray_data OBJECT ${_ar_data_rc})
set_source_files_properties(${_ar_data_rc} PROPERTIES OBJECT_DEPENDS "${_ar_data_files}")
