// SPDX-License-Identifier: Apache-2.0
// k_shade_shadow_medium: one thread per item of the SHADE_SHADOW segment of the shadow pool,
// grid-stride over the step table's count read on the device; launched before k_shade_shadow with the
// media feature (stages/shade_shadow_medium.h).
// Ref: docs/decisions/0002-kernel-architecture.md D4, D6, D8, D9; 0006 D19; stages/shade_shadow_medium.h.
#include "launch_config.h"
#include "stages/shade_shadow_medium.h"

extern "C" __global__ void AR_LAUNCH_BOUNDS(ar::kLaunch_k_shade_shadow_medium.threads, ar::kLaunch_k_shade_shadow_medium.min_blocks)
    k_shade_shadow_medium(const ar::KernelGlobals* __restrict__ kg) {
  const ar::KernelGlobals& g = *kg;
  const ar::StepEntry step = g.step_table->stage[static_cast<uint32_t>(ar::Stage::SHADE_SHADOW)];
  for (uint32_t item = blockIdx.x * blockDim.x + threadIdx.x; item < step.count; item += gridDim.x * blockDim.x)
    ar::stage_shade_shadow_medium(g, item, step);
}
