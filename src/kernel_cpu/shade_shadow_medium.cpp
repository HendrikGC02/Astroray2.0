// SPDX-License-Identifier: Apache-2.0
// shade_shadow_medium on the host thread pool: a kernel over the SHADE_SHADOW segment, launched by
// the scheduler before shade_shadow with the media feature (stages/shade_shadow_medium.h).
// Ref: docs/decisions/0002-kernel-architecture.md D4, D8, D9; 0006 D19; stages/shade_shadow_medium.h.
#include <type_traits>

#include "../kernel/stages/shade_shadow_medium.h"
#include "stage_cpu.h"

static_assert(std::is_same_v<decltype(&cpu_shade_shadow_medium), CpuKernelFn>, "registered as a CpuKernelFn");

void cpu_shade_shadow_medium(const ar::KernelGlobals& kg, uint32_t first, uint32_t last) {
  const ar::StepEntry step = kg.step_table->stage[static_cast<uint32_t>(ar::Stage::SHADE_SHADOW)];
  for (uint32_t item = first; item < last && item < step.count; ++item) ar::stage_shade_shadow_medium(kg, item, step);
}
