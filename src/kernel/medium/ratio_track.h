// SPDX-License-Identifier: Apache-2.0
// Ratio tracking of one shadow segment over a homogeneous majorant segment: the per-lane
// transmittance estimate T_ray and the factors by which r_u and r_l of the light sample are rescaled
// (0006 D8: "shadow transmittance uses ratio tracking ... with the same rescaling"), with the
// tentative-collision distance drawn at the item's hero lane h, D8's one change to pbrt. Pure: the
// coefficients, the majorant and the sampler stream are arguments.
//
// pbrt-v4 VolPathIntegrator::SampleLd (cpu/integrators.cpp) with SampleT_maj (media.h), [0] read as
// [h]. T_ray, r_l and r_u start at 1. The distance is t = t_min + SampleExponential(u, sigma_maj[h])
// and at a collision T_maj = exp(-(t - t_min) sigma_maj); quoted from the source:
//   SampledSpectrum sigma_n = ClampZero(sigma_maj - mp.sigma_a - mp.sigma_s);
//   Float pdf = T_maj[0] * sigma_maj[0];
//   T_ray *= T_maj * sigma_n / pdf;
//   r_l *= T_maj * sigma_maj / pdf;
//   r_u *= T_maj * sigma_n / pdf;
//   SampledSpectrum Tr = T_ray / (r_l + r_u).Average();
//   if (Tr.MaxComponentValue() < 0.05f) {
//       Float q = 0.75f;
//       if (rng.Uniform<Float>() < q) T_ray = SampledSpectrum(0.);
//       else T_ray /= 1 - q;
//   }
//   if (!T_ray) return false;
// and past the segment's end ("Update transmittance estimate for final segment"):
//   T_ray *= T_maj / T_maj[0]; r_l *= T_maj / T_maj[0]; r_u *= T_maj / T_maj[0];
// A zero majorant at h takes no collision and draws nothing: T_maj = exp(-dt sigma_maj) over the
// whole segment, then the final update (SampleT_maj's "zero-valued majorant" branch; an infinite dt
// is the largest finite float there, so a zero lane gives exp(-0) = 1 and not exp(-inf * 0)).
// The caller then forms the light sample's weight as SampleLd does, r_l *= r_p p_l, r_u *= r_p
// p_scatter, contribution beta f_hat T_ray L / avg(r_l + r_u) (avg(r_l) for a delta light).
// Two departures that change no expectation: an estimate already 0 at every lane returns before the
// roulette (pbrt draws its uniform and gets 0 either way), and the roulette's uniform has a block of
// its own (below).
// Random numbers: every draw takes one 4-D block of the variable-count stream `st.stream` (0003
// d23, dims.h: one block per draw), its index the counter *counter (the item's medium_rng), and
// reads its lane 0: a tentative collision's distance, or the roulette's uniform when the roulette
// runs. Lane 0 only, because each Sobol lane a kernel reads is a 128 B direction table on the GPU
// stack (sample/sobol.h) and shade_shadow_medium is held to 0002 D10's 512 B VM stack. After `limit`
// blocks (D8: 131,072 tentative collisions per shadow segment, PROVISIONAL; roulette draws count
// too, so an item's blocks never leave its 2^17 stride) the estimate stops with `limit` set and
// T_ray = 0: the caller blocks the item and counts VOLUME_LIMIT.
// Ref: Novak, Selle and Jarosz 2014, "Residual ratio tracking for estimating attenuation in
// participating media"; docs/decisions/0006-volumes-and-astrophysics.md D8, D19; 0003 d6, d7, d23
// (stream 2); pbrt-v4 (Apache-2.0, read, not copied) in
// C:\Astroray2.0\third_party\_cache\refs\pbrt-v4-092b23a47ad4: src/pbrt/cpu/integrators.cpp
// VolPathIntegrator::SampleLd, src/pbrt/media.h SampleT_maj, src/pbrt/util/sampling.h
// SampleExponential.
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../core/spectrum.h"
#include "../portability.h"
#include "../sample/dims.h"
#include "../sample/sobol.h"
#include "../state/state.h"
#include "delta_track.h"

AR_NS_BEGIN

