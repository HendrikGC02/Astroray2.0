// SPDX-License-Identifier: Apache-2.0
// The OIDN library, loaded at run time by absolute path: no import library is linked, so the
// engine DLL imports nothing of OIDN and a host that never denoises never maps it. The entry
// points come from GetProcAddress into a table typed by OIDN's own header.
// Ref: docs/decisions/0001-product-shape.md D9.1 (our renamed build, loaded lazily by absolute path), D9.4
// (renamed modules, no collision with Blender's OIDN); third_party/_cache/oidn-2.5.0/extracted/
// include/OpenImageDenoise/oidn.h (the API, read there).
#pragma once
#include <OpenImageDenoise/oidn.h>

#include <string>

namespace astroray::host {

// The entry points the OIDN backend calls, by their oidn.h names.
#define AR_OIDN_FUNCTIONS(X)                                                                              \
  X(oidnIsCPUDeviceSupported) X(oidnIsCUDADeviceSupported) X(oidnNewDevice) X(oidnNewCUDADevice)          \
  X(oidnCommitDevice) X(oidnGetDeviceError) X(oidnGetDeviceInt) X(oidnReleaseDevice) X(oidnSyncDevice)    \
  X(oidnGetNumPhysicalDevices) X(oidnGetPhysicalDeviceInt) X(oidnGetPhysicalDeviceString)                 \
  X(oidnNewBuffer) X(oidnWriteBuffer) X(oidnReadBuffer) X(oidnReleaseBuffer) X(oidnNewFilter)             \
  X(oidnSetFilterImage) X(oidnSetFilterBool) X(oidnSetFilterInt) X(oidnCommitFilter) X(oidnExecuteFilter) \
  X(oidnReleaseFilter)

class OidnLibrary {
 public:
  // The module file the backend loads from `library_dir`; its sibling modules (core, devices)
  // must sit in the same folder.
  static constexpr const char* kModuleName = "astroray_oidn.dll";

  // Loads `library_dir`/astroray_oidn.dll with that folder searched for its dependencies and
  // resolves every entry point. Throws std::runtime_error naming the module path (or the empty
  // or relative folder) and the system's message when the folder is not absolute, the module
  // does not load or an entry point is missing.
  explicit OidnLibrary(const std::string& library_dir);
  ~OidnLibrary();
  OidnLibrary(const OidnLibrary&) = delete;
  OidnLibrary& operator=(const OidnLibrary&) = delete;

  // The absolute path of the loaded module, for messages.
  const std::string& path() const { return path_; }

#define AR_OIDN_MEMBER(n) decltype(&::n) n = nullptr;
  AR_OIDN_FUNCTIONS(AR_OIDN_MEMBER)
#undef AR_OIDN_MEMBER

 private:
  void* module_ = nullptr;  // HMODULE
  std::string path_;
};

}  // namespace astroray::host
