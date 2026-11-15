// SPDX-License-Identifier: Apache-2.0
// Build identity: generated build_info.cpp holds the values, this header is the
// one way to read them (0001 D2.7, 0009 d5).
#pragma once
#include <stdlib.h>

#include <string>

namespace astroray {

struct BuildInfo {
  const char* version;
  const char* git_sha;
  int dirty;
  const char* cuda;
  const char* optix;
  const char* embree;
  const char* oidn;
  const char* build_type;
};

extern const BuildInfo kBuildInfo;

// "embedded", or ASTRORAY_KERNEL_DIR in a build compiled with AR_DEV_KERNEL_DIR
// (the dev presets only), which is the one build allowed to load loose kernels.
inline std::string kernel_source() {
#ifdef AR_DEV_KERNEL_DIR
  char* dir = nullptr;
  size_t len = 0;
  if (_dupenv_s(&dir, &len, "ASTRORAY_KERNEL_DIR") == 0 && dir != nullptr) {
    std::string s(dir);
    free(dir);
    if (!s.empty()) return s;
  }
#endif
  return "embedded";
}

}  // namespace astroray
