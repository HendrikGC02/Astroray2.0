// SPDX-License-Identifier: Apache-2.0
// One OptixDeviceContext per scene, on the scene's CUDA primary context. Created on the
// scene's init thread (032); a failure is a DeviceError whose text carries the OptiX log
// (0002 D15: the GPU device is unavailable for the session). No OptiX or CUDA types
// appear here; the state lives in an Impl defined in optix_context.cpp.
// Ref: OptiX 9.1 Programming Guide, "Initialization" (optixInit, optixDeviceContextCreate);
// docs/decisions/0002-kernel-architecture.md D11, D15.
#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "host/device.h"

namespace astroray::host {

class OptixContext {
 public:
  // `device` is the scene's CudaDevice (its ledger accounts every allocation made through
  // it) and `device_index` the ordinal it was created with. Throws DeviceError.
  OptixContext(Device& device, uint32_t device_index);
  ~OptixContext();
  OptixContext(const OptixContext&) = delete;
  OptixContext& operator=(const OptixContext&) = delete;

  Device& device() const { return device_; }
  // Makes the CUDA context current on the calling thread. The context is per thread, so
  // every worker calls this before its first OptiX or CUDA call (the device's own methods
  // bind for themselves).
  void bind() const;
  // The handles, as raw values for optix_accel.cpp (OptixDeviceContext, CUstream).
  void* handle() const;
  void* stream() const;
  // Throws DeviceError naming `call`, the OptiX result name and the log collected so far
  // when `result` (an OptixResult) is not success.
  void check(int result, const char* call) const;
  // Sets the OptiX disk-cache directory of this context (it is a property of the context, not
  // of a pipeline). Creates the directory. A second call with the same path is a no-op; a
  // different path throws std::invalid_argument naming both.
  void set_cache_dir(const std::string& dir);
  // Waits for the OptiX stream; throws DeviceError on a CUDA error.
  void sync() const;

  // Device memory OptiX takes for itself, outside any allocation of ours: the context, and a fixed
  // ~230 MiB inside the first instance-AS build (measured on b1m and b5m alike, 126). free_bytes()
  // is the device's free memory now; record_drop(before) charges the drop since `before` to the
  // device's ledger as DEVICE_CONTEXT until this context is destroyed (0002 D13: the ledger total
  // must match the drop in free bytes). Another thread's allocation or free inside the window moves
  // the reading too, so the category can over- or under-report; that is why each cost is charged
  // once, as early as it appears: the context at creation, the instance build at the first one
  // (claim_first_instance_build). Module loads and OptiX module and pipeline creation measured
  // 0 MiB at cuMemGetInfo's resolution (126, b5m), so they have no record.
  uint64_t free_bytes() const;
  void record_drop(uint64_t free_before);
  // True exactly once per context: the caller is about to make the context's first instance build
  // and charges its drop with record_drop.
  bool claim_first_instance_build();
  // How many instance builds were charged (tests: 1 after any number of builds).
  uint32_t instance_build_charges() const;

 private:
  void release_records() noexcept;
  struct Impl;
  Device& device_;
  uint32_t index_;
  std::string cache_dir_;
  std::unique_ptr<Impl> impl_;
};

}  // namespace astroray::host
