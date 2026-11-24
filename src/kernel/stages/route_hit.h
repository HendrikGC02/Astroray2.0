// SPDX-License-Identifier: Apache-2.0
// route_hit: the next stage of a traced segment, by table lookup only (no VM, texture or
// BSDF code), so it runs in the CPU intersect stage and inside OptiX raygens alike.
//   miss:            camera path SHADE_BACKGROUND, light path TERMINATED;
//   back face of a medium-bearing instance:  SHADE_VOLUME (the segment ran inside that medium);
//   lamp instance:   SHADE_LIGHT (pass-through, whichever path);
//   EXTENSION flag:  SHADE_SURFACE_EXT (camera) or SHADE_LIGHT_PATH_EXT (light);
//   otherwise:       SHADE_SURFACE (camera) or SHADE_LIGHT_PATH (light).
// The medium rule until the medium stack lands (one medium, no stack): a segment is inside the
// medium of instance I exactly when it ended on a back face of I, because the segment is traversed
// before whatever it hit is shaded. The rule tests geometry only, not sigma_t (a coefficient needs
// the VM, which route_hit never runs): a sigma_t = 0 medium is still routed and shade_volume
// forwards it unchanged. A segment that ends on another instance's front face inside the medium
// is not routed; the stack covers that. shade_volume reads the medium from the hit's instance
// and sends the hit on with route_hit_after_medium, the same function minus the medium test.
// The curved-region branch (0002 D11) arrives with M7.
// Ref: docs/decisions/0002-kernel-architecture.md D8, D11; 0005 item 17 (EXTENSION routing);
// 0006 D16-D20 (medium boundaries); Cycles kernel/integrator/intersect_closest.h (Apache-2.0) for
// the routing structure.
#pragma once
#include <cstdint>

#include "../core/vec.h"
#include "../geom/hit.h"
#include "../kernel_globals.h"
#include "../material/material_header.h"
#include "../portability.h"
#include "../scene/scene_view.h"
#include "../state/flags.h"
#include "../state/stage.h"

AR_NS_BEGIN

// Whether the hit triangle is seen from its back along ray_d. The side rule is
// shade_context_geometry's (stages/shade_common.h, which route_hit must not include: it pulls in
// the shading code): the front is the side the object-space winding (p1 - p0) x (p2 - p0) faces
// under any instance transform, so Ng = sign(det M) (M e1) x (M e2) and front = dot(Ng, -ray_d) > 0.
// Extra loads: three u32 vertex indices, nine float positions and the instance's twelve transform
// floats; the caller reaches them only for an instance that bears a medium. A degenerate triangle
// reads as a back face, as there; shade_surface then fails the sample.
AR_FN bool hit_is_back_face(const KernelGlobals& kg, const HitRecord& hit, Real3 ray_d) {
  const SceneData& s = scene_of(kg);
  const DeviceInstanceRecord& rec = s.instances[hit.inst];
  const DeviceMeshView& mv = s.meshes[load_ro(&rec.mesh)];
  const uint32_t* tri_vertex = reinterpret_cast<const uint32_t*>(uintptr_t(load_ro(&mv.tri_vertex)));
  const float* positions = reinterpret_cast<const float*>(uintptr_t(load_ro(&mv.positions)));
  const float* m = rec.object_to_world;
  const Real3 r0{Real(load_ro(m + 0)), Real(load_ro(m + 1)), Real(load_ro(m + 2))};
  const Real3 r1{Real(load_ro(m + 4)), Real(load_ro(m + 5)), Real(load_ro(m + 6))};
  const Real3 r2{Real(load_ro(m + 8)), Real(load_ro(m + 9)), Real(load_ro(m + 10))};
  const auto vertex = [&](uint32_t corner) {
    const uint32_t v = load_ro(tri_vertex + 3u * hit.prim + corner);
    return Real3{Real(load_ro(positions + 3u * v)), Real(load_ro(positions + 3u * v + 1u)),
                 Real(load_ro(positions + 3u * v + 2u))};
  };
  const Real3 p0 = vertex(0u), e1 = vertex(1u) - p0, e2 = vertex(2u) - p0;
  const Real3 a{dot(r0, e1), dot(r1, e1), dot(r2, e1)}, b{dot(r0, e2), dot(r1, e2), dot(r2, e2)};
  const Real det_sign = dot(r0, cross(r1, r2)) < Real(0) ? Real(-1) : Real(1);
  return !(dot(cross(a, b) * det_sign, -ray_d) > Real(0));
}

// The route of a hit (not a miss) that the medium test did not take.
AR_FN Stage route_hit_after_medium(const KernelGlobals& kg, const HitRecord& hit, uint32_t path_flags) {
  const bool light_path = (path_flags & PATH_KIND) != 0u;
  if (instance_light_index(kg, hit.inst) != kNoLightIndex) return Stage::SHADE_LIGHT;
  if ((hit_material_flags(kg, hit.inst, hit.prim) & EXTENSION) != 0u)
    return light_path ? Stage::SHADE_LIGHT_PATH_EXT : Stage::SHADE_SURFACE_EXT;
  return light_path ? Stage::SHADE_LIGHT_PATH : Stage::SHADE_SURFACE;
}

AR_FN Stage route_hit(const KernelGlobals& kg, const HitRecord& hit, Real3 ray_d, uint32_t path_flags) {
  if (hit.inst == kNoInstance) return (path_flags & PATH_KIND) != 0u ? Stage::TERMINATED : Stage::SHADE_BACKGROUND;
  if (instance_medium(kg, hit.inst) != kNoMedium && hit_is_back_face(kg, hit, ray_d)) return Stage::SHADE_VOLUME;
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
