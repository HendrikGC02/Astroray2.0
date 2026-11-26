// SPDX-License-Identifier: Apache-2.0
// route_hit: the next stage of a traced segment, by table lookup only (no VM, texture or
// BSDF code), so it runs in the CPU intersect stage and inside OptiX raygens alike.
//   miss:            camera path SHADE_BACKGROUND, light path TERMINATED;
//   medium-stack depth not 0:  SHADE_VOLUME (the segment ran inside the medium the stack names);
//   lamp instance:   SHADE_LIGHT (pass-through, whichever path);
//   EXTENSION flag:  SHADE_SURFACE_EXT (camera) or SHADE_LIGHT_PATH_EXT (light);
//   otherwise:       SHADE_SURFACE (camera) or SHADE_LIGHT_PATH (light).
// The medium test reads only the depth bits of the path flags (0003 d21): the stack holds what the
// path is inside while the segment is traversed, before whatever it hit is shaded, so the instance
// it ended on does not matter. The test does not look at sigma_t (a coefficient needs the VM, which
// route_hit never runs): a sigma_t = 0 medium is still routed and shade_volume forwards it
// unchanged. shade_volume takes the medium from the stack and sends the hit on with
// route_hit_after_medium, the same function minus the medium test. A path that starts inside a
// medium has depth 0 there until the camera's initial stack exists (0006 D20), so its segments are
// not routed.
// The curved-region branch (0002 D11) arrives with M7.
// Ref: docs/decisions/0002-kernel-architecture.md D8, D11; 0005 item 17 (EXTENSION routing);
// 0006 D16-D20 (medium boundaries); Cycles kernel/integrator/intersect_closest.h (Apache-2.0) for
// the routing structure.
#pragma once
#include <cstdint>

#include "../geom/hit.h"
#include "../kernel_globals.h"
#include "../material/material_header.h"
#include "../portability.h"
#include "../scene/scene_view.h"
#include "../state/flags.h"
#include "../state/stage.h"

AR_NS_BEGIN

// The route of a hit (not a miss) that the medium test did not take.
AR_FN Stage route_hit_after_medium(const KernelGlobals& kg, const HitRecord& hit, uint32_t path_flags) {
  const bool light_path = (path_flags & PATH_KIND) != 0u;
  if (instance_light_index(kg, hit.inst) != kNoLightIndex) return Stage::SHADE_LIGHT;
  if ((hit_material_flags(kg, hit.inst, hit.prim) & EXTENSION) != 0u)
    return light_path ? Stage::SHADE_LIGHT_PATH_EXT : Stage::SHADE_SURFACE_EXT;
  return light_path ? Stage::SHADE_LIGHT_PATH : Stage::SHADE_SURFACE;
}

AR_FN Stage route_hit(const KernelGlobals& kg, const HitRecord& hit, uint32_t path_flags) {
  if (hit.inst == kNoInstance) return (path_flags & PATH_KIND) != 0u ? Stage::TERMINATED : Stage::SHADE_BACKGROUND;
  if (medium_depth(path_flags) != 0u) return Stage::SHADE_VOLUME;
  return route_hit_after_medium(kg, hit, path_flags);
}

AR_NS_END

#if !AR_CUDA_BUILD
// CPU entry points (src/device/cpu/intersect_cpu.cpp), CpuStageFn chunk functions for
// Stage::INTERSECT_CLOSEST and INTERSECT_SHADOW; the host registers them on its CpuDevice.
// They read the traversal structure from KernelGlobals::scene->accel (an EmbreeScene).
void cpu_stage_intersect_closest(const ar::KernelGlobals& kg, const ar::StepEntry& step, uint32_t first,
                                 uint32_t last);
void cpu_stage_intersect_shadow(const ar::KernelGlobals& kg, const ar::StepEntry& step, uint32_t first,
                                uint32_t last);
#endif
