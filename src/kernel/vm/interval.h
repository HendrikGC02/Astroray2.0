// SPDX-License-Identifier: Apache-2.0
// Interval rules of the volume-reachable ops (0006 D7): interval_<name> bounds op_<name> over a box
// of its inputs. A majorant cell is built from these (0006 D6). Pure functions, CPU and GPU.
//
// Sums and products use closed-interval arithmetic (min and max of the four endpoint products), then
// each endpoint moves one ulp outward, because the float evaluation of the op rounds to nearest (error
// at most half an ulp) and must stay inside. Constants and the position box are exact.
// NaN: a NaN endpoint in an input, or one made by an operation (0 * inf), gives {NaN, NaN} from every
// rule, never a clamp; the majorant builder reports `!(lo <= hi)` as unsupported (0006 D7).
// Precondition: lo <= hi on every input interval; an inverted box has no defined result.
// Ref: R. E. Moore, Interval Analysis (1966), section 3; IEEE 754-2008 section 4.3 (rounding to nearest);
// docs/decisions/0006-volumes-and-astrophysics.md D7.
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../portability.h"

AR_NS_BEGIN

struct Interval {
  float lo, hi;
};
struct Interval3 {
  Interval c[3];
};

AR_FN bool interval_is_nan(const Interval& a) { return a.lo != a.lo || a.hi != a.hi; }

AR_FN Interval interval_nan() {
  const float n = bit_cast<float>(0x7fc00000u);
  return Interval{n, n};
}

// The next float toward +inf or -inf, as nextafterf does (which the kernel headers may not include):
// one step in the sign-magnitude bit pattern; NaN and the infinity already reached stay put.
AR_FN float interval_step_up(float x) {
  if (x != x || x == bit_cast<float>(0x7f800000u)) return x;
  if (x == 0.f) return bit_cast<float>(0x00000001u);
  const uint32_t b = bit_cast<uint32_t>(x);
  return bit_cast<float>(x > 0.f ? b + 1u : b - 1u);
}

AR_FN float interval_step_down(float x) {
  if (x != x || x == bit_cast<float>(0xff800000u)) return x;
  if (x == 0.f) return bit_cast<float>(0x80000001u);
  const uint32_t b = bit_cast<uint32_t>(x);
  return bit_cast<float>(x > 0.f ? b - 1u : b + 1u);
}

AR_FN Interval interval_widen(float lo, float hi) { return Interval{interval_step_down(lo), interval_step_up(hi)}; }

AR_FN Interval interval_mul_scalar(const Interval& x, const Interval& y) {
  if (interval_is_nan(x) || interval_is_nan(y)) return interval_nan();
  const float p0 = x.lo * y.lo, p1 = x.lo * y.hi, p2 = x.hi * y.lo, p3 = x.hi * y.hi;
  if (p0 != p0 || p1 != p1 || p2 != p2 || p3 != p3) return interval_nan();  // 0 * inf
  float lo = p0, hi = p0;
  const float rest[3] = {p1, p2, p3};
  for (int i = 0; i < 3; ++i) {
    lo = rest[i] < lo ? rest[i] : lo;
    hi = rest[i] > hi ? rest[i] : hi;
  }
  return interval_widen(lo, hi);
}

AR_FN Interval interval_add_scalar(const Interval& x, const Interval& y) {
  const float lo = x.lo + y.lo, hi = x.hi + y.hi;
  if (lo != lo || hi != hi) return interval_nan();  // NaN input or inf - inf
  return interval_widen(lo, hi);
}

AR_FN Interval interval_const_f32(uint32_t bits) {
  const float v = bit_cast<float>(bits);
  return Interval{v, v};
}

AR_FN Interval3 interval_const_f32x3(const uint32_t* bits) {
  Interval3 r;
  for (int i = 0; i < 3; ++i) r.c[i] = interval_const_f32(bits[i]);
  return r;
}

// P over the box: the op converts P to float, which is monotone, so the box is exact.
AR_FN Interval3 interval_load_ctx_p(const Interval3& box) { return box; }

AR_FN Interval interval_mul_f32(const Interval& x, const Interval& y) { return interval_mul_scalar(x, y); }

AR_FN Interval3 interval_mul_f32x3(const Interval3& x, const Interval3& y) {
  Interval3 r;
  for (int i = 0; i < 3; ++i) r.c[i] = interval_mul_scalar(x.c[i], y.c[i]);
  return r;
}

AR_FN Interval3 interval_mul_f32x3_f32(const Interval3& x, const Interval& s) {
  Interval3 r;
  for (int i = 0; i < 3; ++i) r.c[i] = interval_mul_scalar(x.c[i], s);
  return r;
}

AR_FN Interval3 interval_add_f32x3(const Interval3& x, const Interval3& y) {
  Interval3 r;
  for (int i = 0; i < 3; ++i) r.c[i] = interval_add_scalar(x.c[i], y.c[i]);
  return r;
}

AR_NS_END
