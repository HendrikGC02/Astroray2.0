// ar-f64
// SPDX-License-Identifier: Apache-2.0
// 0006 D7: each interval_<name> in vm/interval.h encloses the op it names. 1e4 seeded random boxes per
// rule, a sixth of the endpoints edge values (zeros, infinities, FLT_MAX, denormals, NaN); 16 points of
// each box go through the op_* function and must land in the interval, a NaN result in a {NaN, NaN}
// box. A box with lo = hi must come back exactly one ulp wide on each side. NaN propagates, never clamps.
// Ref: docs/decisions/0006-volumes-and-astrophysics.md D7; Moore, Interval Analysis (1966), section 3.
#include <doctest.h>

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>

#include "medium/medium_eval.h"
#include "vm/interp.h"
#include "vm/interval.h"

namespace {

using ar::Interval;
using ar::Interval3;

struct Rng {
  std::mt19937 g{178u};
  float u() { return std::uniform_real_distribution<float>(0.f, 1.f)(g); }
  // Finite, mixed scales and signs, so products cross zero and span many binades; sometimes exactly 0.
  float finite() {
    const float mag = std::exp2(std::uniform_real_distribution<float>(-8.f, 8.f)(g));
    return (u() < 0.5f ? -mag : mag) * (u() < 0.1f ? 0.f : 1.f);
  }
  float value() {
    if (u() < 1.f / 6.f) {
      const float edge[] = {0.f, -0.f, INFINITY, -INFINITY, FLT_MAX, -FLT_MAX, FLT_TRUE_MIN, -FLT_TRUE_MIN, std::nanf("")};
      return edge[g() % (sizeof(edge) / sizeof(edge[0]))];
    }
    return finite();
  }
  Interval box() {
    const float a = value(), b = value();
    if (a != a || b != b) return Interval{a, b};  // a NaN endpoint is kept as it is
    return a <= b ? Interval{a, b} : Interval{b, a};
  }
  Interval3 box3() { return Interval3{{box(), box(), box()}}; }
  // A point of the box: an endpoint, zero when inside, or an interior point when the span is finite.
  float in(const Interval& i) {
    if (i.lo != i.lo || i.hi != i.hi) return 0.f;
    const unsigned k = g() % 4;
    if (k == 0) return i.lo;
    if (k == 1) return i.hi;
    if (k == 2 && i.lo <= 0.f && 0.f <= i.hi) return 0.f;
    if (!std::isfinite(i.hi - i.lo)) return u() < 0.5f ? i.lo : i.hi;
    float x = i.lo + u() * (i.hi - i.lo);
    x = x < i.lo ? i.lo : x;
    return x > i.hi ? i.hi : x;
  }
};

bool both_nan(const Interval& i) { return i.lo != i.lo && i.hi != i.hi; }

// The enclosure contract: a NaN result needs a {NaN, NaN} box; otherwise the box holds the value. A
// {NaN, NaN} box is always acceptable (the majorant builder reports it unsupported).
bool encloses(const Interval& i, float x) {
  if (ar::interval_is_nan(i)) return both_nan(i);
  return x == x && i.lo <= x && x <= i.hi;
}

// The configurations where an op can really produce NaN: a NaN endpoint, 0 * inf inside a product, and
// inf - inf inside a sum. Outside them a rule returning {NaN, NaN} is a bug, not caution.
bool has_zero(const Interval& a) { return a.lo <= 0.f && 0.f <= a.hi; }
bool has_inf(const Interval& a) { return std::isinf(a.lo) || std::isinf(a.hi); }
bool is_nan_box(const Interval& a) { return a.lo != a.lo || a.hi != a.hi; }
bool mul_defined(const Interval& x, const Interval& y) {
  return !is_nan_box(x) && !is_nan_box(y) && !(has_zero(x) && has_inf(y)) && !(has_zero(y) && has_inf(x));
}
bool add_defined(const Interval& x, const Interval& y) {
  return !is_nan_box(x) && !is_nan_box(y) && !(x.lo == -INFINITY && y.hi == INFINITY) &&
         !(x.hi == INFINITY && y.lo == -INFINITY);
}

// Distance in representable floats; both finite.
int64_t ulps(float a, float b) {
  auto key = [](float f) {
    int32_t i;
    std::memcpy(&i, &f, 4);
    return int64_t(i < 0 ? int32_t(0x80000000u) - i : i);
  };
  return std::llabs(key(a) - key(b));
}

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("interval/rules enclose the op") {
  Rng rng;
  constexpr int kBoxes = 10000, kPoints = 16;
  for (int n = 0; n < kBoxes; ++n) {
    const Interval s = rng.box(), t = rng.box();
    const Interval3 x = rng.box3(), y = rng.box3();
    const Interval ms = ar::interval_mul_f32(s, t);
    const Interval3 mv = ar::interval_mul_f32x3(x, y), mvs = ar::interval_mul_f32x3_f32(x, s),
                    av = ar::interval_add_f32x3(x, y), p = ar::interval_load_ctx_p(x);
    // Where the op cannot give NaN the rule must not either.
    if (mul_defined(s, t)) CHECK_FALSE(ar::interval_is_nan(ms));
    for (int i = 0; i < 3; ++i) {
      if (mul_defined(x.c[i], y.c[i])) CHECK_FALSE(ar::interval_is_nan(mv.c[i]));
      if (mul_defined(x.c[i], s)) CHECK_FALSE(ar::interval_is_nan(mvs.c[i]));
      if (add_defined(x.c[i], y.c[i])) CHECK_FALSE(ar::interval_is_nan(av.c[i]));
      if (!is_nan_box(x.c[i])) CHECK_FALSE(ar::interval_is_nan(p.c[i]));
    }
    for (int k = 0; k < kPoints; ++k) {
      const float a = rng.in(s), b = rng.in(t);
      const float xa[3] = {rng.in(x.c[0]), rng.in(x.c[1]), rng.in(x.c[2])};
      const float ya[3] = {rng.in(y.c[0]), rng.in(y.c[1]), rng.in(y.c[2])};
      CHECK(encloses(ms, ar::op_mul_f32(a, b)));
      float o[3];
      ar::op_mul_f32x3(xa, ya, o);
      for (int i = 0; i < 3; ++i) CHECK(encloses(mv.c[i], o[i]));
      ar::op_mul_f32x3_f32(xa, a, o);
      for (int i = 0; i < 3; ++i) CHECK(encloses(mvs.c[i], o[i]));
      ar::op_add_f32x3(xa, ya, o);
      for (int i = 0; i < 3; ++i) CHECK(encloses(av.c[i], o[i]));
      ar::ShadeContext ctx = {};
      ctx.P = ar::Real3{ar::Real(xa[0]), ar::Real(xa[1]), ar::Real(xa[2])};
      ar::op_load_ctx_p(ctx, o);
      for (int i = 0; i < 3; ++i) CHECK(encloses(p.c[i], o[i]));
    }
    // Constants: the point interval holds the op's value (a NaN constant gives {NaN, NaN}).
    const float v = rng.value();
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    const uint32_t bits3[3] = {bits, bits ^ 0x1u, bits};
    CHECK(encloses(ar::interval_const_f32(bits), ar::op_const_f32(bits)));
    if (std::isfinite(v)) {  // a finite constant is exactly the point {v, v}
      CHECK(ar::interval_const_f32(bits).lo == v);
      CHECK(ar::interval_const_f32(bits).hi == v);
    }
    for (int i = 0; i < 3; ++i) CHECK(encloses(ar::interval_const_f32x3(bits3).c[i], ar::op_const_f32(bits3[i])));
  }
}

// Exactly one ulp outward on each side of the rounded result: a dropped or doubled widening fails.
TEST_CASE("interval/degenerate box is a point") {
  Rng rng;
  auto one_ulp_around = [](const Interval& r, float want) {
    return r.lo < want && want < r.hi && ulps(r.lo, want) == 1 && ulps(r.hi, want) == 1;
  };
  for (int n = 0; n < 10000; ++n) {
    const float a = rng.finite(), b = rng.finite();
    const Interval s{a, a}, t{b, b};
    CHECK(one_ulp_around(ar::interval_mul_f32(s, t), ar::op_mul_f32(a, b)));
    const float xa[3] = {a, b, a}, ya[3] = {b, a, b};
    const Interval3 x{{{a, a}, {b, b}, {a, a}}}, y{{{b, b}, {a, a}, {b, b}}};
    float o[3];
    ar::op_add_f32x3(xa, ya, o);
    const Interval3 r = ar::interval_add_f32x3(x, y);
    for (int i = 0; i < 3; ++i) CHECK(one_ulp_around(r.c[i], o[i]));
    ar::op_mul_f32x3(xa, ya, o);
    const Interval3 mv = ar::interval_mul_f32x3(x, y), mvs = ar::interval_mul_f32x3_f32(x, t);
    for (int i = 0; i < 3; ++i) CHECK(one_ulp_around(mv.c[i], o[i]));
    ar::op_mul_f32x3_f32(xa, b, o);
    for (int i = 0; i < 3; ++i) CHECK(one_ulp_around(mvs.c[i], o[i]));
  }
}

TEST_CASE("interval/ulp step matches nextafter") {
  const float inf = INFINITY, nan = std::nanf("");
  const float xs[] = {0.f, -0.f, FLT_MAX, -FLT_MAX, inf, -inf, nan, 1.f, -1.f, FLT_MIN, -FLT_MIN, FLT_TRUE_MIN,
                      -FLT_TRUE_MIN};
  auto same = [](float a, float b) {
    if (a != a || b != b) return (a != a) && (b != b);
    return std::memcmp(&a, &b, 4) == 0;  // bitwise, so +0 and -0 differ
  };
  for (float x : xs) {
    INFO(x);
    CHECK(same(ar::interval_step_up(x), std::nextafter(x, inf)));
    CHECK(same(ar::interval_step_down(x), std::nextafter(x, -inf)));
  }
}

TEST_CASE("interval/NaN endpoints propagate") {
  const float nan = std::nanf(""), inf = INFINITY;
  const Interval bad{nan, 1.f}, ok{1.f, 2.f}, zero{0.f, 0.f}, infinite{inf, inf};
  CHECK(ar::interval_is_nan(ar::interval_mul_f32(bad, ok)));
  CHECK(ar::interval_is_nan(ar::interval_mul_f32(ok, bad)));
  CHECK(ar::interval_is_nan(ar::interval_mul_f32(zero, infinite)));
  const Interval3 b3{{bad, ok, ok}}, o3{{ok, ok, ok}};
  CHECK(ar::interval_is_nan(ar::interval_add_f32x3(b3, o3).c[0]));
  CHECK_FALSE(ar::interval_is_nan(ar::interval_add_f32x3(b3, o3).c[1]));
  CHECK(ar::interval_is_nan(ar::interval_mul_f32x3_f32(o3, bad).c[2]));
  // One NaN endpoint of the position box gives {NaN, NaN}, both ends; a NaN constant too.
  for (const Interval& e : {Interval{nan, 1.f}, Interval{1.f, nan}}) {
    const Interval3 p = ar::interval_load_ctx_p(Interval3{{ok, e, ok}});
    CHECK(both_nan(p.c[1]));
    CHECK_FALSE(ar::interval_is_nan(p.c[0]));
  }
  CHECK(both_nan(ar::interval_const_f32(0x7fc00000u)));
}

// Interior points the endpoint values do not show: 0 * inf and inf - inf.
TEST_CASE("interval/infinity corners the endpoints miss") {
  const float inf = INFINITY;
  const Interval around_zero{-1.f, 1.f}, pos_inf{inf, inf}, mixed{-inf, 1.f}, upper{0.f, inf}, pos{1.f, 2.f};
  CHECK(both_nan(ar::interval_mul_f32(around_zero, pos_inf)));
  CHECK(both_nan(ar::interval_mul_f32(pos_inf, around_zero)));
  // No zero inside: the product is a real infinity, not NaN.
  CHECK_FALSE(ar::interval_is_nan(ar::interval_mul_f32(pos, pos_inf)));
  CHECK(ar::interval_mul_f32(pos, pos_inf).lo > 0.f);
  const Interval3 m{{mixed, mixed, mixed}}, u3{{upper, upper, upper}};
  CHECK(both_nan(ar::interval_add_f32x3(m, u3).c[0]));
  CHECK(both_nan(ar::interval_add_f32x3(u3, m).c[2]));
  // Same-signed infinities add to an infinity.
  CHECK_FALSE(ar::interval_is_nan(ar::interval_add_f32x3(u3, u3).c[1]));
}

// VOLUME_COEFFS reads no register, so its interval is the block's extinction bound
// (medium_block_sigma_t_bound) and must hold sigma_t of the op at every wavelength. Random blocks:
// RGB refs with sigmoid coefficients over the range real colours give, scales from 0 to 8, CONSTANT
// refs, zero to two lobes; a lane of the evaluated sample lies in the interval.
TEST_CASE("interval/volume coeffs encloses the op") {
  std::mt19937 g(179u);
  auto u = [&](float lo, float hi) { return lo + (hi - lo) * std::uniform_real_distribution<float>(0.f, 1.f)(g); };
  auto ref = [&]() {
    ar::SpectrumRef r = {};
    r.semantic = uint8_t(ar::SpectrumSemantic::REFLECTANCE);
    if (g() % 4 == 0) {
      r.kind = uint8_t(ar::SpectrumKind::CONSTANT);
      r.semantic = uint8_t(ar::SpectrumSemantic::UNBOUNDED);  // a medium's constants are not clamped to 1
      r.p[0] = u(0.f, 8.f);
    } else {
      r.kind = uint8_t(ar::SpectrumKind::RGB);
      r.p[0] = u(-2e-4f, 2e-4f);
      r.p[1] = u(-0.2f, 0.2f);
      r.p[2] = u(-60.f, 60.f);
      r.p[3] = u(0.f, 8.f);
    }
    return r;
  };
  const ar::SceneTables tables = {};
  for (int n = 0; n < 4000; ++n) {
    ar::MediumBlock b = ar::medium_block_vacuum();
    b.sigma_a = ref();
    b.sigma_s[0] = ref();
    b.sigma_s[1] = ref();
    b.lobe_count = g() % 3;
    const Interval bound = ar::medium_block_sigma_t_bound(b);
    const Interval box = ar::interval_volume_coeffs(bound);
    CHECK_FALSE(ar::interval_is_nan(box));
    // Tight, not merely valid: finite, and no wider than the refs' own scales plus the 2^-22 slack
    // per ref, so {-inf, +inf} or a loose widening fails.
    float scales = 0.f;
    scales += b.sigma_a.kind == uint8_t(ar::SpectrumKind::CONSTANT) ? b.sigma_a.p[0] : b.sigma_a.p[3];
    for (uint32_t l = 0; l < b.lobe_count; ++l)
      scales += b.sigma_s[l].kind == uint8_t(ar::SpectrumKind::CONSTANT) ? b.sigma_s[l].p[0] : b.sigma_s[l].p[3];
    CHECK(std::isfinite(box.lo));
    CHECK(std::isfinite(box.hi));
    // Each ref adds 2^-22 of slack, one ulp (2^-23) for the step up and one for the widening of
    // the sum it joins: 2^-21 of its scale per ref, per side.
    const float width = 4.76837158203125e-7f * float(1u + b.lobe_count);
    CHECK(box.lo >= -scales * width - 1e-30f);
    CHECK(box.hi <= scales * (1.f + width) + 1e-30f);
    for (int k = 0; k < 8; ++k) {
      ar::Lambda lam;
      for (int l = 0; l < 4; ++l) lam.nm[l] = ar::Real(u(360.f, 830.f));
      ar::MediumSample s;
      REQUIRE(ar::op_volume_coeffs(b, tables.observer, lam, &s) == ar::VmStatus::OK);
      const ar::Spec st = ar::medium_sigma_t(s);
      for (int l = 0; l < 4; ++l) CHECK(encloses(box, float(st.v[l])));
    }
  }
  // A negative scale or a NaN has no bound: {NaN, NaN}, never a clamp.
  ar::MediumBlock bad = ar::medium_block_vacuum();
  bad.sigma_a.kind = uint8_t(ar::SpectrumKind::RGB);
  bad.sigma_a.semantic = uint8_t(ar::SpectrumSemantic::REFLECTANCE);
  bad.sigma_a.p[3] = -1.f;
  CHECK(both_nan(ar::medium_block_sigma_t_bound(bad)));
  bad.sigma_a.p[3] = std::nanf("");
  CHECK(both_nan(ar::medium_block_sigma_t_bound(bad)));
  // A (kind, semantic) pair the exact bound does not cover has no bound either.
  bad.sigma_a.p[3] = 1.f;
  CHECK_FALSE(ar::interval_is_nan(ar::medium_block_sigma_t_bound(bad)));
  bad.sigma_a.semantic = uint8_t(ar::SpectrumSemantic::ILLUMINANT);
  CHECK(both_nan(ar::medium_block_sigma_t_bound(bad)));
  CHECK(both_nan(ar::interval_volume_coeffs(Interval{std::nanf(""), 1.f})));
}

}  // TEST_SUITE
