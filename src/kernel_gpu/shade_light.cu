// SPDX-License-Identifier: Apache-2.0
// k_shade_light: one thread per path of the SHADE_LIGHT segment, grid-stride over the step
// table's count read on the device.
// Ref: docs/decisions/0002-kernel-architecture.md D4, D6, D8; stages/shade_light.h.
#include "launch_config.h"
#include "stages/shade_light.h"

extern "C" __global__ void AR_LAUNCH_BOUNDS(ar::kLaunch_k_shade_light.threads, ar::kLaunch_k_shade_light.min_blocks)
    k_shade_light(const ar::KernelGlobals* __restrict__ kg) {
  const ar::KernelGlobals& g = *kg;
  const ar::StepEntry step = g.step_table->stage[static_cast<uint32_t>(ar::Stage::SHADE_LIGHT)];
  for (uint32_t item = blockIdx.x * blockDim.x + threadIdx.x; item < step.count; item += gridDim.x * blockDim.x)
    ar::stage_shade_light(g, item, step);
}
