// SPDX-License-Identifier: Apache-2.0
// The only place the CUDA runtime is called for enumeration. Static cudart loads
// nvcuda.dll dynamically, so a machine without a driver reports an error here
// instead of failing to load the DLL (0001 D3.7).
#include <cuda_runtime_api.h>

#include "host/device_probe.h"

namespace astroray {

CudaProbe probe_cuda() {
  CudaProbe p;
  int driver = 0;
  if (cudaDriverGetVersion(&driver) != cudaSuccess || driver == 0) {
    p.reason = "no CUDA device (no NVIDIA driver)";
    return p;
  }
  p.driver_version = driver;
  int count = 0;
  cudaError_t e = cudaGetDeviceCount(&count);
  if (e != cudaSuccess || count == 0) {
    p.reason = std::string("no CUDA device (") + cudaGetErrorString(e == cudaSuccess ? cudaErrorNoDevice : e) + ")";
    return p;
  }
  for (int i = 0; i < count; ++i) {
    CudaDeviceProps d;
    cudaDeviceProp prop;
    if (cudaGetDeviceProperties(&prop, i) != cudaSuccess) continue;
    d.name = prop.name;
    d.memory_bytes = prop.totalGlobalMem;
    cudaDeviceGetAttribute(&d.cc_major, cudaDevAttrComputeCapabilityMajor, i);
    cudaDeviceGetAttribute(&d.cc_minor, cudaDevAttrComputeCapabilityMinor, i);
    p.devices.push_back(d);
  }
  if (p.devices.empty()) p.reason = "no CUDA device (properties unreadable)";
  return p;
}

}  // namespace astroray
