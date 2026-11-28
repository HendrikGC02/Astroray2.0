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

// The architecture the CUDA device modules were built for ("sm_120"; cmake/kernels.cmake,
// ASTRORAY_CUDA_ARCH); empty in a build without the CUDA backend.
#define AR_BUILD_STR_(x) #x
#define AR_BUILD_STR(x) AR_BUILD_STR_(x)
inline std::string built_cuda_arch() {
#ifdef AR_BUILT_CUDA_ARCH
  return "sm_" AR_BUILD_STR(AR_BUILT_CUDA_ARCH);
#else
  return {};
#endif
}

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
