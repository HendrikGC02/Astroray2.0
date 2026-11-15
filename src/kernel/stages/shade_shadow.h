// SPDX-License-Identifier: Apache-2.0
// shade_shadow: delivery of an unoccluded shadow item. intersect_shadow is an OptiX raygen,
// limited to traversal (0002 D11), so it only writes the outcome: SHADOW_TERMINATED when the
// segment is blocked, SHADE_SHADOW when it is clear. This stage then adds the item's
// contribution to its target, the per-sample buffer slot of the sample that emitted it (0004 D5:
// the path may have ended, or its pool slot been refilled, since), and frees the shadow slot.
// The item is self-contained: nothing of the creating path is read (0004 D4, 0003 d20).
// M1a has no non-opaque materials and no media, so there are no recorded hits to evaluate and
// no VM; M2 extends this stage with ah_record's hits (opacity through the VM) and transmittance.
// Ref: docs/decisions/0004-integrator.md D4 (delivery), D5 (late delivery to the sample's slot);
// 0002 D5, D8 (shade_shadow), D11 (outcomes); 0003 d20 (shadow item).
#pragma once
#include <cstdint>

#include "../film/film_write.h"
#include "../kernel_globals.h"
#include "../portability.h"
#include "../sched/step_table.h"
#include "../state/stage.h"

AR_NS_BEGIN

AR_FN void stage_shade_shadow(const KernelGlobals& kg, uint32_t item, const StepEntry& step) {
  const uint32_t slot = load_ro(kg.shadow_queue + step.start + item);
  sample_add(kg, sh_target(kg, slot), sh_contribution(kg, slot));
  sh_next_stage(kg, slot) = static_cast<uint8_t>(Stage::SHADOW_TERMINATED);
}

AR_NS_END

#if !AR_CUDA_BUILD
// CPU chunk function, defined by AR_CPU_STAGE in kernel_cpu/shade_shadow.cpp; registered by
// make_cpu_session_device.
void cpu_stage_shade_shadow(const ar::KernelGlobals& kg, const ar::StepEntry& step, uint32_t first, uint32_t last);
#endif
