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

namespace {

// Little-endian field reads at a byte offset already known to lie inside the image.
uint64_t rd(const uint8_t* p, size_t at, int bytes) {
  uint64_t v = 0;
  for (int i = bytes - 1; i >= 0; --i) v = (v << 8) | p[at + static_cast<size_t>(i)];
  return v;
}

// [off, off + len) inside [0, size), without overflow.
bool inside(uint64_t off, uint64_t len, size_t size) { return off <= size && len <= size - off; }

}  // namespace

std::string cubin_layout_defect(const uint8_t* p, size_t size) {
  // Ref: System V gABI ch. 4, ELF Header: Elf64_Ehdr is 64 bytes; e_ident holds the magic
  // 0x7f 'E' 'L' 'F', EI_CLASS (byte 4) ELFCLASS64 = 2 and EI_DATA (byte 5) ELFDATA2LSB = 1;
  // e_phoff at 32, e_shoff at 40, e_ehsize at 52, e_phentsize 54, e_phnum 56, e_shentsize 58,
  // e_shnum 60, e_shstrndx 62.
  constexpr size_t kEhdr = 64, kPhdr = 56, kShdr = 64;
  if (p == nullptr || size < kEhdr) return "shorter than an ELF-64 header";
  if (p[0] != 0x7f || p[1] != 'E' || p[2] != 'L' || p[3] != 'F') return "no ELF magic";
  if (p[4] != 2 || p[5] != 1) return "not ELF-64 little-endian";
  if (rd(p, 52, 2) < kEhdr) return "ELF header size below 64";
  const uint64_t phoff = rd(p, 32, 8), shoff = rd(p, 40, 8);
  const uint64_t phentsize = rd(p, 54, 2), phnum = rd(p, 56, 2);
  const uint64_t shentsize = rd(p, 58, 2), shnum = rd(p, 60, 2), shstrndx = rd(p, 62, 2);
  // Extended numbering (e_shnum 0 with a section table, PN_XNUM, SHN_XINDEX) moves the counts into
  // section 0; no cubin uses it, so it is refused rather than half-checked.
  if (phnum == 0xffff || shstrndx == 0xffff || (shnum == 0 && shoff != 0)) return "extended ELF numbering";
  // Program Header: Elf64_Phdr is 56 bytes; p_offset at 8, p_filesz at 32.
  if (phnum != 0) {
    if (phentsize < kPhdr) return "program header entry below 56 bytes";
    if (!inside(phoff, phnum * phentsize, size)) return "program header table past the end";
    for (uint64_t i = 0; i < phnum; ++i) {
      const size_t at = static_cast<size_t>(phoff + i * phentsize);
      if (!inside(rd(p, at + 8, 8), rd(p, at + 32, 8), size)) return "segment " + std::to_string(i) + " past the end";
    }
  }
  // Sections: Elf64_Shdr is 64 bytes; sh_name at 0, sh_type at 4, sh_offset at 24, sh_size at 32;
  // SHT_NULL = 0 and SHT_NOBITS = 8 occupy no file space.
  if (shnum == 0) return "no section header table";
  if (shentsize < kShdr) return "section header entry below 64 bytes";
  if (!inside(shoff, shnum * shentsize, size)) return "section header table past the end";
  if (shstrndx == 0 || shstrndx >= shnum) return "section name table index out of range";
  for (uint64_t i = 0; i < shnum; ++i) {
    const size_t at = static_cast<size_t>(shoff + i * shentsize);
    const uint64_t type = rd(p, at + 4, 4);
    if (type != 0 && type != 8 && !inside(rd(p, at + 24, 8), rd(p, at + 32, 8), size))
      return "section " + std::to_string(i) + " past the end";
  }
  // Every section name starts inside the name table and ends with a NUL inside it.
  const size_t str_at = static_cast<size_t>(shoff + shstrndx * shentsize);
  const uint64_t str_off = rd(p, str_at + 24, 8), str_size = rd(p, str_at + 32, 8);
  if (rd(p, str_at + 4, 4) == 8) return "section name table has no file contents";
  for (uint64_t i = 0; i < shnum; ++i) {
    const uint64_t name = rd(p, static_cast<size_t>(shoff + i * shentsize), 4);
    if (name >= str_size) return "section " + std::to_string(i) + " name outside the name table";
    const uint8_t* s = p + str_off + name;
    if (std::find(s, p + str_off + str_size, uint8_t{0}) == p + str_off + str_size)
      return "section " + std::to_string(i) + " name not terminated";
  }
  return std::string();
}

std::string cubin_layout_message(const std::string& name, const std::string& defect) {
  return "module image truncated or invalid: " + name + " (" + defect + ")";
}

}  // namespace astroray::host
