// SPDX-License-Identifier: Apache-2.0
// ar-f64: compiled twice into this executable, once with AR_REAL_DOUBLE (cmake/sources.cmake).
// Ratio tracking of one shadow segment (medium/ratio_track.h; 0006 D8, D19) on its own, without a
// scene: the estimate's mean against Beer-Lambert per lane, the zero-coefficient segment, one
// collision against pbrt-v4 SampleLd's updates recomputed in double, the collision limit, and the
// per-launch cap (an estimate resumed after every cap stop is bit-identical to an uncapped one).
// Analytic value: transmittance exp(-sigma_t d) per lane. With a spectrally constant majorant the
// rescaling factor of r_l is 1 at every collision and at the end (T_maj sigma_maj / pdf and
// T_maj / T_maj[h] have equal lanes), so the light-sample weight avg(r_l) is 1 and T_ray alone is
// the estimate, unbiased per lane whatever the hero lane (Novak et al. 2014).
// Ref: docs/decisions/0006-volumes-and-astrophysics.md D8, D19; Novak, Selle and Jarosz 2014;
// pbrt-v4 src/pbrt/cpu/integrators.cpp VolPathIntegrator::SampleLd (Apache-2.0).
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>

#include "medium/ratio_track.h"

namespace {

using ar::Real;
using ar::Spec;

// sigma_t = 0.5, 1, 1.5, 2 per m, all absorption (0006 V1's lane spread).
const Spec kSigmaT{{Real(0.5), Real(1.0), Real(1.5), Real(2.0)}};

ar::MediumStream stream(uint32_t id, uint32_t index, uint32_t seed) {
  return ar::MediumStream{ar::SAMPLER_SOBOL, id, index, seed, ar::STREAM_SHADOW_MEDIUM};
}

constexpr uint32_t kUncapped = 0xffffffffu;

// The uncapped estimate over a fresh segment [0, t_max].
ar::RatioTrack ratio(const Spec& sa, const Spec& ss, const Spec& maj, Real t_max, uint32_t h, const ar::MediumStream& st,
                     uint32_t* counter, uint32_t limit) {
  ar::RatioTrack r = ar::ratio_track_start();
  ar::ratio_track_segment(sa, ss, maj, &t_max, h, st, counter, limit, kUncapped, &r);
  return r;
}

// An estimate driven to its end through cap stops: each capped return is a requeue, the next call going
// on from the stored state, length left and counter with the blocks left of `limit` from `first`.
// calls is 0xffffffff when a call advanced the counter by fewer blocks than its collisions or a capped
// call took other than `cap` collisions.
struct CappedRun {
  uint32_t calls, collisions;
};

CappedRun run_capped(const Spec& sa, const Spec& ss, const Spec& maj, uint32_t h, const ar::MediumStream& st, uint32_t first,
                     uint32_t limit, uint32_t cap, uint32_t* counter, Real* t_max, ar::RatioTrack* r) {
  CappedRun c{0u, 0u};
  *r = ar::ratio_track_start();
  do {
    const uint32_t before = *counter;
    ar::ratio_track_segment(sa, ss, maj, t_max, h, st, counter, limit - (*counter - first), cap, r);
    c.calls = c.calls == 0xffffffffu ? c.calls : c.calls + 1u;
    c.collisions += r->collisions;
    if (*counter - before < r->collisions || (r->capped && r->collisions != cap)) c.calls = 0xffffffffu;
  } while (r->capped);
  return c;
}

bool same_bits(const void* a, const void* b, size_t n) { return std::memcmp(a, b, n) == 0; }

}  // namespace

