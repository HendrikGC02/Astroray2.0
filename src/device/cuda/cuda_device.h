// SPDX-License-Identifier: Apache-2.0
// The CUDA implementation of the Device seam, through the driver API only (no <<<>>>):
// one retained primary context per device, one stream and event per session, modules
// loaded on first launch, allocation under the reserve rule. INTERSECT_CLOSEST and
// INTERSECT_SHADOW are OptiX raygens: a session device launches them through its own
// TraversalPipeline on the scene's OptiX context. No CUDA types appear here; the state lives
// in an Impl defined in cuda_device.cpp.
// Ref: docs/decisions/0002-kernel-architecture.md D4, D8, D11, D14, D15; 0001 D2.1, D2.5;
// 0007 d14 (reserve rule); RULINGS 70.
#pragma once
#include <memory>
#include <string>

#include "device/cuda/optix_pipeline.h"
#include "host/device.h"
#include "kernel/launch_config.h"

namespace astroray::host {

class AccelBuilder;

// launch_config.h's T for `kernel`: throws std::logic_error naming the kernel when `row` is null or
// has no threads, or when `function_limit` (CU_FUNC_ATTRIBUTE_MAX_THREADS_PER_BLOCK of the compiled
// function; negative: not checked yet) is below T.
uint32_t checked_launch_threads(const std::string& kernel, const ar::LaunchRow* row, int function_limit);

class CudaDevice final : public ModuleDevice {
 public:
  // Throws DeviceError when CUDA cannot be initialised or is dead (a sticky error was
  // observed, in this or any other session of the process).
  CudaDevice(Ledger& ledger, uint32_t device_index);
  ~CudaDevice() override;

  void upload(const DeviceBuffer& dst, uint64_t offset, const void* src, uint64_t bytes) override;
  // Stages `src` in the device's pinned ring and enqueues the copy; when the ring is full it
  // waits once first. Throws std::invalid_argument above the ring's size.
  void upload_async(const DeviceBuffer& dst, uint64_t offset, const void* src, uint64_t bytes) override;
  void download(void* dst, const DeviceBuffer& src, uint64_t offset, uint64_t bytes) override;
  void memset(const DeviceBuffer& dst, uint64_t offset, uint8_t value, uint64_t bytes) override;
  void copy(const DeviceBuffer& dst, uint64_t dst_offset, const DeviceBuffer& src, uint64_t src_offset,
            uint64_t bytes) override;
  void set_kernel_globals(const DeviceBuffer& buffer) override;
  // Launches k_<stage name> from the module of that name with launch_config.h's T threads per
  // block, or, for INTERSECT_CLOSEST and INTERSECT_SHADOW, the attached TraversalPipeline's raygen
  // on the traversable set by set_traversable. `grid` only sizes the grid; the kernel is
  // grid-stride over the step table's count, read on the device. Throws std::logic_error without
  // bound globals, without an attached pipeline or traversable for a traversal stage, or for a
  // kernel without a launch_config.h row or whose compiled limit is below T;
  // std::invalid_argument for a stage that is not launchable or has no CUDA route; DeviceError
  // for a missing module or a failed CUDA call.
  void launch(ar::Stage stage, uint32_t grid) override;
  // Launches k_<kernel name>(const KernelGlobals*) with launch_config.h's T; `count` only sizes
  // the grid. Throws as launch(Stage) for a missing row, limit or module.
  void launch(KernelId kernel, uint32_t count) override;

  // Builds this device's TraversalPipeline on the OptiX context of `scene_accel` (the scene's
  // SceneOptixBuilder, created by the scene's init), its SBT and launch parameters in this
  // device's ledger under `owner`, its launches on this device's stream. The scene's context
  // must outlive the attachment: detach_traversal before the scene goes. Throws
  // std::invalid_argument for a builder that is not OptiX, std::logic_error when one is
  // attached, DeviceError from OptiX.
  TraversalPipeline::Times attach_traversal(AccelBuilder& scene_accel, Owner owner);
  // Waits for its launches and frees the pipeline; nothing when none is attached. The pipeline and
  // the traversable are gone on return even when it throws (a dead device: the DeviceError of the
  // failed bind is rethrown after the pipeline was freed).
  void detach_traversal();
  // Test hook: marks the device dead as a sticky error would; every later call throws `why`.
  void poison_for_test(const std::string& why);
  // The OptixTraversableHandle the traversal stages trace: the pinned snapshot's SceneData::accel
  // (scene_view.h), set before every pass that launches them.
  void set_traversable(uint64_t handle);
  void read_step_table(ar::StepTable& out) override;
  void synchronize() override;
  uint32_t kind() const override { return AR_DEVICE_CUDA; }
  std::string untested_architecture() const override;

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
