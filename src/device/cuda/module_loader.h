// SPDX-License-Identifier: Apache-2.0
// Finds the bytes of a GPU kernel module: the cubin (or, for OptiX modules, PTX) embedded in the DLL as RCDATA K_<NAME>,
// or, in a build with AR_DEV_KERNEL_DIR and ASTRORAY_KERNEL_DIR set, the loose file in
// that directory. No CUDA types, so tests include it freely; cuModuleLoadData is the
// device's job.
// Ref: docs/decisions/0002-kernel-architecture.md D14 (module images, dev directory),
// D15 (loader failure rows); 0009 decision 5 (loose kernels in dev builds only);
// 0001 D8.4 (RCDATA).
#pragma once
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

// Test modules only (never embedded): reads the file at `path`. Throws DeviceError when
// it cannot be read.
ModuleImage load_module_image_file(const std::string& path);

}  // namespace astroray::host