TEST_SUITE("fast") {

AR_RT_CASE("ratio_track/mean equals beer lambert") {
  // 2^16 segments of d = 1 per majorant, Sobol keys (2^10 x 64 samples), a fixed seed and the hero
  // lane cycling over the four lanes; each lane's mean T_ray within 3 standard errors of exp(-sigma_t).
  // The loose majorant takes more tentative collisions for the same mean.
  const Spec zero = ar::spec_fill(0);
  constexpr uint32_t kN = 1u << 16;
  double collisions[2] = {0, 0};
  for (int m = 0; m < 2; ++m) {
    const Spec maj = ar::spec_fill(Real(m == 0 ? 2 : 8));
    double sum[4] = {0, 0, 0, 0}, sum2[4] = {0, 0, 0, 0};
    int bad_r_l = 0;
    for (uint32_t i = 0; i < kN; ++i) {
      uint32_t counter = ar::shadow_rng_start(3u);
      const ar::RatioTrack r =
          ratio(kSigmaT, zero, maj, Real(1), i & 3u, stream(i >> 6, i & 63u, 0x5eedu + uint32_t(m)), &counter,
                                  ar::kShadowCollisionLimit);
      collisions[m] += r.collisions;
      bad_r_l += std::fabs(double(ar::avg(r.r_l)) - 1.0) > 1e-6;
      for (int j = 0; j < 4; ++j) sum[j] += double(r.T.v[j]), sum2[j] += double(r.T.v[j]) * double(r.T.v[j]);
    }
    CHECK(bad_r_l == 0);
    for (int j = 0; j < 4; ++j) {
      const double mean = sum[j] / kN, se = std::sqrt((sum2[j] / kN - mean * mean) / (kN - 1));
      const double truth = std::exp(-double(kSigmaT.v[j]));
      std::printf("ratio_track majorant %d lane %d: mean %.6f truth %.6f se %.2e z %+.2f\n", m == 0 ? 2 : 8, j, mean, truth, se,
                  (mean - truth) / se);
      INFO("majorant " << (m == 0 ? 2 : 8) << " lane " << j);
      CHECK(se > 0.0);
      CHECK(std::fabs(mean - truth) < 3.0 * se);
    }
  }
  CHECK(collisions[1] > 2.0 * collisions[0]);
}

AR_RT_CASE("ratio_track/roulette keeps beer lambert") {
  // A long segment (d = 8 m) under majorant 8: about 64 tentative collisions, so T_ray falls below
  // 0.05 avg(r_l + r_u) at every lane and pbrt's roulette runs (q = 0.75, survivors / (1 - q)). Over
  // 2^14 segments each lane's mean stays within 3 standard errors of exp(-sigma_t d), and the
  // roulette's blocks (blocks drawn beyond the tentative collisions) are a large share. 2^14 segments keep the
  // f64 copy inside B-12's per-test time.
  const Spec zero = ar::spec_fill(0);
  const Spec maj = ar::spec_fill(Real(8));
  constexpr uint32_t kN = 1u << 14;
  double sum[4] = {0, 0, 0, 0}, sum2[4] = {0, 0, 0, 0}, roulette_blocks = 0.0;
  for (uint32_t i = 0; i < kN; ++i) {
    const uint32_t start = ar::shadow_rng_start(2u);
    uint32_t counter = start;
    const ar::RatioTrack r =
        ratio(kSigmaT, zero, maj, Real(8), i & 3u, stream(i >> 6, i & 63u, 0x0de7u), &counter, ar::kShadowCollisionLimit);
    roulette_blocks += double(counter - start) - double(r.collisions);
    for (int j = 0; j < 4; ++j) sum[j] += double(r.T.v[j]), sum2[j] += double(r.T.v[j]) * double(r.T.v[j]);
  }
  std::printf("ratio_track roulette: %.0f roulette blocks over %u segments\n", roulette_blocks, kN);
  CHECK(roulette_blocks > 0.5 * kN);
  for (int j = 0; j < 4; ++j) {
    const double mean = sum[j] / kN, se = std::sqrt((sum2[j] / kN - mean * mean) / (kN - 1));
    const double truth = std::exp(-8.0 * double(kSigmaT.v[j]));
    std::printf("ratio_track roulette lane %d: mean %.4e truth %.4e se %.2e z %+.2f\n", j, mean, truth, se, (mean - truth) / se);
    INFO("lane " << j);
    CHECK(se > 0.0);
    CHECK(std::fabs(mean - truth) < 3.0 * se);
  }
}

AR_RT_CASE("ratio_track/zero sigma is one") {
  // No absorption or scattering and a zero majorant: no collision, no draw, T_ray and both factors
  // exactly 1 (pbrt's zero-valued majorant branch), over a finite and over an unbounded segment.
  const Spec zero = ar::spec_fill(0);
  for (const Real t_max : {Real(3), Real(0x1.fffffep+127f)}) {
    uint32_t counter = 77u;
    const ar::RatioTrack r = ratio(zero, zero, zero, t_max, 2u, stream(5u, 1u, 9u), &counter, ar::kShadowCollisionLimit);
    CHECK(counter == 77u);
    CHECK(r.collisions == 0u);
    CHECK_FALSE(r.limit);
    for (int j = 0; j < 4; ++j) {
      CHECK(r.T.v[j] == Real(1));
      CHECK(r.r_u.v[j] == Real(1));
      CHECK(r.r_l.v[j] == Real(1));
    }
  }
}

AR_RT_CASE("ratio_track/one collision follows pbrt") {
  // pbrt-v4 SampleLd's ratio-tracking update and final segment with h for lane 0, recomputed in
  // double from the stream's own uniforms. The majorant differs per lane and the coefficients mix
  // absorption and scattering, so a factor on the wrong spectrum or lane shows. The key is searched
  // for one whose first distance falls inside [0, 1] and whose second passes its end: one collision,
  // then the final update; the roulette does not run (Tr stays above 0.05 at these coefficients).
  const Spec sa{{Real(0.1), Real(0.2), Real(0.3), Real(0.4)}}, ss{{Real(0.05), Real(0.1), Real(0.15), Real(0.2)}};
  const Spec maj{{Real(1.0), Real(1.25), Real(1.5), Real(1.75)}};
  const uint32_t h = 1;
  const double tol = sizeof(Real) == 4 ? 2e-5 : 1e-12;
  int found = 0;
  for (uint32_t id = 0; id < 4096 && found == 0; ++id) {
    const ar::MediumStream st = stream(id, 0u, 21u);
    const double u0 = double(ar::draw(st.mode, st.id, st.index, st.stream, ar::stream_dim(10u, 0u), st.seed));
    const double u1 = double(ar::draw(st.mode, st.id, st.index, st.stream, ar::stream_dim(11u, 0u), st.seed));
    const double mh = double(maj.v[h]);
    const double t1 = -std::log(1.0 - u0) / mh, t2 = t1 - std::log(1.0 - u1) / mh;
    if (!(t1 < 0.99 && t2 > 1.01)) continue;
    ++found;
    uint32_t counter = 10u;
    const ar::RatioTrack r = ratio(sa, ss, maj, Real(1), h, st, &counter, ar::kShadowCollisionLimit);
    CHECK(r.collisions == 2u);  // the second draw is the one that passes the end
    CHECK(counter == 12u);
    double T[4], ru[4], rl[4], tm[4], te[4];
    for (int j = 0; j < 4; ++j) {
      tm[j] = std::exp(-t1 * double(maj.v[j]));
      te[j] = std::exp(-(1.0 - t1) * double(maj.v[j]));
    }
    const double pdf = tm[h] * mh;
    for (int j = 0; j < 4; ++j) {
      const double sn = double(maj.v[j]) - double(sa.v[j]) - double(ss.v[j]);
      T[j] = tm[j] * sn / pdf * te[j] / te[h];
      ru[j] = tm[j] * sn / pdf * te[j] / te[h];
      rl[j] = tm[j] * double(maj.v[j]) / pdf * te[j] / te[h];
    }
    for (int j = 0; j < 4; ++j) {
      INFO("lane " << j);
      CHECK(std::fabs(double(r.T.v[j]) - T[j]) <= tol * T[j]);
      CHECK(std::fabs(double(r.r_u.v[j]) - ru[j]) <= tol * ru[j]);
      CHECK(std::fabs(double(r.r_l.v[j]) - rl[j]) <= tol * rl[j]);
    }
  }
  CHECK(found == 1);
}

AR_RT_CASE("ratio_track/collision limit blocks") {
  // Null collisions only (majorant 1e6 per m, no absorption or scattering, over 1 m): T_ray never
  // falls, so the estimate stops at the limit, advancing the counter by as many blocks, with T 0.
  const Spec zero = ar::spec_fill(0);
  uint32_t counter = 40u;
  const ar::RatioTrack r = ratio(zero, zero, ar::spec_fill(Real(1e6)), Real(1), 3u, stream(7u, 2u, 3u), &counter, 16u);
  CHECK(r.limit);
  CHECK(r.collisions == 16u);
  CHECK(counter == 56u);
  for (int j = 0; j < 4; ++j) CHECK(r.T.v[j] == Real(0));
}

AR_RT_CASE("ratio_track/capped equals uncapped") {
  // 0006 D8 per-launch cap, V6 third clause, 0003 check 26: 1e4 random shadow segments with a loose
  // majorant (2 to 20 times sigma_t) and lengths up to 4 m, so estimates take tens of collisions and
  // the roulette runs in many. Each is estimated uncapped and again with caps 1, 7 and 4,096, requeued
  // after every stop; T, r_u, r_l, the length left, the counter and the limit flag must equal the
  // uncapped estimate's bit for bit, and the calls must be ceil(collisions / cap).
  std::mt19937 rng(0x193u);
  std::uniform_real_distribution<double> U(0.0, 1.0);
  const uint32_t caps[3] = {1u, 7u, 4096u};
  int differ = 0, bad_calls = 0, capped_runs = 0, roulette = 0;
  for (int i = 0; i < 10000; ++i) {
    Spec sa, ss, maj;
    const double loose = 2.0 + 18.0 * U(rng);
    for (int j = 0; j < 4; ++j) {
      sa.v[j] = Real(0.05 + 1.0 * U(rng));
      ss.v[j] = Real(0.05 + 1.0 * U(rng));
      maj.v[j] = Real(double(sa.v[j] + ss.v[j]) * loose);
    }
    const uint32_t h = uint32_t(i) & 3u;
    const Real len = Real(0.1 + 4.0 * U(rng));
    const ar::MediumStream st = stream(uint32_t(i) >> 4, uint32_t(i) & 15u, 0x5eedu);
    const uint32_t first = ar::shadow_rng_start(uint32_t(i) & 7u);
    uint32_t c_ref = first;
    Real t_ref = len;
    ar::RatioTrack ref = ar::ratio_track_start();
    ar::ratio_track_segment(sa, ss, maj, &t_ref, h, st, &c_ref, ar::kShadowCollisionLimit, kUncapped, &ref);
    roulette += c_ref - first > ref.collisions;
    for (const uint32_t cap : caps) {
      uint32_t c = first;
      Real t = len;
      ar::RatioTrack r;
      const CappedRun run = run_capped(sa, ss, maj, h, st, first, ar::kShadowCollisionLimit, cap, &c, &t, &r);
      capped_runs += run.calls > 1u;
      bad_calls += run.calls != (ref.collisions + cap - 1u) / cap || run.collisions != ref.collisions;
      differ += c != c_ref || r.limit != ref.limit || !same_bits(&t, &t_ref, sizeof t) || !same_bits(&r.T, &ref.T, sizeof r.T) ||
                !same_bits(&r.r_u, &ref.r_u, sizeof r.r_u) || !same_bits(&r.r_l, &ref.r_l, sizeof r.r_l);
    }
  }
  std::printf("ratio_track capped: %d capped runs, %d estimates with roulette draws\n", capped_runs, roulette);
  CHECK(differ == 0);
  CHECK(bad_calls == 0);
  CHECK(capped_runs > 10000);
  CHECK(roulette > 1000);
}

AR_RT_CASE("ratio_track/limit after the same count whatever the cap") {
  // Null collisions only (majorant 1e6 per m over 1 m): T_ray never falls, so the segment's limit, 100
  // blocks from an offset start, blocks the item after exactly 100 tentative collisions uncapped or
  // capped at 1, 7, 100 or 4,096; at a cap equal to the blocks left the limit wins.
  const Spec zero = ar::spec_fill(0), maj = ar::spec_fill(Real(1e6));
  const ar::MediumStream st = stream(11u, 3u, 5u);
  const uint32_t first = ar::shadow_rng_start(2u) + 30u;
  for (const uint32_t cap : {1u, 7u, 100u, 4096u, kUncapped}) {
    uint32_t c = first;
    Real t = Real(1);
    ar::RatioTrack r;
    const CappedRun run = run_capped(zero, zero, maj, 1u, st, first, 100u, cap, &c, &t, &r);
    INFO("cap " << cap);
    CHECK(run.calls != 0xffffffffu);
    CHECK(r.limit);
    CHECK(run.collisions == 100u);
    CHECK(c == first + 100u);
    for (int j = 0; j < 4; ++j) CHECK(r.T.v[j] == Real(0));
  }
}

}  // TEST_SUITE
