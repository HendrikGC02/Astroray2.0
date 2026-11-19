// SPDX-License-Identifier: Apache-2.0
// Test-only module (never embedded): the shade_background stage plus a known constant. The dev-reload test
// copies it over shade_background.cubin in a temporary ASTRORAY_KERNEL_DIR, so a pass that ran this
// module shows kReloadConstant in every escaped path's sample, which the production stage does not add
// to a scene with no world.
// Ref: docs/decisions/0002-kernel-architecture.md D14 (dev module swap); src/kernel_gpu/shade_background.cu.
#include "launch_config.h"
#include "stages/shade_background.h"

constexpr float kReloadConstant = 1.0f;

extern "C" __global__ void AR_LAUNCH_BOUNDS(ar::kLaunch_k_shade_background.threads,
                                            ar::kLaunch_k_shade_background.min_blocks)
    k_shade_background(const ar::KernelGlobals* __restrict__ kg) {
  const ar::KernelGlobals& g = *kg;
  const ar::StepEntry step = g.step_table->stage[static_cast<uint32_t>(ar::Stage::SHADE_BACKGROUND)];
  for (uint32_t item = blockIdx.x * blockDim.x + threadIdx.x; item < step.count; item += gridDim.x * blockDim.x) {
    const uint32_t slot = ar::load_ro(g.main_queue + step.start + item);
    ar::sample_add(g, ar::ps_sample_id(g, slot), ar::spec_fill(kReloadConstant));
    ar::stage_shade_background(g, item, step);
  }
}
