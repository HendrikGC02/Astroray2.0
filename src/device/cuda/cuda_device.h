// SPDX-License-Identifier: Apache-2.0
// The CUDA implementation of the Device seam, through the driver API only (no <<<>>>):
// one retained primary context per device, one stream and event per session, modules
// loaded on first launch, allocation under the reserve rule. No CUDA types appear here;
// the state lives in an Impl defined in cuda_device.cpp.
// Ref: docs/decisions/0002-kernel-architecture.md D4, D14, D15; 0001 D2.1, D2.5;
// 0007 d14 (reserve rule).
#pragma once
#include <memory>

#include "host/device.h"

namespace astroray::host {

class CudaDevice final : public ModuleDevice {
 public:
  // Throws DeviceError when CUDA cannot be initialised or is dead (a sticky error was
  // observed, in this or any other session of the process).
  CudaDevice(Ledger& ledger, uint32_t device_index);
  ~CudaDevice() override;

  void upload(const DeviceBuffer& dst, uint64_t offset, const void* src, uint64_t bytes) override;
  void download(void* dst, const DeviceBuffer& src, uint64_t offset, uint64_t bytes) override;
  void memset(const DeviceBuffer& dst, uint64_t offset, uint8_t value, uint64_t bytes) override;
  void set_kernel_globals(const DeviceBuffer& buffer) override;
  // Launches k_<stage name> from the module of that name. `grid` only sizes the grid; the
  // kernel is grid-stride over the step table's count, read on the device. Throws
  // std::logic_error without bound globals, std::invalid_argument for a stage that is not
  // launchable and DeviceError for a missing module or a failed CUDA call.
  void launch(ar::Stage stage, uint32_t grid) override;
  // Launches k_<kernel name>(const KernelGlobals*); `count` only sizes the grid.
  void launch(KernelId kernel, uint32_t count) override;
  void read_step_table(ar::StepTable& out) override;
  void synchronize() override;
  uint32_t kind() const override { return AR_DEVICE_CUDA; }

  uint32_t load_module_file(const std::string& path) override;
  void launch_module_kernel(uint32_t module, const char* function, uint32_t blocks, uint32_t threads,
                            void* const* args) override;

 protected:
  bool may_allocate(uint64_t need, uint64_t* available_bytes) override;
  std::string refusal_text(LedgerCategory category, uint64_t need, uint64_t available) const override;
  void* alloc_memory(uint64_t bytes) override;
  void free_memory(void* ptr) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace astroray::host
