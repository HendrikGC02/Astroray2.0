// SPDX-License-Identifier: Apache-2.0
#include "host/device_probe.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace astroray {

namespace {
std::string version_text(int v) { return std::to_string(v / 1000) + "." + std::to_string((v % 1000) / 10); }
}  // namespace

DeviceStatus cpu_device_status(bool avx2) {
  if (!avx2) return {AR_ERR_DEVICE, "CPU device needs AVX2"};
  return {AR_OK, ""};
}

DeviceStatus cuda_device_status(int driver_version, int cc_major, int cc_minor) {
  if (driver_version < kMinDriverCudaVersion) {
    return {AR_ERR_DEVICE, "driver " + version_text(driver_version) + " is older than required " +
                               version_text(kMinDriverCudaVersion) + " (R590)"};
  }
  if (cc_major * 10 + cc_minor < kMinComputeCapability) {
    return {AR_ERR_DEVICE, "GPU compute capability " + std::to_string(cc_major) + "." + std::to_string(cc_minor) +
                               " is below the required 7.5"};
  }
  return {AR_OK, ""};
}

DeviceRecord cpu_device() {
  bool avx2 = IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE) != 0;
  return {AR_DEVICE_CPU, 0, cpu_device_status(avx2), "CPU", 0, 0};
}

DeviceList list_devices() {
  DeviceList list;
  list.devices.push_back(cpu_device());
#ifdef ASTRORAY_GPU
  CudaProbe p = probe_cuda();
  uint32_t i = 0;
  for (const CudaDeviceProps& d : p.devices) {
    list.devices.push_back({AR_DEVICE_CUDA, i++, cuda_device_status(p.driver_version, d.cc_major, d.cc_minor),
                            d.name, d.memory_bytes, static_cast<uint32_t>(d.cc_major * 10 + d.cc_minor)});
  }
  if (p.devices.empty()) {
    if (p.driver_version != 0 && p.driver_version < kMinDriverCudaVersion) {
      // The runtime refuses to enumerate on a driver this old, but the machine does
      // have a driver: list one refused entry so the reason reaches the user.
      list.devices.push_back({AR_DEVICE_CUDA, 0, cuda_device_status(p.driver_version, 0, 0),
                              "NVIDIA GPU (driver too old to enumerate)", 0, 0});
    } else {
      list.cuda_reason = p.reason;
    }
  }
#else
  list.cuda_reason = "no CUDA device (this build has no CUDA support)";
#endif
  return list;
}

}  // namespace astroray
