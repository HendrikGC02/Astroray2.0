// SPDX-License-Identifier: Apache-2.0
// The OptiX traversal pipeline: the module of optix_traverse.cu (PTX embedded as K_OPTIX_TRAVERSE),
// four program groups (two raygens, miss, triangle hit group), one SBT record per program, and
// the launch of either raygen over a step-table segment. Creation runs on the calling thread;
// the scene's init thread calls it (032), so scene sync proceeds in parallel. The OptiX disk
// cache lives in the directory the host supplies (ar_scene_desc.cache_dir). A second pipeline
// with motion blur enabled is created only on request. No OptiX types appear here.
// Ref: OptiX 9.1 Programming Guide (modules, program groups, pipelines, SBT, optixLaunch);
// docs/decisions/0002-kernel-architecture.md D11, D14; 0007 d45 (cache_dir).
#pragma once
#include <cstdint>
#include <memory>
#include <string>

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

  // `cache_dir` is created when missing and must not be empty. Throws DeviceError with the
  // OptiX log (0002 D15), std::invalid_argument for an empty directory.
  TraversalPipeline(OptixContext& context, Owner owner, const std::string& cache_dir);
  ~TraversalPipeline();
  TraversalPipeline(const TraversalPipeline&) = delete;
  TraversalPipeline& operator=(const TraversalPipeline&) = delete;

  // Compiles the motion variant (usesMotionBlur) once; called only by the measurement of task 094.
  void create_motion_variant();
  bool has_motion_variant() const;
  Times times() const;
  Times motion_times() const;  // zero until create_motion_variant

  // Asynchronous on the OptiX stream (OptixContext::sync waits). `kernel_globals` is the device
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
