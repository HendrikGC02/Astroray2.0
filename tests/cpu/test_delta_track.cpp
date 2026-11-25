// SPDX-License-Identifier: Apache-2.0
// ar-f64: compiled twice into this executable, once with AR_REAL_DOUBLE (cmake/sources.cmake).
// Hero-lane delta tracking (medium/delta_track.h; 0006 D8, 0003 d6-d7) on its own, without a scene:
// the free-flight distance against the hero lane's exponential, an exact enumeration of a flight's
// one-collision outcomes against the per-lane analytic transmittance and scattered weight, the
// collision limit, and (gate) the per-lane transmittance of absorbing slabs at 2^20 flights.
// Analytic values: transmittance exp(-sigma_t d); with the majorant equal to sigma_t a flight has
// at most one collision, scattered at depth t with density sigma_t e^(-sigma_t t), so the expected
// scattered weight per lane is sigma_s / sigma_t (1 - exp(-sigma_t d)).
// Ref: docs/decisions/0006-volumes-and-astrophysics.md D8, V1; 0003 d6 (one-sample balance heuristic
// over the hero lane: beta / avg(r_u)); Veach 1997 section 9.2.4.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "common/stat.h"
#include "medium/delta_track.h"

namespace {

namespace st = astroray::stat;
using ar::Real;
using ar::Spec;
using ar::TrackEvent;
using ar::TrackWeights;

// The coefficients every case uses: lanes with sigma_t = 0.5, 1, 1.5, 2 per m (0006 V1's lane spread).
const Spec kSigmaT{{Real(0.5), Real(1.0), Real(1.5), Real(2.0)}};

TrackWeights unit_weights() { return TrackWeights{ar::spec_fill(1), ar::spec_fill(1), ar::spec_fill(1)}; }

ar::MediumStream stream(uint32_t id, uint32_t mode, uint32_t seed) {
  return ar::MediumStream{mode, id, 0u, seed, ar::STREAM_PATH_MEDIUM};
}

// The per-lane estimate a flight leaves at the film: beta_j / avg(r_u) (0003 d6), 0 when it ended.
double lane_estimate(const TrackWeights& w, int j) { return double(w.beta.v[j]) / double(ar::avg(w.r_u)); }

}  // namespace

TEST_SUITE("fast") {

AR_RT_CASE("delta_track/free flight distribution") {
  // A pure absorber (majorant = sigma_a) ends at its first tentative collision, which therefore lies
  // at the free-flight distance. For each hero lane h, 1e5 flights of independent-mode draws, binned
  // into 20 cells of equal probability under the exponential of rate sigma_t[h].
  const st::Run run = st::begin("delta_track/free flight distribution" AR_RT_SUFFIX, st::gate_family());
  const Spec zero = ar::spec_fill(0);
  constexpr int kCells = 20, kN = 100000;
  for (uint32_t h = 0; h < 4u; ++h) {
    std::vector<double> obs(kCells, 0.0), exp_(kCells, double(kN) / kCells);
    const double rate = double(kSigmaT.v[h]);
    for (int i = 0; i < kN; ++i) {
      TrackWeights w = unit_weights();
      uint32_t counter = 0;
      const ar::TrackResult r = ar::track_segment(kSigmaT, zero, kSigmaT, Real(1e30), h,
                                                  stream(uint32_t(i), ar::SAMPLER_INDEPENDENT, uint32_t(run.seed) + h),
                                                  &counter, ar::kFlightCollisionLimit, &w);
      REQUIRE(r.event == TrackEvent::ABSORBED);
      REQUIRE(r.collisions == 1u);
      REQUIRE(counter == 1u);
      const double cdf = 1.0 - std::exp(-rate * double(r.t));
      obs[std::min(kCells - 1, int(cdf * kCells))] += 1.0;
    }
    const st::Chi2Result res = st::chi2_test(run, obs, exp_);
    std::printf("delta_track free flight hero %u: chi2 %.1f dof %d p %.4f\n", h, res.chi2, res.dof, res.p);
    CHECK_MESSAGE(res.pass, res.message);
  }
}

AR_RT_CASE("delta_track/weights sum to transmittance") {
  // Majorant = sigma_t: a flight over d = 1 either reaches the end or has one real collision. The
  // expectation over the hero lane (1/4 each) and both uniforms is enumerated: the distance uniform
  // on a 2^14-point midpoint grid, the event uniform on 20 midpoints. The coefficients make every
  // hero's absorb probability (0.2, 0.5, 0.4, 0.5) a multiple of 1/20, so the event grid splits
  // exactly and the only error is the distance grid's (below 2^-14 per lane times the weight).
  const Spec sigma_s{{Real(0.4), Real(0.5), Real(0.9), Real(1.0)}};
  const Spec sigma_a = kSigmaT - sigma_s;
  constexpr int kDist = 1 << 14, kEvent = 20;
  double reach[4] = {0, 0, 0, 0}, scatter[4] = {0, 0, 0, 0};
  for (uint32_t h = 0; h < 4u; ++h)
    for (int a = 0; a < kDist; ++a)
      for (int b = 0; b < kEvent; ++b) {
        TrackWeights w = unit_weights();
        Real t = 0;
        const TrackEvent e = ar::track_collision(sigma_a, sigma_s, kSigmaT, Real(1), h, Real((a + 0.5) / kDist),
                                                 Real((b + 0.5) / kEvent), &t, &w);
        REQUIRE(e != TrackEvent::NULL_COLLISION);  // the majorant leaves no null collision
        REQUIRE(e != TrackEvent::ZERO);
        for (int j = 0; j < 4; ++j) {
          const double v = lane_estimate(w, j) / (4.0 * kDist * kEvent);
          if (e == TrackEvent::REACHED) reach[j] += v;
          if (e == TrackEvent::SCATTERED) scatter[j] += v;
        }
      }
  for (int j = 0; j < 4; ++j) {
    const double st_ = double(kSigmaT.v[j]), tr = std::exp(-st_);
    const double sc = double(sigma_s.v[j]) / st_ * (1.0 - tr);
    INFO("lane " << j);
    CHECK(std::fabs(reach[j] - tr) < 3e-4);
    CHECK(std::fabs(scatter[j] - sc) < 3e-4);
  }
}

AR_RT_CASE("delta_track/collision limit and zero majorant") {
  // Null collisions only (no absorption or scattering, majorant 1e6 per m over 1 m): the flight
  // takes exactly `limit` tentative collisions, advancing the counter by as many blocks, and stops.
  const Spec zero = ar::spec_fill(0);
  TrackWeights w = unit_weights();
  uint32_t counter = 40;
  const ar::TrackResult r =
      ar::track_segment(zero, zero, ar::spec_fill(Real(1e6)), Real(1), 2u, stream(7u, ar::SAMPLER_SOBOL, 3u), &counter, 16u, &w);
  CHECK(r.event == TrackEvent::LIMIT);
  CHECK(r.collisions == 16u);
  CHECK(counter == 56u);
  // A zero majorant at the hero lane takes no collision and draws nothing; the other lanes carry
  // their transmittance through T / T[h] = T.
  Spec maj = kSigmaT;
  maj.v[1] = Real(0);
  TrackWeights z = unit_weights();
  uint32_t c2 = 5;
  const ar::TrackResult r2 = ar::track_segment(zero, zero, maj, Real(1), 1u, stream(7u, ar::SAMPLER_SOBOL, 3u), &c2,
                                               ar::kFlightCollisionLimit, &z);
  CHECK(r2.event == TrackEvent::REACHED);
  CHECK(r2.collisions == 0u);
  CHECK(c2 == 5u);
  for (int j = 0; j < 4; ++j) CHECK(double(z.beta.v[j]) == doctest::Approx(std::exp(-double(maj.v[j]))).epsilon(1e-6));
}

}  // TEST_SUITE

