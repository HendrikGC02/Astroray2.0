// SPDX-License-Identifier: Apache-2.0
#include "device/cuda/module_loader.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include "host/build_info.h"
#include "host/data_resource.h"
#include "host/device.h"

namespace astroray::host {
namespace {

// Opened with every share flag, delete included: a dev rebuild replaces or renames the module while the
// engine may be reading it, and a handle without FILE_SHARE_DELETE (what std::ifstream opens) would make
// that replacement fail with a sharing violation.
ModuleImage read_file(const std::filesystem::path& path) {
  const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) throw DeviceError("cannot read kernel module file " + path.string());
  ModuleImage image;
  LARGE_INTEGER size{};
  bool ok = GetFileSizeEx(h, &size) != 0;
  if (ok) {
    image.bytes.resize(static_cast<size_t>(size.QuadPart));
    size_t done = 0;
    while (ok && done < image.bytes.size()) {
      DWORD got = 0;
      const DWORD want = static_cast<DWORD>(std::min<size_t>(image.bytes.size() - done, 1u << 24));
      ok = ReadFile(h, image.bytes.data() + done, want, &got, nullptr) != 0 && got != 0;
      done += got;
    }
    ok = ok && done == image.bytes.size();
  }
  CloseHandle(h);
  if (!ok || image.bytes.empty())
    throw DeviceError("kernel module file " + path.string() + " is empty or unreadable");
  image.origin = path.string();
  return image;
}

// ASTRORAY_TEST_CORRUPT_CUBIN: the copy handed to the driver loses its header, as a damaged embedded
// image would (0002 D15 injection). The embedded bytes themselves are never touched. OptiX modules are
// PTX the driver compiles, not cubins, and keep their text.
void damage(ModuleImage& image) {
  const size_t n = std::min<size_t>(image.bytes.size(), 64);
  std::fill(image.bytes.begin(), image.bytes.begin() + static_cast<std::ptrdiff_t>(n), uint8_t{0xA5});
  image.origin += " (damaged by ASTRORAY_TEST_CORRUPT_CUBIN)";
}

ModuleImage load_undamaged(const std::string& name, const std::string& dir);

void check_name(const std::string& name) {
  bool ok = !name.empty();
  for (char c : name) {
    const unsigned char u = static_cast<unsigned char>(c);
    ok = ok && (std::islower(u) || std::isdigit(u) || c == '_');
  }
  if (!ok)
    throw std::invalid_argument("kernel module name '" + name + "' is not lower-case letters, digits and underscores");
}

// A cubin for CUDA stages, PTX for OptiX modules.
std::filesystem::path loose_path(const std::string& name, const std::string& dir) {
  const std::filesystem::path d(dir);
  // The error_code overload: a rebuild may hold the file, and that goes through the warning path.
  std::error_code ec;
  return std::filesystem::exists(d / (name + ".cubin"), ec) ? d / (name + ".cubin") : d / (name + ".ptx");
}

}  // namespace

std::string dev_kernel_dir() {
  const std::string source = kernel_source();
  return source == "embedded" ? std::string() : source;
}

ModuleImage load_module_image(const std::string& name, bool corrupt, const std::string& dir) {
  ModuleImage image = load_undamaged(name, dir);
  if (corrupt && name.rfind("optix_", 0) != 0) damage(image);
  return image;
}

ModuleImage load_module_image(const std::string& name, bool corrupt) {
  return load_module_image(name, corrupt, dev_kernel_dir());
}

bool module_file_stamp(const std::string& name, const std::string& dir, ModuleStamp* out, std::string* error) {
  check_name(name);
  if (error) error->clear();
  const std::filesystem::path d(dir);
  // A cubin for CUDA stages, PTX for OptiX modules; error_code overloads, so a file a rebuild holds goes
  // through the warning path with the filesystem's own message.
  for (const char* ext : {".cubin", ".ptx"}) {
    const std::filesystem::path p = d / (name + ext);
    std::error_code ec;
    const uint64_t size = std::filesystem::file_size(p, ec);
    if (!ec) {
      const auto t = std::filesystem::last_write_time(p, ec);
      if (ec) {
        if (error) *error = ec.message();
        return false;
      }
      out->size = size;
      out->mtime = static_cast<int64_t>(t.time_since_epoch().count());
      return true;
    }
    if (ec != std::errc::no_such_file_or_directory) {
      if (error) *error = ec.message();
      return false;
    }
  }
  return false;
}

namespace {

ModuleImage load_undamaged(const std::string& name, const std::string& dir) {
  check_name(name);
  if (!dir.empty()) return read_file(loose_path(name, dir));

  std::string resource = "K_";
  for (char c : name) resource.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  DataBlob blob;
  if (!find_data(resource.c_str(), &blob))
    throw DeviceError("kernel module '" + name + "' is not embedded in this build (resource " + resource + ")");
  ModuleImage image;
  image.bytes.assign(blob.data, blob.data + blob.size);
  image.origin = "embedded " + resource;
  return image;
}

}  // namespace

ModuleImage load_module_image_file(const std::string& path) { return read_file(path); }

}  // namespace astroray::host
