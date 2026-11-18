// SPDX-License-Identifier: Apache-2.0
// shade_light: a path's ray crossed a lamp primitive (route_hit sends every lamp-instance hit
// here). A lamp crossing is a pass-through, not a vertex (0004 D3, D8):
//  - a camera path adds the lamp's radiance toward the ray, if the ray meets the emitting face,
//    times beta and the emitter-hit MIS weight (integrator/mis.h) to its sample's slot (0004 D5);
//  - a light path adds nothing (light paths pass through lamps, 0004 D8);
//  - both re-queue INTERSECT_CLOSEST from just past the lamp (geom/ray_offset.h
//    offset_past_surface) with the same direction. Bounce counters, event_index and the
//    previous-vertex normals stay as they were; prev_dist grows by the distance the origin moved
//    along the ray, so ray_o - prev_dist ray_d is still the previous scattering vertex (0003 d19
//    row 17) and an emitter reached after the crossing is weighted from the right context.
// Emitting face: every lamp emits from one side only, the area lamps toward their front (local
// -Z) and the sphere lamp outward (light/sphere.h), so Le is added only when dot(ray_d, n) < 0
// with n that face's normal. A camera ray through a sphere lamp therefore adds L once: its
// second crossing, from inside on the way out, meets the non-emitting side and adds nothing.
// MIS: as shade_surface's emission hit, with p_light = pmf(ctx_prev, light) pdf_li(ctx_prev, wi)
// from the previous vertex's context after a non-delta bounce, and weight 1 / avg(r_u) at the
// first vertex or after a delta bounce (LAST_BOUNCE_SPECULAR). The NEE test modes
// (kernel_globals.h TEST_HOOK_NEE_*) weight the hit 1 (off) or drop it after a non-delta bounce
// (light_only); the crossing itself is unchanged.
// Lamp records already carry the radiance scale s (068), so Le is not multiplied by it here.
// Lamp crossings are counted by the scheduler from this stage's item count (0004 D29): every
// item is one crossing. A non-finite contribution marks the sample invalid and ends the path
// INVALID (0004 D17, 0008 d6).
// M1a lamps emit a constant colour times power; emission programs, IES and media (0004 D8's
// transmittance) arrive with M2 and M4.
// Ref: Cycles kernel/integrator/shade_light.h integrate_light (Apache-2.0): add the light's
// emission with its MIS weight, then advance the ray origin past the lamp and re-queue
// intersect_closest without changing the bounce state; pbrt-v4 src/pbrt/cpu/integrators.cpp
// VolPathIntegrator::Li (emitter-hit MIS); docs/decisions/0004-integrator.md D1, D3, D5, D6, D8;
// 0003 d19 (prev_dist); 0002 D8, D16.
#pragma once
#include <cstdint>

#include "../geom/ray_offset.h"
#include "../integrator/mis.h"
#include "../kernel_globals.h"
#include "../light/light.h"
#include "../light/light_sampler.h"
#include "../portability.h"
#include "../sched/step_table.h"
#include "../state/flags.h"
#include "../state/stage.h"
#include "shade_common.h"

AR_NS_BEGIN

// The hit on a lamp: the point on the ray and the normal of the emitting face there (outward for
// a sphere, the front for a rectangle or disk). Lamp shapes: geom/lamp_intersect.h.
struct LampCrossing {
  Real3 p, n;
  bool valid;
};

AR_FN LampCrossing lamp_crossing(const LightRecord& r, Real3 o, Real3 d, Real t) {
  LampCrossing c;
  c.p = o + d * t;
  c.n = Real3{Real(0), Real(0), Real(1)};
  c.valid = true;
  switch (LightType(r.type)) {
    case LightType::SPHERE: {
      const Real3 v = c.p - light_position(r);
      const Real len = length(v);
      if (!(len > Real(0))) c.valid = false;
      else c.n = v / len;
      break;
    }
    case LightType::AREA_RECT:
    case LightType::AREA_DISK: c.n = area_front(r); break;
    default: c.valid = false;  // point lamps have no primitive; triangles and the world are no lamps
  }
  return c;
}

// Le toward the ray of the emitting face; the caller has checked that the ray meets that face.
AR_FN Spec lamp_radiance(const LightRecord& r, const Lambda& lam, const ObserverView& obs) {
  switch (LightType(r.type)) {
    case LightType::SPHERE: return sphere_radiance(r, lam, obs);
    case LightType::AREA_RECT: return rect_radiance(r, lam, obs);
    case LightType::AREA_DISK: return disk_radiance(r, lam, obs);
    default: break;
  }
  AR_ASSERT(false);
  return spec_fill(Real(0));
}

