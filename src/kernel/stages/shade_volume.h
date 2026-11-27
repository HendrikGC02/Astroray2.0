// SPDX-License-Identifier: Apache-2.0
// shade_volume: the stage for a segment that ran inside a medium (route_hit sends a hit here when the
// path's medium-stack depth is not 0). It delta-tracks the segment from the ray origin to the hit
// (medium/delta_track.h) over the medium of the stack's highest entry (stack_highest_id, whose id is
// 1 + the medium index in M1b, RULINGS 98; M1b tracks that one medium, summing the active media is
// M2's, 0006 D17), which is homogeneous until grids
// arrive: its coefficients are evaluated once, at the segment start, and the majorant is sigma_t
// itself (one majorant cell, 0006 D8 "homogeneous media run the same loop with one cell").
// Outcomes:
//   reached:   beta, r_u, r_l rescaled; the interrupted hit goes on to the stage route_hit gives
//              without the medium test (route_hit_after_medium), the hit record unchanged;
//   absorbed:  the path ends ABSORBED; a zero weight ends it ZERO_THROUGHPUT;
//   limit:     the event's 1,048,576 tentative collisions (D8, shared by its flights) end it
//              VOLUME_LIMIT, counted in the statistics' termination histogram;
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
// Random numbers: the path medium stream (0003 d23 stream 1). Event index e owns its blocks
// [e * 2^20, (e + 1) * 2^20) (flight_rng_start, 2^20 D8's limit). Several flights can run between
// two vertices, separated by a false interface or a lamp crossing, which advance neither the event
// index nor the camera dimensions; each continues the counter the previous one stored, so no two
// flights of a path share a block, as Cycles advances its rng offset at every volume-bounds crossing
// (path_state_volume_next: "Random number generator next bounce. rng_offset += PRNG_BOUNCE_NUM";
// here only the medium stream advances). A stored counter outside e's range is an earlier event's or
// a reused pool slot's and is not read: the flight starts at e * 2^20. A pool slot's stale counter
// could lie in e's range only before the path's first flight, and every path enters a medium through
// an opacity pass-through, which sets the counter to its new event's start (shade_surface.h). The
// flights of one event share D8's 2^20 tentative collisions; the stage stores the counter it ends with.
// The event and distance are decided at the hero lane (flags bits 7-8, 0003 d6).
// Not built here, each with its own task: the emission-absorption marcher (a medium whose estimator
// is MARCH_EMISSION_ABSORPTION, sigma_s = 0, is delta-tracked too, which is unbiased), volume NEE,
// medium emission and light-path items. A medium that emits, has a second phase lobe, fails its
// volume entry or yields a non-finite coefficient fails the sample loudly (INVALID) instead of
// rendering without that term. Light paths (no stage creates them yet) are forwarded untouched, so a
// light path passes a medium unattenuated: a known gap that the light-path task closes (0006 D11).
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
#include "../medium/medium_stack.h"
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

AR_FN void volume_fail(const KernelGlobals& kg, uint32_t slot, uint32_t sample) {
  sample_mark_invalid(kg, sample);
  path_end(kg, slot, Term::INVALID);
}

