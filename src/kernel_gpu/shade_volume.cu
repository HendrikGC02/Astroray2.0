// SPDX-License-Identifier: Apache-2.0
// k_shade_volume: one thread per path of the SHADE_VOLUME segment, grid-stride over the step
// table's count read on the device.
// Ref: docs/decisions/0002-kernel-architecture.md D4, D6, D8; stages/shade_volume.h.
#include "launch_config.h"
#include "stages/shade_volume.h"

extern "C" __global__ void AR_LAUNCH_BOUNDS(ar::kLaunch_k_shade_volume.threads, ar::kLaunch_k_shade_volume.min_blocks)
    k_shade_volume(const ar::KernelGlobals* __restrict__ kg) {
  const ar::KernelGlobals& g = *kg;
  const ar::StepEntry step = g.step_table->stage[static_cast<uint32_t>(ar::Stage::SHADE_VOLUME)];
  for (uint32_t item = blockIdx.x * blockDim.x + threadIdx.x; item < step.count; item += gridDim.x * blockDim.x)
    ar::stage_shade_volume(g, item, step);
}
