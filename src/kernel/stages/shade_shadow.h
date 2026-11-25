// SPDX-License-Identifier: Apache-2.0
// shade_shadow: delivery of an unoccluded shadow item. intersect_shadow is an OptiX raygen,
// limited to traversal (0002 D11), so it only writes the outcome: SHADOW_TERMINATED when the
// segment is blocked, SHADE_SHADOW when it is clear. This stage then adds the item's
// contribution to its target, the per-sample buffer slot of the sample that emitted it (0004 D5:
// the path may have ended, or its pool slot been refilled, since), and frees the shadow slot.
// The item is self-contained: nothing of the creating path is read (0004 D4, 0003 d20).
// M1a has no non-opaque materials and no media, so there are no recorded hits to evaluate; M2
// extends this stage with ah_record's hits (opacity through the VM) and transmittance.
// An item flagged ITEM_WORLD_LE (NEE toward an environment world, light/world_env.h) still lacks
// the world's radiance: it is the world program run along ray_d at the item's lanes, through
// shade_background's world_program_radiance in the background context with no ray type and depth 0
// (as the bake evaluates it). The NEE stage already applied the MIS weight and the record's
// strength. A program that cannot run, or a non-finite product, marks the sample invalid (rule 5).
// This makes shade_shadow a VM kernel (0002 D10 lists it among them).
// Ref: docs/decisions/0004-integrator.md D4 (delivery), D5 (late delivery to the sample's slot),
// D11 (radiance from the world program, never the table); 0003 d20 (item_flags, lambda_t);
// 0002 D5, D8 (shade_shadow), D11 (outcomes); 0003 d20 (shadow item).
#pragma once
#include <cstdint>

#include "../film/film_write.h"
#include "../kernel_globals.h"
#include "../portability.h"
#include "../sched/step_table.h"
#include "../state/flags.h"
#include "../state/stage.h"
#include "shade_background.h"

AR_NS_BEGIN

AR_FN void stage_shade_shadow(const KernelGlobals& kg, uint32_t item, const StepEntry& step) {
  const uint32_t slot = load_ro(kg.shadow_queue + step.start + item);
  Spec c = sh_contribution(kg, slot);
  if (kg.shadow->item_flags != nullptr && (sh_item_flags(kg, slot) & ITEM_WORLD_LE) != 0u) {
    const Spec lt = sh_lambda_t(kg, slot);
    Lambda lam;
    for (int i = 0; i < kLanes; ++i) lam.nm[i] = lt.v[i];
    Spec le;
    if (kg.scene == nullptr || !world_program_radiance(*kg.scene, *kg.tables, world_shade_context(normalize(sh_ray_d(kg, slot)), lam), &le) ||
        !is_finite(c * le)) {
      sample_mark_invalid(kg, sh_target(kg, slot));
      sh_next_stage(kg, slot) = static_cast<uint8_t>(Stage::SHADOW_TERMINATED);
      return;
    }
    c *= le;
  }
  sample_add(kg, sh_target(kg, slot), c);
  sh_next_stage(kg, slot) = static_cast<uint8_t>(Stage::SHADOW_TERMINATED);
}

AR_NS_END

#if !AR_CUDA_BUILD
// CPU chunk function, defined by AR_CPU_STAGE in kernel_cpu/shade_shadow.cpp; registered by
// make_cpu_session_device.
void cpu_stage_shade_shadow(const ar::KernelGlobals& kg, const ar::StepEntry& step, uint32_t first, uint32_t last);
#endif
