// SPDX-License-Identifier: Apache-2.0
// Finds the bytes of a GPU kernel module: the cubin (or, for OptiX modules, PTX) embedded in the DLL as RCDATA K_<NAME>,
// or, in a build with AR_DEV_KERNEL_DIR and ASTRORAY_KERNEL_DIR set, the loose file in
// that directory. No CUDA types, so tests include it freely; cuModuleLoadData is the
// device's job.
// Ref: docs/decisions/0002-kernel-architecture.md D14 (module images, dev directory),
// D15 (loader failure rows); 0009 decision 5 (loose kernels in dev builds only);
// 0001 D8.4 (RCDATA).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace astroray::host {

struct ModuleImage {
  std::vector<uint8_t> bytes;
  std::string origin;  // "embedded K_<NAME>" or the file path, for messages
};

// `name` is the kernel source's file-name stem (kernels.cmake: module names are file
// names), lower-case letters, digits and underscores. Throws std::invalid_argument for a
// malformed name and DeviceError when the image is missing or unreadable. A dev build
// with ASTRORAY_KERNEL_DIR set reads only from that directory: a missing file is an
// error, never a fall back to the embedded copy.
// `corrupt` (ASTRORAY_TEST_CORRUPT_CUBIN, read once by the device that asks) hands over a damaged
// copy of a cubin; OptiX PTX is never damaged. The caller decides, so no environment is read here.
ModuleImage load_module_image(const std::string& name, bool corrupt = false);

// The directory loose modules come from: ASTRORAY_KERNEL_DIR in a dev build, empty otherwise. A device reads
// it once at creation and passes it to every later call, so a later change of the variable moves nothing.
std::string dev_kernel_dir();
// As above, reading `dir` (empty: the embedded copy) instead of the environment.
ModuleImage load_module_image(const std::string& name, bool corrupt, const std::string& dir);
// Size and last write time of the loose file of module `name` in `dir`; false when there is none.
struct ModuleStamp {
  uint64_t size = 0;
  int64_t mtime = 0;
  bool operator==(const ModuleStamp& o) const { return size == o.size && mtime == o.mtime; }
  bool operator!=(const ModuleStamp& o) const { return !(*this == o); }
};
// `error`, when given, stays empty for a file that is genuinely absent and otherwise holds the filesystem's
// message (a rebuild holding the file, a permission error).
bool module_file_stamp(const std::string& name, const std::string& dir, ModuleStamp* out, std::string* error = nullptr);

// Test modules only (never embedded): reads the file at `path`. Throws DeviceError when
// it cannot be read.
ModuleImage load_module_image_file(const std::string& path);

// cuModuleLoadData takes no length: the driver parses whatever ELF headers it finds and reads where
// they point, so a truncated cubin makes it read past the buffer (it has hung there). Every image
// handed to it is a cubin (kernels.cmake builds -cubin; OptiX PTX goes to optixModuleCreate with its
// size), so before every call the image's layout is checked: an ELF-64 little-endian header, the
// program and section header tables, every section and segment with file contents, and the section
// name table all inside the bytes. Bounds and layout only, never contents. Returns the first defect,
// or an empty string for a well-formed layout.
// Ref: System V ABI, Generic ABI, chapter 4 "Object Files" (ELF Header, Sections, Program Header),
// ELF-64 structure layouts.
std::string cubin_layout_defect(const uint8_t* data, size_t size);
// The DeviceError text for a refused image: "module image truncated or invalid: <name> (<defect>)".
std::string cubin_layout_message(const std::string& name, const std::string& defect);

}  // namespace astroray::host
