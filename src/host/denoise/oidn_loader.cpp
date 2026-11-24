// SPDX-License-Identifier: Apache-2.0
#include "host/denoise/oidn_loader.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <filesystem>
#include <stdexcept>

namespace astroray::host {
namespace {

std::string system_message(DWORD code) {
  char* text = nullptr;
  const DWORD n = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                 nullptr, code, 0, reinterpret_cast<char*>(&text), 0, nullptr);
  std::string s = n ? std::string(text, n) : std::string();
  if (text) LocalFree(text);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '.')) s.pop_back();
  return "Windows error " + std::to_string(code) + (s.empty() ? "" : ": " + s);
}

template <class Fn>
void resolve(HMODULE m, const std::string& path, const char* symbol, Fn& fn) {
  FARPROC p = GetProcAddress(m, symbol);
  if (!p) throw std::runtime_error("OIDN library " + path + " has no entry point " + symbol);
  fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(p));
}

}  // namespace

OidnLibrary::OidnLibrary(const std::string& library_dir) {
  if (library_dir.empty())
    throw std::runtime_error(std::string("OIDN library ") + kModuleName + " cannot be loaded: no library folder given");
  const std::filesystem::path dir = std::filesystem::u8path(library_dir);
  const std::filesystem::path file = dir / kModuleName;
  path_ = file.generic_u8string();
  // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR needs an absolute path; a relative one would also depend on
  // the process's current directory.
  if (!dir.is_absolute()) throw std::runtime_error("OIDN library " + path_ + " cannot be loaded: the folder is not absolute");
  // The module's own folder is searched for astroray_oidn_core.dll; nothing is taken from PATH or
  // the current directory, so Blender's OIDN can never stand in for ours (0001 D9.4).
  HMODULE m = LoadLibraryExW(std::filesystem::path(file).make_preferred().wstring().c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!m) throw std::runtime_error("OIDN library " + path_ + " cannot be loaded: " + system_message(GetLastError()));
  module_ = m;
  try {
#define AR_OIDN_RESOLVE(n) resolve(m, path_, #n, n);
    AR_OIDN_FUNCTIONS(AR_OIDN_RESOLVE)
#undef AR_OIDN_RESOLVE
  } catch (...) {
    FreeLibrary(m);
    module_ = nullptr;
    throw;
  }
}

OidnLibrary::~OidnLibrary() {
  if (module_) FreeLibrary(static_cast<HMODULE>(module_));
}

}  // namespace astroray::host