AR_FN void stage_shade_volume(const KernelGlobals& kg, uint32_t item, const StepEntry& step) {
  const uint32_t slot = load_ro(kg.main_queue + step.start + item);
  const uint32_t inst = ps_hit_inst(kg, slot);
  if ((ps_flags(kg, slot) & PATH_KIND) != 0u) {
    const Real2 uv = ps_hit_uv(kg, slot);
    const HitRecord hit{ps_hit_t(kg, slot), ps_hit_prim(kg, slot), inst, uv.x, uv.y};
    ps_next_stage(kg, slot) = static_cast<uint8_t>(route_hit_after_medium(kg, hit, ps_flags(kg, slot)));
    return;
  }
  const SceneData& s = scene_of(kg);
  // route_hit sends only paths with a non-empty stack here, which exists only with the media feature.
  const uint32_t id = stack_highest_id(ps_medium_stack(kg, slot), ps_flags(kg, slot));
  AR_ASSERT(id != 0u && id <= s.medium_count);
  const uint32_t medium = id - 1u;
  const MediumDesc& desc = s.medium_descs[medium];
  bool ok = (load_ro(&desc.flags) & MEDIUM_HAS_EMISSION) == 0u;
  // The coefficients first, with as little path state live as possible: the volume entry inlines the
  // interpreter, whose register pressure is the stage's largest (0002 D10 ladder step 2).
  Spec sigma_a{}, sigma_s{};
  PhaseLobe lobe{};
  {
    if (ok) {
      Lambda lambda;
      const Spec lt = ps_lambda_t(kg, slot);
      for (int i = 0; i < kLanes; ++i) lambda.nm[i] = lt.v[i];
      MediumSample ms;
      VmRegs regs;
      const MediumTables mt{s.medium_programs, s.medium_blocks, *kg.tables};
      ok = medium_eval(mt, desc, ps_ray_o(kg, slot), lambda, &regs, &ms) == VmStatus::OK && !(max_lane(ms.phase[1].weight) > Real(0));
      sigma_a = ms.sigma_a, sigma_s = ms.sigma_s, lobe = ms.phase[0];
      // A NaN or infinite coefficient would read as a zero majorant or a NaN event probability.
      ok = ok && is_finite(sigma_a) && is_finite(sigma_s);
    }
    if (!ok) {
      volume_fail(kg, slot, ps_sample_id(kg, slot));
      return;
    }
  }
  const uint32_t sample = ps_sample_id(kg, slot);
  const uint32_t flags = ps_flags(kg, slot);
  const Real3 d = ps_ray_d(kg, slot);
  // The flight runs in the ray's parameter, the unit hit_t and prev_dist are stored in, so its state
  // is stored without a conversion: coefficients per metre times metres per unit parameter (ray_d
  // need not be unit length).
  const Real d_len = length(d);
  sigma_a *= d_len;
  sigma_s *= d_len;
  TrackRay ray{ps_ray_o(kg, slot), d, ps_hit_t(kg, slot), ps_prev_dist(kg, slot)};
  const uint32_t e = ps_event_index(kg, slot);
  const SamplerKey key = camera_key(kg.active_pixels, sample, kg.session.samples_done, kg.session.samples_per_pass);
  const MediumStream st{kg.session.sampler_mode, key.id, key.index, kg.session.seed, STREAM_PATH_MEDIUM};
  const uint32_t first = flight_rng_start(e), end = flight_rng_start(e + 1u);
  const uint32_t stored = kg.session.features.media ? ps_medium_rng(kg, slot) : 0u;
  // A stored counter equal to `end` (the event's blocks used up) reads as out of range and restarts;
  // the flight that used them up ended its path VOLUME_LIMIT, so no later flight reads it. Once grid
  // media requeue flights (M2) it must be treated as exhausted instead.
  uint32_t counter = stored >= first && stored < end ? stored : first;
  TrackWeights w{ps_beta(kg, slot), ps_r_u(kg, slot), ps_r_l(kg, slot)};
  const TrackResult r = track_segment(sigma_a, sigma_s, sigma_a + sigma_s, hero_lane(flags), st, &counter, end - counter,
                                      0xffffffffu, &ray, &w);
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
    // The ray rebased at the flight's null collisions: o + d hit_t is still the hit, and
    // ray_o - prev_dist ray_d still the previous vertex (shade_light reads both).
    ps_ray_o(kg, slot) = ray.o;
    ps_hit_t(kg, slot) = ray.t_max;
    ps_prev_dist(kg, slot) = ray.back;
    const Real2 uv = ps_hit_uv(kg, slot);
    const HitRecord hit{ray.t_max, ps_hit_prim(kg, slot), inst, uv.x, uv.y};
    ps_next_stage(kg, slot) = static_cast<uint8_t>(route_hit_after_medium(kg, hit, flags));
    return;
  }
  AR_ASSERT(r.event == TrackEvent::SCATTERED);
  const Real3 wo = -(d / d_len);
  const Real2 u2{camera_path_draw(kg, sample, event_dim(e, EV_BSDF_U)), camera_path_draw(kg, sample, event_dim(e, EV_BSDF_V))};
  const PhaseSample ps = phase_sample(lobe, wo, u2);
  if (!(ps.pdf > Real(0))) {
    path_end(kg, slot, Term::ZERO_THROUGHPUT);
    return;
  }
  const Spec beta = w.beta * (ps.p / ps.pdf);
  if (!is_finite(beta)) {
    volume_fail(kg, slot, sample);
    return;
  }
  // pbrt's "if (depth++ >= maxDepth) terminated" and 0006 D14's volume_bounces, checked once the
  // vertex is known to be valid; nothing of the path is written before it.
  U16x6 b = ps_bounces(kg, slot);
  if (b.v[0] >= kg.session.max_bounces[0] || b.v[4] >= kg.session.max_bounces[4]) {
    path_end(kg, slot, Term::MAX_BOUNCE);
    return;
  }
  ps_beta(kg, slot) = beta;
  ps_r_u(kg, slot) = w.r_u;
  ps_r_l(kg, slot) = spec_fill(Real(0));
  // A real scatter before the denoising surface ends the guides of the path with nothing written
  // (0008 d15, PROVISIONAL): the bit stops every later surface and the escape from writing.
  const uint32_t done = kg.film->guide_albedo >= 0 ? DENOISE_SURFACE_DONE : 0u;
  ps_flags(kg, slot) = (flags & ~LAST_BOUNCE_SPECULAR) | done;
  b.v[0] = uint16_t(b.v[0] + 1u);
  b.v[4] = uint16_t(b.v[4] + 1u);
  ps_bounces(kg, slot) = b;
  ps_event_index(kg, slot) = uint16_t(e + 1u);
  // A medium vertex has no normal; the stored one only has to decode to a unit vector, since r_l = 0
  // removes the light pdf it would enter.
  ps_prev_ng(kg, slot) = oct_encode(ps.wi);
  ps_prev_ns(kg, slot) = oct_encode(ps.wi);
  ps_prev_dist(kg, slot) = Real(0);
  ps_ray_o(kg, slot) = ray.o;
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
