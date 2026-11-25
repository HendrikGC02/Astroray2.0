// SPDX-License-Identifier: Apache-2.0
// Null-scattering delta tracking of one flight over a homogeneous majorant segment, with the
// event chosen at the path's hero lane h and every lane's pdf carried in r_u and r_l (0006 D8,
// 0003 d6-d7). Pure: the coefficients, the majorant and the sampler stream are arguments, so the
// tests drive it without a scene.
//
// The flight, as pbrt-v4 SampleT_maj (media.h) and VolPathIntegrator::Li (cpu/integrators.cpp)
// with h in place of lane 0, D8's one change. The tentative-collision distance is
// t = t_min + SampleExponential(u, sigma_maj[h]) (pbrt: -log(1 - u) / a); past t_max the flight
// has reached the segment's end. At a collision T_maj = exp(-(t - t_min) sigma_maj) and the event
// is SampleDiscrete({pAbsorb, pScatter, pNull}, u') with pAbsorb = sigma_a[h] / sigma_maj[h],
// pScatter = sigma_s[h] / sigma_maj[h], pNull = max(0, 1 - pAbsorb - pScatter). pbrt's updates,
// quoted from the source with [0] read as [h]:
//   real scattering:  Float pdf = T_maj[0] * mp.sigma_s[0];
//                     beta *= T_maj * mp.sigma_s / pdf;
//                     r_u *= T_maj * mp.sigma_s / pdf;
//   null scattering:  SampledSpectrum sigma_n = ClampZero(sigma_maj - mp.sigma_a - mp.sigma_s);
//                     Float pdf = T_maj[0] * sigma_n[0];
//                     beta *= T_maj * sigma_n / pdf;
//                     if (pdf == 0) beta = SampledSpectrum(0.f);
//                     r_u *= T_maj * sigma_n / pdf;
//                     r_l *= T_maj * sigma_maj / pdf;
//                     return beta && r_u;
//   reached:          beta *= T_maj / T_maj[0];
//                     r_u *= T_maj / T_maj[0];
//                     r_l *= T_maj / T_maj[0];
// Absorption ends the path. A zero majorant at h takes no collision: T_maj = exp(-dt sigma_maj)
// over the whole segment and the reached update applies (pbrt's "zero-valued majorant" branch).
// Null collisions are not bounces and draw nothing from the fixed dimension map (0003 d23): each
// tentative collision draws one 4-D block of the variable-count stream `stream.stream`, whose block
// index is the counter *counter (the path's medium_rng): lane 0 the distance, lane 1 the event;
// lanes 2-3 (overlapping-medium choice, unused) are left for D18. After `limit` tentative collisions
// the flight stops with LIMIT, which the caller ends as VOLUME_LIMIT (D8: 1,048,576 per flight).
// Ref: Woodcock et al. 1965; Miller, Georgiev and Jarosz 2019 (null-scattering path integral);
// docs/decisions/0006-volumes-and-astrophysics.md D8; 0003 d6, d7, d23 (stream 1); pbrt-v4
// (Apache-2.0, read, not copied) in C:\Astroray2.0\third_party\_cache\refs\pbrt-v4-092b23a47ad4:
// src/pbrt/media.h SampleT_maj, src/pbrt/cpu/integrators.cpp VolPathIntegrator::Li (the
// SampleT_maj callback), src/pbrt/wavefront/media.cpp (the same updates on the GPU path),
// src/pbrt/util/sampling.h SampleExponential, SampleDiscrete.
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../core/spectrum.h"
#include "../portability.h"
#include "../sample/dims.h"
#include "../sample/sobol.h"

AR_NS_BEGIN

// D8's limit on tentative collisions per flight.
inline constexpr uint32_t kFlightCollisionLimit = 1u << 20;

// Where a flight's random numbers come from: one sampler key, mode and seed, and the stream id.
struct MediumStream {
  uint32_t mode, id, index, seed, stream;
};

// The path's spectral weights, which tracking rescales.
struct TrackWeights {
  Spec beta, r_u, r_l;
};

enum class TrackEvent : uint8_t {
  NULL_COLLISION,  // the flight goes on from the collision (track_collision only)
  REACHED,         // no real collision before t_max
  SCATTERED,       // a real scattering collision at t
  ABSORBED,        // a real absorption at t: the path ends
  ZERO,            // beta or r_u became zero: the path ends
  LIMIT,           // the flight's collision limit: the path ends VOLUME_LIMIT
};

struct TrackResult {
  TrackEvent event;
  Real t;               // where the flight stopped, in the segment's length unit
  uint32_t collisions;  // tentative collisions taken
};

