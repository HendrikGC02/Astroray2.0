// SPDX-License-Identifier: Apache-2.0
// k_shade_surface_ext: one thread per path of the SHADE_SURFACE_EXT segment, grid-stride over the step
// table's count read on the device, under the surface identities' launch bounds (256, 2).
// Ref: docs/decisions/0002-kernel-architecture.md D4, D6, D8, D9; stages/shade_surface_ext.h.
#include "launch_config.h"
#include "stages/shade_surface_ext.h"

extern "C" __global__ void AR_LAUNCH_BOUNDS(ar::kLaunch_k_shade_surface_ext.threads, ar::kLaunch_k_shade_surface_ext.min_blocks)
    k_shade_surface_ext(const ar::KernelGlobals* __restrict__ kg) {
  const ar::KernelGlobals& g = *kg;
  const ar::StepEntry step = g.step_table->stage[static_cast<uint32_t>(ar::Stage::SHADE_SURFACE_EXT)];
  for (uint32_t item = blockIdx.x * blockDim.x + threadIdx.x; item < step.count; item += gridDim.x * blockDim.x)
    ar::stage_shade_surface_ext(g, item, step);
}
