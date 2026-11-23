// ar-f64
// SPDX-License-Identifier: Apache-2.0
// 0006 D7: each interval_<name> in vm/interval.h encloses the op it names. 1e4 seeded random boxes per
// rule; 16 random points inside each go through the op_* function and must land in the interval. A box
// with lo = hi must come back as that point within 1 ulp. NaN propagates, never clamps.
// Ref: docs/decisions/0006-volumes-and-astrophysics.md D7; Moore, Interval Analysis (1966), section 3.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>

#include "vm/interp.h"
#include "vm/interval.h"

namespace {

using ar::Interval;
using ar::Interval3;

struct Rng {
  std::mt19937 g{178u};
  float u() { return std::uniform_real_distribution<float>(0.f, 1.f)(g); }
  // Mixed scales and signs, so products cross zero and span many binades.
  float value() {
    const float mag = std::exp2(std::uniform_real_distribution<float>(-8.f, 8.f)(g));
    return (u() < 0.5f ? -mag : mag) * (u() < 0.1f ? 0.f : 1.f);
  }
  Interval box() {
    const float a = value(), b = value();
    return a <= b ? Interval{a, b} : Interval{b, a};
  }
  Interval3 box3() { return Interval3{{box(), box(), box()}}; }
  float in(const Interval& i) {
    float x = i.lo + u() * (i.hi - i.lo);
    x = x < i.lo ? i.lo : x;
    return x > i.hi ? i.hi : x;
  }
};

bool contains(const Interval& i, float x) { return i.lo <= x && x <= i.hi; }

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
    for (int k = 0; k < kPoints; ++k) {
      const float a = rng.in(s), b = rng.in(t);
      const float xa[3] = {rng.in(x.c[0]), rng.in(x.c[1]), rng.in(x.c[2])};
      const float ya[3] = {rng.in(y.c[0]), rng.in(y.c[1]), rng.in(y.c[2])};
      CHECK(contains(ms, ar::op_mul_f32(a, b)));
      float o[3];
      ar::op_mul_f32x3(xa, ya, o);
      for (int i = 0; i < 3; ++i) CHECK(contains(mv.c[i], o[i]));
      ar::op_mul_f32x3_f32(xa, a, o);
      for (int i = 0; i < 3; ++i) CHECK(contains(mvs.c[i], o[i]));
      ar::op_add_f32x3(xa, ya, o);
      for (int i = 0; i < 3; ++i) CHECK(contains(av.c[i], o[i]));
      ar::ShadeContext ctx = {};
      ctx.P = ar::Real3{ar::Real(xa[0]), ar::Real(xa[1]), ar::Real(xa[2])};
      ar::op_load_ctx_p(ctx, o);
      for (int i = 0; i < 3; ++i) CHECK(contains(p.c[i], o[i]));
    }
    // Constants: the point interval holds the op's value exactly.
    const float v = rng.value();
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    const uint32_t bits3[3] = {bits, bits ^ 0x1u, bits};
    CHECK(contains(ar::interval_const_f32(bits), ar::op_const_f32(bits)));
    for (int i = 0; i < 3; ++i) CHECK(contains(ar::interval_const_f32x3(bits3).c[i], ar::op_const_f32(bits3[i])));
  }
}

TEST_CASE("interval/degenerate box is a point") {
  Rng rng;
  for (int n = 0; n < 10000; ++n) {
    const float a = rng.value(), b = rng.value();
    const Interval s{a, a}, t{b, b};
    const float want_mul = ar::op_mul_f32(a, b);
    const Interval m = ar::interval_mul_f32(s, t);
    CHECK(ulps(m.lo, want_mul) <= 1);
    CHECK(ulps(m.hi, want_mul) <= 1);
    const float xa[3] = {a, b, a}, ya[3] = {b, a, b};
    const Interval3 x{{{a, a}, {b, b}, {a, a}}}, y{{{b, b}, {a, a}, {b, b}}};
    float o[3];
    ar::op_add_f32x3(xa, ya, o);
    const Interval3 r = ar::interval_add_f32x3(x, y);
    for (int i = 0; i < 3; ++i) {
      CHECK(ulps(r.c[i].lo, o[i]) <= 1);
      CHECK(ulps(r.c[i].hi, o[i]) <= 1);
    }
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
}

}  // TEST_SUITE
