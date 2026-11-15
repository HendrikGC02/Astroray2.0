// SPDX-License-Identifier: Apache-2.0
// Loader for the data files cmake/data.cmake embeds as RCDATA (0001 D8.4, 0003 d14).
#include "host/data_resource.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cwctype>
#include <vector>

namespace astroray {
namespace {

HMODULE this_module() {
  HMODULE m = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&find_data), &m);
  return m;
}

std::wstring upper_wide(const char* s) {
  std::wstring w;
  for (; *s; ++s) w.push_back(static_cast<wchar_t>(std::towupper(static_cast<unsigned char>(*s))));
  return w;
}

BOOL CALLBACK collect(HMODULE, LPCWSTR, LPWSTR name, LONG_PTR user) {
  if (!IS_INTRESOURCE(name)) static_cast<std::vector<std::wstring>*>(reinterpret_cast<void*>(user))->push_back(name);
  return TRUE;
}

bool find_wide(const std::wstring& name, DataBlob* out) {
  HMODULE m = this_module();
  HRSRC r = FindResourceW(m, name.c_str(), reinterpret_cast<LPCWSTR>(RT_RCDATA));
  if (!r) return false;
  HGLOBAL g = LoadResource(m, r);
  if (!g) return false;
  out->data = static_cast<const uint8_t*>(LockResource(g));
  out->size = SizeofResource(m, r);
  return out->data != nullptr;
}

}  // namespace

bool find_data(const char* name, DataBlob* out) { return find_wide(upper_wide(name), out); }

std::string check_observer_consistency() {
  std::vector<std::wstring> names;
  EnumResourceNamesW(this_module(), reinterpret_cast<LPCWSTR>(RT_RCDATA), collect, reinterpret_cast<LONG_PTR>(&names));
  if (names.empty()) return "no data files are embedded in this module";
  std::vector<std::pair<std::string, DataBlob>> files;
  for (const auto& w : names) {
    DataBlob b;
    std::string n;
    for (wchar_t ch : w) n.push_back(static_cast<char>(ch));  // names are ASCII paths
    if (!find_wide(w, &b)) return "cannot load embedded data file " + n;
    files.emplace_back(n, b);
  }
  return check_hash_set(files);
}

}  // namespace astroray