// The reached update over [t_min, t_max]: beta, r_u, r_l *= T_maj / T_maj[h]. False when T_maj[h]
// is 0, which no reached flight can have unless the majorant transmittance underflowed.
AR_FN bool track_reach(const Spec& sigma_maj, Real dt, uint32_t h, TrackWeights* w) {
  Spec tm;
  for (int i = 0; i < kLanes; ++i) tm.v[i] = ar::exp(-dt * sigma_maj.v[i]);
  if (!(tm.v[h] > Real(0))) return false;
  const Spec s = tm / tm.v[h];
  w->beta *= s;
  w->r_u *= s;
  w->r_l *= s;
  return true;
}

// One tentative collision from *t (in: t_min) with uniforms u_dist, u_event. Out: *t is the
// collision's distance, or t_max when the flight reached the end. sigma_maj[h] must be > 0.
AR_FN TrackEvent track_collision(const Spec& sigma_a, const Spec& sigma_s, const Spec& sigma_maj, Real t_max, uint32_t h,
                                 Real u_dist, Real u_event, Real* t, TrackWeights* w) {
  const Real t_min = *t;
  const Real tc = t_min - ar::log(Real(1) - u_dist) / sigma_maj.v[h];
  if (!(tc < t_max)) {
    *t = t_max;
    return track_reach(sigma_maj, t_max - t_min, h, w) ? TrackEvent::REACHED : TrackEvent::ZERO;
  }
  *t = tc;
  Spec tm;
  for (int i = 0; i < kLanes; ++i) tm.v[i] = ar::exp(-(tc - t_min) * sigma_maj.v[i]);
  const Real p_absorb = sigma_a.v[h] / sigma_maj.v[h], p_scatter = sigma_s.v[h] / sigma_maj.v[h];
  const Real p_null = ar::max(Real(0), Real(1) - p_absorb - p_scatter);
  // SampleDiscrete: the first offset whose running sum passes u' * sum.
  const Real up = u_event * (p_absorb + p_scatter + p_null);
  if (up < p_absorb) return TrackEvent::ABSORBED;
  if (up < p_absorb + p_scatter) {
    const Real pdf = tm.v[h] * sigma_s.v[h];
    const Spec s = tm * sigma_s / pdf;
    w->beta *= s;
    w->r_u *= s;
    return TrackEvent::SCATTERED;
  }
  Spec sigma_n;
  for (int i = 0; i < kLanes; ++i) sigma_n.v[i] = ar::max(Real(0), sigma_maj.v[i] - sigma_a.v[i] - sigma_s.v[i]);
  const Real pdf = tm.v[h] * sigma_n.v[h];
  if (!(pdf > Real(0))) {
    w->beta = spec_fill(Real(0));
    return TrackEvent::ZERO;
  }
  w->beta *= tm * sigma_n / pdf;
  w->r_u *= tm * sigma_n / pdf;
  w->r_l *= tm * sigma_maj / pdf;
  return max_lane(w->beta) > Real(0) && max_lane(w->r_u) > Real(0) ? TrackEvent::NULL_COLLISION : TrackEvent::ZERO;
}

// One flight over [0, t_max] of a homogeneous medium (one majorant segment). Coefficients and
// t_max share one length unit; sigma_maj >= sigma_a + sigma_s at lane h is the caller's to keep.
AR_FN TrackResult track_segment(const Spec& sigma_a, const Spec& sigma_s, const Spec& sigma_maj, Real t_max, uint32_t h,
                                const MediumStream& st, uint32_t* counter, uint32_t limit, TrackWeights* w) {
  AR_ASSERT(h < uint32_t(kLanes));
  TrackResult r{TrackEvent::REACHED, t_max, 0u};
  if (!(sigma_maj.v[h] > Real(0))) {
    if (!track_reach(sigma_maj, t_max, h, w)) r.event = TrackEvent::ZERO;
    return r;
  }
  Real t = Real(0);
  for (;;) {
    if (r.collisions >= limit) {
      r.event = TrackEvent::LIMIT;
      r.t = t;
      return r;
    }
    const uint32_t block = (*counter)++;
    ++r.collisions;
    const Real u_dist = draw(st.mode, st.id, st.index, st.stream, stream_dim(block, 0u), st.seed);
    const Real u_event = draw(st.mode, st.id, st.index, st.stream, stream_dim(block, 1u), st.seed);
    const TrackEvent e = track_collision(sigma_a, sigma_s, sigma_maj, t_max, h, u_dist, u_event, &t, w);
    if (e != TrackEvent::NULL_COLLISION) {
      r.event = e;
      r.t = t;
      return r;
    }
  }
}

AR_NS_END
