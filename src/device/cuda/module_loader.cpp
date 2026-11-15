// SPDX-License-Identifier: Apache-2.0
#include "device/cuda/module_loader.h"

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

ModuleImage read_file(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw DeviceError("cannot read kernel module file " + path.string());
  ModuleImage image;
  image.bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  if (f.bad() || image.bytes.empty())
    throw DeviceError("kernel module file " + path.string() + " is empty or unreadable");
  image.origin = path.string();
  return image;
}

}  // namespace

ModuleImage load_module_image(const std::string& name) {
  bool ok = !name.empty();
  for (char c : name) {
    const unsigned char u = static_cast<unsigned char>(c);
    ok = ok && (std::islower(u) || std::isdigit(u) || c == '_');
  }
  if (!ok)
    throw std::invalid_argument("kernel module name '" + name + "' is not lower-case letters, digits and underscores");

  const std::string source = kernel_source();
  if (source != "embedded") {
    // A cubin for CUDA stages, PTX for OptiX modules.
    const std::filesystem::path dir(source);
    if (std::filesystem::exists(dir / (name + ".cubin"))) return read_file(dir / (name + ".cubin"));
    return read_file(dir / (name + ".ptx"));
  }

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

ModuleImage load_module_image_file(const std::string& path) { return read_file(path); }

}  // namespace astroray::host
