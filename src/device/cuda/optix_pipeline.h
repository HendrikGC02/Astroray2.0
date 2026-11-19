// SPDX-License-Identifier: Apache-2.0
// The OptiX traversal pipeline: the module of optix_traverse.cu (PTX embedded as K_OPTIX_TRAVERSE),
// four program groups (two raygens, miss, triangle hit group), one SBT record per program, and
// the launch of either raygen over a step-table segment. Each CUDA session owns one, built on the
// scene's OptixContext (a traversable works only in the context that built it) with its SBT and
// launch parameters on the session's device and its launches on the session's stream, so they are
// ordered with the session's CUDA stages (RULINGS 70). Creation runs on the calling thread. The
// OptiX disk cache is the context's (ar_scene_desc.cache_dir, set when the scene's context was
// made). A second pipeline with motion blur enabled is created only on request. No OptiX types
// appear here.
// Ref: OptiX 9.1 Programming Guide (modules, program groups, pipelines, SBT, optixLaunch);
// docs/decisions/0002-kernel-architecture.md D11, D14; 0007 d45 (cache_dir); RULINGS 70.
#pragma once
#include <cstdint>
#include <memory>

#include "device/cuda/optix_context.h"
#include "host/device.h"

namespace astroray::host {

class TraversalPipeline {
 public:
  // Wall time of the two creation phases of one variant.
  struct Times {
    double module_ms = 0;    // optixModuleCreate
    double pipeline_ms = 0;  // program groups, optixPipelineCreate, stack sizes, SBT upload
  };

  // `device` allocates the SBT and the launch-parameter buffer (its ledger records them under
  // `owner`); `stream` (a CUstream on the context's CUDA context) carries every launch. Both and
  // `context` must outlive the pipeline. Throws DeviceError with the OptiX log (0002 D15).
  TraversalPipeline(OptixContext& context, Device& device, void* stream, Owner owner);
  ~TraversalPipeline();
  TraversalPipeline(const TraversalPipeline&) = delete;
  TraversalPipeline& operator=(const TraversalPipeline&) = delete;

  // Compiles the motion variant (usesMotionBlur) once; called only by the measurement of task 094.
  void create_motion_variant();
  bool has_motion_variant() const;
  Times times() const;
  Times motion_times() const;  // zero until create_motion_variant

  // The launch parameters: the traversable and the device buffer of the session's KernelGlobals.
  // Uploaded in stream order (one wait) only when they differ from the last values, so a launch
  // recorded in a CUDA graph reads them from device memory at replay and needs no host copy.
  void set_params(uint64_t traversable, const DeviceBuffer& kernel_globals);
  // Asynchronous on the pipeline's stream, with the parameters of the last set_params (throws
  // std::logic_error before the first). The raygen reads its segment from the step table and
  // strides over it with `width` threads.
  void launch_closest(uint32_t width);
  void launch_shadow(uint32_t width);
  // set_params, then the launch.
  void launch_closest(uint64_t traversable, const DeviceBuffer& kernel_globals, uint32_t width);
  void launch_shadow(uint64_t traversable, const DeviceBuffer& kernel_globals, uint32_t width);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace astroray::host
