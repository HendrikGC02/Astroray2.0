// SPDX-License-Identifier: Apache-2.0
// shade_volume: the stage for a segment that ran inside a medium (route_hit sends the back-face hit
// of a medium-bearing instance here). It delta-tracks the segment from the ray origin to the hit
// (medium/delta_track.h) over the medium of the hit's instance, which is homogeneous until grids
// arrive: its coefficients are evaluated once, at the segment start, and the majorant is sigma_t
// itself (one majorant cell, 0006 D8 "homogeneous media run the same loop with one cell").
// Outcomes:
//   reached:   beta, r_u, r_l rescaled; the interrupted hit goes on to the stage route_hit gives
//              without the medium test (route_hit_after_medium), the hit record unchanged;
//   absorbed:  the path ends ABSORBED; a zero weight ends it ZERO_THROUGHPUT;
//   limit:     the flight's 1,048,576 tentative collisions (D8) end it VOLUME_LIMIT, counted in the
//              statistics' termination histogram;
//   scattered: a real scattering vertex, as pbrt-v4 VolPathIntegrator::Li's real-scattering branch:
//              "if (depth++ >= maxDepth) terminated", then a phase sample, beta *= p / pdf (1 for
//              HG, sampled exactly, D12), the ray leaves from the collision point. This stage has no
//              volume NEE yet, so no light strategy exists at the vertex: r_l = 0, which makes the next
//              emitter hit weigh beta Le / avg(r_u + 0 p_l) = beta Le / avg(r_u), pbrt's no-MIS branch
//              ("if (depth == 0 || specularBounce) L += beta * Le / r_u.Average()"). pbrt sets
//              r_l = r_u / ps->pdf there only because its SampleLd ran at the vertex.
// Bounces: the total limit as pbrt's depth check, and Blender's volume_bounces (0006 D14) on
// bounces[4]: scatter event k continues only while k <= volume_bounces (terminate-after); no
// Russian roulette after a medium scatter, as in pbrt (a scattered ray skips the loop's tail). The
// continuation's ray type is AR_VISIBILITY_VOLUME. A scatter advances the event index (0003 d23:
// real volume scatters are vertices); null collisions do not.
// Random numbers: the path medium stream (0003 d23 stream 1). Its block counter medium_rng starts
// each flight that enters from route_hit at e * 2^20 (e the event index, 2^20 D8's per-flight
// limit), so flights of one path never share a block and a reused pool slot's stale counter is never
// read; the stage stores the counter it ends with, from which a requeued flight would continue.
// The event and distance are decided at the hero lane (flags bits 7-8, 0003 d6).
// Not built here, each with its own task: the emission-absorption marcher (a medium whose estimator
// is MARCH_EMISSION_ABSORPTION, sigma_s = 0, is delta-tracked too, which is unbiased), volume NEE,
// medium emission and light-path items. A medium that emits, or has a second phase lobe, fails
// the sample loudly instead of rendering without that term. Light paths (no stage creates them yet)
// are forwarded untouched, as before tracking existed.
// Ref: docs/decisions/0006-volumes-and-astrophysics.md D8, D9, D12, D14; 0003 d6, d7, d23; 0004 D1
// (r_u / r_l form); 0002 D8, D11; pbrt-v4 src/pbrt/cpu/integrators.cpp VolPathIntegrator::Li
// (Apache-2.0) in C:\Astroray2.0\third_party\_cache\refs\pbrt-v4-092b23a47ad4.
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../core/spectrum.h"
#include "../core/vec.h"
#include "../kernel_globals.h"
#include "../medium/delta_track.h"
#include "../medium/medium.h"
#include "../medium/medium_block.h"
#include "../medium/medium_eval.h"
#include "../medium/phase.h"
#include "../portability.h"
#include "../sample/dims.h"
#include "../sample/keys.h"
#include "../scene/scene_view.h"
#include "../sched/step_table.h"
#include "../state/flags.h"
#include "../state/stage.h"
#include "../vm/vm_regs.h"
#include "route_hit.h"
#include "shade_common.h"

AR_NS_BEGIN

// ray_type of a continuation from a volume scatter: AR_VISIBILITY_VOLUME of
// include/astroray/astroray.h (pinned by tests/cpu/test_shade_volume.cpp).
inline constexpr uint8_t kRayTypeVolume = 0x20;

// The first medium_rng block of a flight that starts at event index e.
AR_FN uint32_t flight_rng_start(uint32_t e) { return e * kFlightCollisionLimit; }

AR_FN void volume_fail(const KernelGlobals& kg, uint32_t slot, uint32_t sample) {
  sample_mark_invalid(kg, sample);
  path_end(kg, slot, Term::INVALID);
}

