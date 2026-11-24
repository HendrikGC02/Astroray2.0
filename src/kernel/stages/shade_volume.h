// SPDX-License-Identifier: Apache-2.0
// shade_volume: the stage for a segment that ran inside a medium (route_hit sends the back-face hit
// of a medium-bearing instance here). This skeleton does nothing physical: it reads the medium of
// the hit's instance, then writes `next_stage` as route_hit would have without the medium test
// (route_hit_after_medium), so a light or extension hit behind a medium is not sent to
// SHADE_SURFACE. Tracking, the marcher and volume NEE arrive with the stage's estimators.
// The medium id is not stored in the path state: the instance the segment ended on already names
// it (instance_medium), and the path keeps the hit record for the stage that follows.
// Ref: docs/decisions/0002-kernel-architecture.md D8 (shade_volume), D11 (medium routing);
// 0006 D8 (flow).
#pragma once
#include <cstdint>

#include "../kernel_globals.h"
#include "../portability.h"
#include "../scene/scene_view.h"
#include "../sched/step_table.h"
#include "../state/stage.h"
#include "route_hit.h"

AR_NS_BEGIN

AR_FN void stage_shade_volume(const KernelGlobals& kg, uint32_t item, const StepEntry& step) {
  const uint32_t slot = load_ro(kg.main_queue + step.start + item);
  const uint32_t inst = ps_hit_inst(kg, slot);
  const uint32_t medium = instance_medium(kg, inst);
  AR_ASSERT(medium != kNoMedium);  // route_hit sends only medium-bearing instances here
  (void)medium;
  const Real2 uv = ps_hit_uv(kg, slot);
  const HitRecord hit{ps_hit_t(kg, slot), ps_hit_prim(kg, slot), inst, uv.x, uv.y};
  ps_next_stage(kg, slot) = static_cast<uint8_t>(route_hit_after_medium(kg, hit, ps_flags(kg, slot)));
}

AR_NS_END

#if !AR_CUDA_BUILD
// CPU chunk function, defined by AR_CPU_STAGE in kernel_cpu/shade_volume.cpp; registered by
// make_cpu_session_device.
void cpu_stage_shade_volume(const ar::KernelGlobals& kg, const ar::StepEntry& step, uint32_t first, uint32_t last);
#endif
