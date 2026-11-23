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

AR_FN bool interval_is_inf(float x) { return x == bit_cast<float>(0x7f800000u) || x == bit_cast<float>(0xff800000u); }

AR_FN bool interval_has_zero(const Interval& a) { return a.lo <= 0.f && 0.f <= a.hi; }

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

// Precondition: lo and hi are results of float arithmetic, never a raw denormal input. With flush to
// zero on the GPU `x == 0.f` is true for a denormal, so stepping a denormal there would jump over it.
AR_FN Interval interval_widen(float lo, float hi) { return Interval{interval_step_down(lo), interval_step_up(hi)}; }

// A zero inside one box and an infinity at an end of the other: the op gives 0 * inf = NaN at that
// interior point, which the four corner products do not show.
AR_FN Interval interval_mul_scalar(const Interval& x, const Interval& y) {
  if (interval_is_nan(x) || interval_is_nan(y)) return interval_nan();
  if ((interval_has_zero(x) && (interval_is_inf(y.lo) || interval_is_inf(y.hi))) ||
      (interval_has_zero(y) && (interval_is_inf(x.lo) || interval_is_inf(x.hi))))
    return interval_nan();
  const float p0 = x.lo * y.lo, p1 = x.lo * y.hi, p2 = x.hi * y.lo, p3 = x.hi * y.hi;
  float lo = p0, hi = p0;
  const float rest[3] = {p1, p2, p3};
  for (int i = 0; i < 3; ++i) {
    lo = rest[i] < lo ? rest[i] : lo;
    hi = rest[i] > hi ? rest[i] : hi;
  }
  return interval_widen(lo, hi);
}

// -inf at one box's low end and +inf at the other's high end: the op gives inf - inf = NaN there, which
// the lo + lo and hi + hi sums do not show.
AR_FN Interval interval_add_scalar(const Interval& x, const Interval& y) {
  const float pinf = bit_cast<float>(0x7f800000u), ninf = bit_cast<float>(0xff800000u);
  if (interval_is_nan(x) || interval_is_nan(y)) return interval_nan();
  if ((x.lo == ninf && y.hi == pinf) || (x.hi == pinf && y.lo == ninf)) return interval_nan();
  return interval_widen(x.lo + y.lo, x.hi + y.hi);
}

// A NaN endpoint makes the whole interval {NaN, NaN}.
AR_FN Interval interval_exact(const Interval& a) { return interval_is_nan(a) ? interval_nan() : a; }

AR_FN Interval interval_const_f32(uint32_t bits) {
  const float v = bit_cast<float>(bits);
  return interval_exact(Interval{v, v});
}

AR_FN Interval3 interval_const_f32x3(const uint32_t* bits) {
  Interval3 r;
  for (int i = 0; i < 3; ++i) r.c[i] = interval_const_f32(bits[i]);
  return r;
}

// P over the box: the op converts P to float, which is monotone, so the box is exact.
AR_FN Interval3 interval_load_ctx_p(const Interval3& box) {
  Interval3 r;
  for (int i = 0; i < 3; ++i) r.c[i] = interval_exact(box.c[i]);
  return r;
}

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