AR_FN void stage_shade_volume(const KernelGlobals& kg, uint32_t item, const StepEntry& step) {
  const uint32_t slot = load_ro(kg.main_queue + step.start + item);
  const uint32_t inst = ps_hit_inst(kg, slot);
  const uint32_t medium = instance_medium(kg, inst);
  AR_ASSERT(medium != kNoMedium);  // route_hit sends only medium-bearing instances here
  const Real2 uv = ps_hit_uv(kg, slot);
  const HitRecord hit{ps_hit_t(kg, slot), ps_hit_prim(kg, slot), inst, uv.x, uv.y};
  const uint32_t flags = ps_flags(kg, slot);
  if ((flags & PATH_KIND) != 0u) {
    ps_next_stage(kg, slot) = static_cast<uint8_t>(route_hit_after_medium(kg, hit, flags));
    return;
  }
  const uint32_t sample = ps_sample_id(kg, slot);
  const SceneData& s = scene_of(kg);
  const MediumDesc& desc = s.medium_descs[medium];
  if ((load_ro(&desc.flags) & MEDIUM_HAS_EMISSION) != 0u) {
    volume_fail(kg, slot, sample);
    return;
  }
  const Real3 o = ps_ray_o(kg, slot), d = ps_ray_d(kg, slot);
  Lambda lambda;
  {
    const Spec lt = ps_lambda_t(kg, slot);
    for (int i = 0; i < kLanes; ++i) lambda.nm[i] = lt.v[i];
  }
  MediumSample ms;
  {
    VmRegs regs;
    const MediumTables mt{s.medium_programs, s.medium_blocks, *kg.tables};
    if (medium_eval(mt, desc, o, lambda, &regs, &ms) != VmStatus::OK || max_lane(ms.phase[1].weight) > Real(0)) {
      volume_fail(kg, slot, sample);
      return;
    }
  }
  // Distances in the coefficients' unit (metres): ray_d need not be unit length.
  const Real d_len = length(d);
  const Real t_max = ps_hit_t(kg, slot) * d_len;
  const uint32_t e = ps_event_index(kg, slot);
  const SamplerKey key = camera_key(kg.active_pixels, sample, kg.session.samples_done, kg.session.samples_per_pass);
  const MediumStream st{kg.session.sampler_mode, key.id, key.index, kg.session.seed, STREAM_PATH_MEDIUM};
  uint32_t counter = flight_rng_start(e);
  TrackWeights w{ps_beta(kg, slot), ps_r_u(kg, slot), ps_r_l(kg, slot)};
  const TrackResult r = track_segment(ms.sigma_a, ms.sigma_s, ms.sigma_a + ms.sigma_s, t_max, hero_lane(flags), st,
                                      &counter, kFlightCollisionLimit, &w);
  if (kg.session.features.media) ps_medium_rng(kg, slot) = counter;
  if (r.event == TrackEvent::ABSORBED) {
    path_end(kg, slot, Term::ABSORBED);
    return;
  }
  if (r.event == TrackEvent::LIMIT) {
    path_end(kg, slot, Term::VOLUME_LIMIT);
    return;
  }
  if (r.event == TrackEvent::ZERO) {
    path_end(kg, slot, Term::ZERO_THROUGHPUT);
    return;
  }
  if (!is_finite(w.beta) || !is_finite(w.r_u) || !is_finite(w.r_l)) {
    volume_fail(kg, slot, sample);
    return;
  }
  if (r.event == TrackEvent::REACHED) {
    ps_beta(kg, slot) = w.beta;
    ps_r_u(kg, slot) = w.r_u;
    ps_r_l(kg, slot) = w.r_l;
    ps_next_stage(kg, slot) = static_cast<uint8_t>(route_hit_after_medium(kg, hit, flags));
    return;
  }
  AR_ASSERT(r.event == TrackEvent::SCATTERED);
  U16x6 b = ps_bounces(kg, slot);
  if (b.v[0] >= kg.session.max_bounces[0] || b.v[4] >= kg.session.max_bounces[4]) {
    path_end(kg, slot, Term::MAX_BOUNCE);
    return;
  }
  const Real3 wo = -(d / d_len);
  const Real2 u2{camera_path_draw(kg, sample, event_dim(e, EV_BSDF_U)), camera_path_draw(kg, sample, event_dim(e, EV_BSDF_V))};
  const PhaseSample ps = phase_sample(ms.phase[0], wo, u2);
  if (!(ps.pdf > Real(0))) {
    path_end(kg, slot, Term::ZERO_THROUGHPUT);
    return;
  }
  const Spec beta = w.beta * (ps.p / ps.pdf);
  if (!is_finite(beta)) {
    volume_fail(kg, slot, sample);
    return;
  }
  ps_beta(kg, slot) = beta;
  ps_r_u(kg, slot) = w.r_u;
  ps_r_l(kg, slot) = spec_fill(Real(0));
  ps_flags(kg, slot) = flags & ~LAST_BOUNCE_SPECULAR;
  b.v[0] = uint16_t(b.v[0] + 1u);
  b.v[4] = uint16_t(b.v[4] + 1u);
  ps_bounces(kg, slot) = b;
  ps_event_index(kg, slot) = uint16_t(e + 1u);
  // A medium vertex has no normal; the stored one only has to decode to a unit vector, since r_l = 0
  // removes the light pdf it would enter.
  ps_prev_ng(kg, slot) = oct_encode(ps.wi);
  ps_prev_ns(kg, slot) = oct_encode(ps.wi);
  ps_prev_dist(kg, slot) = Real(0);
  ps_ray_o(kg, slot) = o + d * (r.t / d_len);
  ps_ray_d(kg, slot) = ps.wi;
  ps_ray_type(kg, slot) = kRayTypeVolume;
  ps_next_stage(kg, slot) = static_cast<uint8_t>(Stage::INTERSECT_CLOSEST);
}

AR_NS_END

#if !AR_CUDA_BUILD
// CPU chunk function, defined by AR_CPU_STAGE in kernel_cpu/shade_volume.cpp; registered by
// make_cpu_session_device.
void cpu_stage_shade_volume(const ar::KernelGlobals& kg, const ar::StepEntry& step, uint32_t first, uint32_t last);
#endif
