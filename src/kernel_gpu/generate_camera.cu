// SPDX-License-Identifier: Apache-2.0
// k_generate_camera: one thread per refill item, grid-stride over the TERMINATED step entry the
// host overwrote for the launch (sched/step_table.h: count = items, start = first free-slot entry,
// shadow_base = first camera sample index).
// Ref: docs/decisions/0002-kernel-architecture.md D4, D7 step 4, D8; stages/generate_camera.h.
#include "launch_config.h"
#include "stages/generate_camera.h"

extern "C" __global__ void AR_LAUNCH_BOUNDS(ar::kLaunch_k_generate_camera.threads,
                                            ar::kLaunch_k_generate_camera.min_blocks)
    k_generate_camera(const ar::KernelGlobals* __restrict__ kg) {
  const ar::KernelGlobals& g = *kg;
  const ar::StepEntry step = g.step_table->stage[static_cast<uint32_t>(ar::Stage::TERMINATED)];
  for (uint32_t item = blockIdx.x * blockDim.x + threadIdx.x; item < step.count; item += gridDim.x * blockDim.x)
    ar::stage_generate_camera(g, item, step);
}
