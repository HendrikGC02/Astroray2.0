// SPDX-License-Identifier: Apache-2.0
// The CPU implementation of the Device seam: memory is host memory (256-B aligned),
// and a launch runs the registered chunk function over the step table's segment on
// the session's thread pool. Launches are synchronous.
// Ref: docs/decisions/0002-kernel-architecture.md D4, D7; 0001 D5.2.
#pragma once
#include <array>

#include "host/device.h"
#include "host/thread_pool.h"
#include "kernel_cpu/stage_cpu.h"

namespace astroray::host {

inline constexpr uint64_t kCpuAlignment = 256;

class CpuDevice : public Device {
 public:
  // The pool belongs to the session or scene; both it and the ledger outlive the device.
  CpuDevice(Ledger& ledger, ThreadPool& pool) : Device(ledger), pool_(pool) {}

  // The per-device entry-point tables. Throws std::invalid_argument for a null function
  // or a value that is not launchable (TERMINATED, SHADOW_TERMINATED), and
  // std::logic_error when the slot is already filled.
  void register_stage(ar::Stage stage, CpuStageFn fn);
  void register_kernel(KernelId kernel, CpuKernelFn fn);

  void upload(const DeviceBuffer& dst, uint64_t offset, const void* src, uint64_t bytes) override;
  void download(void* dst, const DeviceBuffer& src, uint64_t offset, uint64_t bytes) override;
  void memset(const DeviceBuffer& dst, uint64_t offset, uint8_t value, uint64_t bytes) override;
  void set_kernel_globals(const DeviceBuffer& buffer) override;
  // The stage launch runs exactly the step table's segment for that stage, whatever
  // `grid` is. Both launches throw std::logic_error without bound globals and
  // std::runtime_error for a stage or kernel with no entry point.
  void launch(ar::Stage stage, uint32_t grid) override;
  void launch(KernelId kernel, uint32_t count) override;
  void read_step_table(ar::StepTable& out) override;
  void synchronize() override {}
  uint32_t kind() const override { return AR_DEVICE_CPU; }

 protected:
  bool may_allocate(uint64_t, uint64_t* available_bytes) override;
  void* alloc_memory(uint64_t bytes) override;
  void free_memory(void* ptr) override;

 private:
  const ar::KernelGlobals& globals() const;

  ThreadPool& pool_;
  const ar::KernelGlobals* globals_ = nullptr;
  std::array<CpuStageFn, ar::kStageCount> stages_{};
  std::array<CpuKernelFn, kKernelIdCount> kernels_{};
};

}  // namespace astroray::host