// light_pdf_li for the three lamp types only: the triangle and world rows of the full dispatch
// can never be reached here and would only add code to the kernel.
AR_FN Real lamp_pdf_li(const LightRecord& r, const LightSampleContext& ctx, Real3 wi, const LightHit& hit) {
  switch (LightType(r.type)) {
    case LightType::SPHERE: return sphere_pdf_li(r, ctx, wi, hit);
    case LightType::AREA_RECT: return rect_pdf_li(r, ctx, wi, hit);
    case LightType::AREA_DISK: return disk_pdf_li(r, ctx, wi, hit);
    default: break;
  }
  AR_ASSERT(false);
  return Real(0);
}

AR_FN void stage_shade_light(const KernelGlobals& kg, uint32_t item, const StepEntry& step) {
  const uint32_t slot = load_ro(kg.main_queue + step.start + item);
  const uint32_t light = instance_light_index(kg, ps_hit_inst(kg, slot));
  AR_ASSERT(light != kNoLightIndex && kg.lights != nullptr && kg.light_sampler != nullptr &&
            light < kg.light_sampler->light_count);
  const LightRecord& rec = kg.lights[light];
  const Real3 o = ps_ray_o(kg, slot), d = ps_ray_d(kg, slot);
  const LampCrossing hit = lamp_crossing(rec, o, d, ps_hit_t(kg, slot));
  const uint32_t flags = ps_flags(kg, slot);

  // NEE test modes (kernel_globals.h TEST_HOOK_NEE_*): off weights every hit 1; light_only drops a
  // hit that NEE could have sampled, i.e. one after a non-delta bounce.
  const uint32_t hooks = kg.session.test_hooks;
  const bool after_scatter = ps_bounces(kg, slot).v[0] > 0u && (flags & LAST_BOUNCE_SPECULAR) == 0u;
  const bool dropped = after_scatter && (hooks & TEST_HOOK_NEE_LIGHT_ONLY) != 0u;
  if (hit.valid && (flags & PATH_KIND) == 0u && dot(d, hit.n) < Real(0) && !dropped) {
    const uint32_t sample = ps_sample_id(kg, slot);
    const Spec lt = ps_lambda_t(kg, slot);
    Lambda lam;
    for (int i = 0; i < kLanes; ++i) lam.nm[i] = lt.v[i];
    const bool weighted = after_scatter && (hooks & TEST_HOOK_NEE_OFF) == 0u;
    Real p_light = Real(0);
    if (weighted) {
      const LightSampleContext prev = prev_light_context(kg, slot);
      p_light = light_sampler_pmf(*kg.light_sampler, prev, light) *
                lamp_pdf_li(rec, prev, normalize(d), LightHit{hit.p, hit.n});
    }
    const Spec contribution = lamp_radiance(rec, lam, kg.tables->observer) * ps_beta(kg, slot) *
                              mis_emitter_scale(ps_r_u(kg, slot), weighted ? ps_r_l(kg, slot) : Spec{}, p_light, weighted);
    if (!is_finite(contribution)) {
      sample_mark_invalid(kg, sample);
      path_end(kg, slot, Term::INVALID);
      return;
    }
    sample_add(kg, sample, contribution);
  }

  if (!hit.valid) {  // a lamp instance whose record is no lamp shape: a broken table, fail loudly
    if ((flags & PATH_KIND) == 0u) {
      sample_mark_invalid(kg, ps_sample_id(kg, slot));
      path_end(kg, slot, Term::INVALID);
    } else {  // a light path has no sample slot to mark
      ps_term(kg, slot) = static_cast<uint8_t>(Term::INVALID);
      ps_next_stage(kg, slot) = static_cast<uint8_t>(Stage::TERMINATED);
    }
    return;
  }

  const Real3 o_next = offset_past_surface(hit.p, hit.n, d);
  ps_prev_dist(kg, slot) = ps_prev_dist(kg, slot) + dot(o_next - o, d) / dot(d, d);
  ps_ray_o(kg, slot) = o_next;
  ps_next_stage(kg, slot) = static_cast<uint8_t>(Stage::INTERSECT_CLOSEST);
}

AR_NS_END

#if !AR_CUDA_BUILD
// CPU chunk function, defined by AR_CPU_STAGE in kernel_cpu/shade_light.cpp; registered by
// make_cpu_session_device.
void cpu_stage_shade_light(const ar::KernelGlobals& kg, const ar::StepEntry& step, uint32_t first, uint32_t last);
#endif
