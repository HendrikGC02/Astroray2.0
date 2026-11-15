// SPDX-License-Identifier: Apache-2.0
// shade_background: a camera path that left the scene sees the constant world. It adds
// Le(lambda_t) * beta to the sample's slot (weight 1: MIS against NEE arrives with 074) and ends
// the path ESCAPED. A non-finite contribution adds nothing and ends the path INVALID (rule 5), so
// one bad value never reaches the film. Light-path misses never get here (route_hit, 0002 D8).
// Radiance already carries the radiance scale s in the light record (068); the film divides it
// out, so it is not applied here.
// Ref: docs/decisions/0004-integrator.md D5, D6 (world row), D28; 0002 D8 (shade_background);
// Cycles kernel/integrator/shade_background.h (Apache-2.0) for the stage structure.
#pragma once
#include <cstdint>

#include "../kernel_globals.h"
#include "../light/light_sampler.h"
#include "../portability.h"
#include "../sched/step_table.h"
#include "../vm/context.h"
#include "../state/flags.h"
#include "shade_common.h"

AR_NS_BEGIN

// The world's record, or nullptr when the scene has none (the host light table records its
// index, kg_builder copies it).
AR_FN const LightRecord* world_light(const KernelGlobals& kg) {
  if (kg.world_light == kNoLight) return nullptr;
  AR_ASSERT(kg.lights != nullptr && kg.light_sampler != nullptr && kg.world_light < kg.light_sampler->light_count);
  return &kg.lights[kg.world_light];
}

AR_FN void stage_shade_background(const KernelGlobals& kg, uint32_t item, const StepEntry& step) {
  const uint32_t slot = load_ro(kg.main_queue + step.start + item);
  AR_ASSERT((ps_flags(kg, slot) & PATH_KIND) == 0u);
  Term term = Term::ESCAPED;
  const LightRecord* world = world_light(kg);
  if (world != nullptr) {
    const Spec lt = ps_lambda_t(kg, slot);
    Lambda lam;
    for (int i = 0; i < kLanes; ++i) lam.nm[i] = lt.v[i];
    const Spec contribution = light_emission(*world, lam, kg.tables->observer) * ps_beta(kg, slot);
    if (is_finite(contribution))
      sample_add(kg, slot, contribution);
    else
      term = Term::INVALID;
  }
  path_end(kg, slot, term);
}

AR_NS_END

#if !AR_CUDA_BUILD
// CPU chunk function, defined by AR_CPU_STAGE in kernel_cpu/shade_background.cpp; registered by
// make_cpu_session_device.
void cpu_stage_shade_background(const ar::KernelGlobals& kg, const ar::StepEntry& step, uint32_t first,
                                uint32_t last);
#endif
