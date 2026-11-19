// SPDX-License-Identifier: Apache-2.0
// Device enumeration and the synchronous checks of 0001 D2.8: no driver, driver
// below the floor, unsupported GPU, CPU without AVX2. The pure status functions
// take their inputs as arguments so tests can force each case.
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

#include "astroray/astroray.h"

namespace astroray {

struct DeviceStatus {
  ar_status status;
  std::string reason;  // empty when status is AR_OK
};

// The CUDA driver API version (1000 * major + 10 * minor, as cudaDriverGetVersion
// reports it) that the R590 driver branch ships: CUDA 13.1 is paired with R590 in
// "CUDA Toolkit and Corresponding Driver Branch" of the CUDA Toolkit release notes
// (docs.nvidia.com/cuda/cuda-toolkit-release-notes). 0001 D3.5 sets the floor at R590.
constexpr int kMinDriverCudaVersion = 13010;
constexpr int kMinComputeCapability = 75;  // 7.5, 0001 D3.5

// Ref: 0001 D3.6, D2.8.
DeviceStatus cpu_device_status(bool avx2);
// Ref: 0001 D3.5, D2.8. The driver is checked first.
DeviceStatus cuda_device_status(int driver_version, int cc_major, int cc_minor);

// What the CUDA probe (src/device/cuda/cuda_probe.cpp) reports. No CUDA type
// appears here, so host code stays free of CUDA headers.
struct CudaDeviceProps {
  std::string name;
  uint64_t memory_bytes = 0;
  int cc_major = 0;
  int cc_minor = 0;
};
struct CudaProbe {
  int driver_version = 0;  // 0 when there is no NVIDIA driver
  std::vector<CudaDeviceProps> devices;
  std::string reason;  // why devices is empty
};
// Defined in src/device/cuda/cuda_probe.cpp, which exists only in ASTRORAY_GPU builds.
CudaProbe probe_cuda();

struct DeviceRecord {
  uint32_t kind;
  uint32_t index;  // within its kind: what ar_context_desc.device_index takes
  DeviceStatus status;
  std::string name;
  uint64_t memory_bytes;
  uint32_t compute_capability;  // 10 * major + minor; 0 for the CPU
};

struct DeviceList {
  std::vector<DeviceRecord> devices;  // [0] is the CPU, then the CUDA devices
  std::string cuda_reason;            // non-empty when there is no CUDA device
};

// Probes the machine now; nothing is cached (no process-global state).
DeviceList list_devices();
// The CPU entry alone (list_devices()[0]) without touching CUDA: a CPU context needs only the AVX2
// check, and the first CUDA call in a process costs 13-16 ms of driver initialization (RULINGS 81).
DeviceRecord cpu_device();

}  // namespace astroray