TEST_SUITE("gate") {

// Absorbing slab, d = 1, 2^20 flights in Sobol mode, the hero lane drawn per flight from dimension 6
// as a camera path's (0003 d23): per lane, the film estimate's mean against exp(-sigma_t d), one gate
// member per lane (Sidak). Run with the majorant equal to sigma_t and again at 1.5 sigma_t, where
// null collisions carry the non-hero lanes (pbrt's sigma_n update).
static void absorbing_slab(double majorant_scale, const char* name) {
  const st::Run run = st::begin(name, st::gate_family());
  const Spec zero = ar::spec_fill(0);
  const Spec maj = kSigmaT * Real(majorant_scale);
  constexpr uint32_t kN = 1u << 20;
  std::vector<double> samples[4];
  for (auto& s : samples) s.reserve(kN);
  for (uint32_t i = 0; i < kN; ++i) {
    const uint32_t id = i >> 6, index = i & 63u;  // 2^14 keys x 64 samples
    const uint32_t h = uint32_t(4.0 * double(ar::draw(ar::SAMPLER_SOBOL, id, index, ar::STREAM_FIXED, 6u, uint32_t(run.seed))));
    TrackWeights w = unit_weights();
    uint32_t counter = 3u * ar::kFlightCollisionLimit;
    const ar::MediumStream s{ar::SAMPLER_SOBOL, id, index, uint32_t(run.seed), ar::STREAM_PATH_MEDIUM};
    const ar::TrackResult r = ar::track_segment(kSigmaT, zero, maj, Real(1), h < 4u ? h : 3u, s, &counter,
                                                ar::kFlightCollisionLimit, &w);
    for (int j = 0; j < 4; ++j) samples[j].push_back(r.event == TrackEvent::REACHED ? lane_estimate(w, j) : 0.0);
  }
  for (int j = 0; j < 4; ++j) {
    const double truth = std::exp(-double(kSigmaT.v[j]));
    const st::EnergyResult e = st::energy_test(run, samples[j], truth);
    std::printf("%s lane %d: mean %.6f truth %.6f se %.2e z %+.2f\n", name, j, e.mean, truth, e.se, e.z);
    CHECK_MESSAGE(e.pass, e.message);
  }
}

AR_RT_CASE("delta_track/absorbing slab per lane") { absorbing_slab(1.0, "delta_track/absorbing slab per lane" AR_RT_SUFFIX); }
AR_RT_CASE("delta_track/absorbing slab null collisions") {
  absorbing_slab(1.5, "delta_track/absorbing slab null collisions" AR_RT_SUFFIX);
}

}  // TEST_SUITE
