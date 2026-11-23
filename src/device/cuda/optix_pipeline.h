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

  // Compiles the motion variant (usesMotionBlur) once; called only by the motion-blur measurement.
  void create_motion_variant();
  bool has_motion_variant() const;
  Times times() const;
  Times motion_times() const;  // zero until create_motion_variant

  // Asynchronous on the pipeline's stream. `kernel_globals` is the device
  // buffer of the session's KernelGlobals; the raygen reads its segment from the step table and
  // strides over it with `width` threads. The launch parameters are copied in stream order, so
  // back-to-back launches are safe.
  void launch_closest(uint64_t traversable, const DeviceBuffer& kernel_globals, uint32_t width);
  void launch_shadow(uint64_t traversable, const DeviceBuffer& kernel_globals, uint32_t width);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace astroray::host