// D8's limit on tentative collisions per shadow segment; also the stride of a shadow item's
// medium_rng start (dims.h STREAM_SHADOW_MEDIUM: e * 2^17), so the items of two vertices of one path
// never share a block.
inline constexpr uint32_t kShadowCollisionLimit = 1u << 17;

// The first medium_rng block of the NEE item emitted at event index e.
AR_FN uint32_t shadow_rng_start(uint32_t e) { return e * kShadowCollisionLimit; }

// What an M1b shadow item's medium_stack holds until the medium stack lands (0006 D19): entry 0 is 1 +
// the index in SceneData::medium_descs of the one medium the whole segment lies in, entries 1-3 are 0;
// all zero for an item in no medium. The same 1 + index form as DeviceInstanceRecord::medium.
AR_FN U32x4 shadow_medium_field(uint32_t medium_plus_one) { return U32x4{{medium_plus_one, 0u, 0u, 0u}}; }

struct RatioTrack {
  Spec T, r_u, r_l;     // T_ray and the rescaling factors of r_u and r_l, each starting at 1
  uint32_t collisions;  // tentative collisions taken
  bool limit;           // stopped at `limit`: T is 0 and the caller counts VOLUME_LIMIT
};

// One shadow segment [0, t_max] of a homogeneous medium (one majorant segment). Coefficients and
// t_max share one length unit; sigma_maj >= sigma_a + sigma_s at every lane is the caller's to keep.
AR_FN RatioTrack ratio_track_segment(const Spec& sigma_a, const Spec& sigma_s, const Spec& sigma_maj, Real t_max, uint32_t h,
                                     const MediumStream& st, uint32_t* counter, uint32_t limit) {
  AR_ASSERT(h < uint32_t(kLanes));
  RatioTrack r{spec_fill(Real(1)), spec_fill(Real(1)), spec_fill(Real(1)), 0u, false};
  const Real maj_h = lane_at(sigma_maj, h);
  Real t = Real(0);
  if (maj_h > Real(0)) {
    Spec sigma_n;
    for (int i = 0; i < kLanes; ++i) sigma_n.v[i] = ar::max(Real(0), sigma_maj.v[i] - sigma_a.v[i] - sigma_s.v[i]);
    const uint32_t start = *counter;
    for (;;) {
      if (*counter - start >= limit) {
        r.T = spec_fill(Real(0));
        r.limit = true;
        return r;
      }
      const Real u = draw(st.mode, st.id, st.index, st.stream, stream_dim((*counter)++, 0u), st.seed);
      ++r.collisions;
      const Real tc = t - ar::log(Real(1) - u) / maj_h;
      if (!(tc < t_max)) break;
      Spec tm;
      for (int i = 0; i < kLanes; ++i) tm.v[i] = ar::exp(-(tc - t) * sigma_maj.v[i]);
      const Real pdf = lane_at(tm, h) * maj_h;
      r.T *= tm * sigma_n / pdf;
      r.r_l *= tm * sigma_maj / pdf;
      r.r_u *= tm * sigma_n / pdf;
      if (!(max_lane(r.T) > Real(0))) return r;
      if (max_lane(r.T / avg(r.r_l + r.r_u)) < Real(0.05)) {
        if (*counter - start >= limit) continue;  // no block left for the roulette: the limit ends it
        const Real q = Real(0.75);
        if (draw(st.mode, st.id, st.index, st.stream, stream_dim((*counter)++, 0u), st.seed) < q) {
          r.T = spec_fill(Real(0));
          return r;
        }
        r.T = r.T / (Real(1) - q);
      }
      t = tc;
    }
  }
  // The final segment from the last collision (or the start) to t_max.
  // pbrt replaces an infinite dt by the largest finite Float; the float one serves both builds.
  Real dt = t_max - t;
  if (!(dt <= Real(0x1.fffffep+127f))) dt = Real(0x1.fffffep+127f);
  Spec tm;
  for (int i = 0; i < kLanes; ++i) tm.v[i] = ar::exp(-dt * sigma_maj.v[i]);
  const Spec s = tm / lane_at(tm, h);
  r.T *= s;
  r.r_u *= s;
  r.r_l *= s;
  return r;
}

AR_NS_END
