// SPDX-License-Identifier: Apache-2.0
// ar-f64: compiled twice into this executable, once with AR_REAL_DOUBLE (cmake/sources.cmake).
// Packet operations of 0003 d5 and the flag word of d21. Ref: 0003 Verification check 18 (unit part).
#include <doctest.h>

#include <cmath>
#include <limits>
#include <cstdint>

#include "core/packet.h"
#include "core/spectrum.h"
#include "state/flags.h"

namespace {

using ar::Lambda;
using ar::Real;
using ar::Spec;

Lambda make_lambda(Real a, Real b, Real c, Real d) { return Lambda{{a, b, c, d}}; }
Spec make_spec(Real a, Real b, Real c, Real d) { return Spec{{a, b, c, d}}; }

// True when got is within one ulp of the double-precision value want.
bool within_1ulp(Real got, double want) {
  const double ulp = static_cast<double>(std::nextafter(got, std::numeric_limits<Real>::infinity())) -
                     static_cast<double>(got);
  return std::fabs(static_cast<double>(got) - want) <= ulp;
}

}  // namespace

TEST_SUITE("fast") {
  AR_RT_CASE("packet/terminate keeps lane 0") {
    Lambda lam = make_lambda(450, 500, 550, 600);
    Spec beta = make_spec(0.5, 0.25, 2, 8);
    uint32_t flags = ar::set_hero_lane(0, 3);  // the hero lane must not be the one kept
    ar::terminate_secondaries(lam, beta, flags);
    for (int i = 0; i < ar::kLanes; ++i) {
      CHECK(lam.nm[i] == Real(450));
      CHECK(beta.v[i] == Real(0.5));
    }
    CHECK((flags & ar::SECONDARIES_TERMINATED) != 0u);
    CHECK((flags & ar::WAVELENGTHS_REWRITTEN) == 0u);
    CHECK(ar::hero(flags) == 3u);
  }

  AR_RT_CASE("packet/rewrite sets every lane") {
    Lambda lam = make_lambda(450, 500, 550, 600);
    Spec beta = make_spec(0.5, 0.25, 2, 8);
    uint32_t flags = 0;
    ar::terminate_secondaries(lam, beta, flags);
    ar::rewrite_wavelength(lam, beta, Real(620), Real(4), flags);
    for (int i = 0; i < ar::kLanes; ++i) {
      CHECK(lam.nm[i] == Real(620));
      CHECK(beta.v[i] == Real(2));  // 0.5 * 4 on every lane
    }
    CHECK((flags & ar::SECONDARIES_TERMINATED) != 0u);
    CHECK((flags & ar::WAVELENGTHS_REWRITTEN) != 0u);
  }

  AR_RT_CASE("packet/frequency shift g^5") {
    // Dyadic g (at most 5 bits, so g^5 also fits a float): g^5 is exact in double, so this checks the law beta*g^5, lambda*g, not rounding.
    const double gs[] = {1.25, 0.75, 13.0 / 8.0, 1.5, 0.5, 2.0};
    for (const double gd : gs) {
      Lambda lam = make_lambda(400, 500, 600, 700);
      Spec beta = make_spec(1.5, Real(0.3), 2, 7.25);
      const Lambda lam0 = lam;
      const Spec beta0 = beta;
      uint32_t flags = 0;
      ar::apply_frequency_shift(lam, beta, Real(gd), flags);
      const double g5 = gd * gd * gd * gd * gd;
      for (int i = 0; i < ar::kLanes; ++i) {
        CHECK(within_1ulp(lam.nm[i], static_cast<double>(lam0.nm[i]) * gd));
        CHECK(within_1ulp(beta.v[i], static_cast<double>(beta0.v[i]) * g5));
      }
      CHECK((flags & ar::WAVELENGTHS_REWRITTEN) != 0u);
      CHECK((flags & ar::SECONDARIES_TERMINATED) == 0u);
    }
  }

  AR_RT_CASE("packet/film weights") {
    const Spec pf = make_spec(0.5, 0.25, 2, 4);
    const Spec free_lanes = ar::film_weights(0, pf);
    CHECK(free_lanes.v[0] == Real(0.5));  // 1 / (4 * 0.5)
    CHECK(free_lanes.v[1] == Real(1));
    CHECK(free_lanes.v[2] == Real(0.125));
    CHECK(free_lanes.v[3] == Real(0.0625));
    const Spec one_lane = ar::film_weights(ar::SECONDARIES_TERMINATED, pf);
    CHECK(one_lane.v[0] == Real(2));  // 1 / pf_0
    CHECK(one_lane.v[1] == Real(0));
    CHECK(one_lane.v[2] == Real(0));
    CHECK(one_lane.v[3] == Real(0));
  }

  AR_RT_CASE("packet/hero bits") {
    const uint32_t bases[] = {0u, 0xffffffffu, 0x5a5a5a5au};
    for (const uint32_t base : bases) {
      for (uint32_t lane = 0; lane < 4; ++lane) {
        const uint32_t f = ar::set_hero_lane(base, lane);
        CHECK(ar::hero_lane(f) == lane);
        CHECK(ar::hero(f) == lane);
        CHECK((f & ~ar::HERO_LANE_MASK) == (base & ~ar::HERO_LANE_MASK));
      }
    }
    CHECK(ar::medium_depth(5u << 12) == 5u);
    CHECK(ar::medium_depth(0xffffffffu & ~ar::MEDIUM_DEPTH_MASK) == 0u);
  }

  AR_RT_CASE("packet/flag bits") {
    // Bits 0-21 are defined exactly once each and nothing above them.
    const uint32_t all[] = {ar::SECONDARIES_TERMINATED, ar::WAVELENGTHS_REWRITTEN, ar::LAST_BOUNCE_SPECULAR,
                            ar::AOV_WRITTEN,            ar::LAST_SEGMENT_CURVED,   ar::IN_CURVED_REGION,
                            ar::PATH_KIND,              ar::HERO_LANE_MASK,        ar::PARTITION_ARMED,
                            ar::PARTITION_CHAIN_NON_EMPTY, ar::PARTITION_CASTER_BIT, ar::MEDIUM_DEPTH_MASK,
                            ar::DENOISE_SURFACE_DONE,   ar::FIRST_VERTEX_CLASS_MASK, ar::SHADOW_CATCHER_HIT,
                            ar::SHADOW_CATCHER_PASS,    ar::FRONT_FACE,            ar::FIRST_BOUNCE_SPECULAR};
    uint32_t seen = 0;
    for (const uint32_t m : all) {
      CHECK((seen & m) == 0u);
      seen |= m;
    }
    CHECK(seen == (1u << 22) - 1u);
  }
}
