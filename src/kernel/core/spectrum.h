// SPDX-License-Identifier: Apache-2.0
// The spectral packet: kLanes radiometric values (Spec) at kLanes wavelengths
// (Lambda). The only place either type is defined.
// Ref: docs/decisions/0003-spectral-model-and-path-state.md d1; layout idea after pbrt-v4
// util/spectrum.h SampledSpectrum (Apache-2.0).
#pragma once
#include <cstdint>

#include "core/real.h"
#include "portability.h"

AR_NS_BEGIN

inline constexpr int kLanes = 4;

// Spec and Lambda differ only in the member's name, which keeps call sites
// readable (beta.v[i], lam.nm[i]); one macro makes the two operator sets
// identical by construction. x - x is 0 for every finite x and NaN for Inf and
// NaN, which is how is_finite avoids <cmath>.
#define AR_LANE_OPS(T, M)                                                      \
  AR_FN T operator+(T a, T b) {                                                \
    for (int i = 0; i < kLanes; ++i) a.M[i] += b.M[i];                         \
    return a;                                                                  \
  }                                                                            \
  AR_FN T operator-(T a, T b) {                                                \
    for (int i = 0; i < kLanes; ++i) a.M[i] -= b.M[i];                         \
    return a;                                                                  \
  }                                                                            \
  AR_FN T operator*(T a, T b) {                                                \
    for (int i = 0; i < kLanes; ++i) a.M[i] *= b.M[i];                         \
    return a;                                                                  \
  }                                                                            \
  AR_FN T operator/(T a, T b) {                                                \
    for (int i = 0; i < kLanes; ++i) a.M[i] /= b.M[i];                         \
    return a;                                                                  \
  }                                                                            \
  AR_FN T operator*(T a, Real s) {                                             \
    for (int i = 0; i < kLanes; ++i) a.M[i] *= s;                              \
    return a;                                                                  \
  }                                                                            \
  AR_FN T operator*(Real s, T a) { return a * s; }                             \
  AR_FN T operator/(T a, Real s) {                                             \
    for (int i = 0; i < kLanes; ++i) a.M[i] /= s;                              \
    return a;                                                                  \
  }                                                                            \
  AR_FN T& operator+=(T& a, T b) { return a = a + b; }                         \
  AR_FN T& operator-=(T& a, T b) { return a = a - b; }                         \
  AR_FN T& operator*=(T& a, T b) { return a = a * b; }                         \
  AR_FN T& operator*=(T& a, Real s) { return a = a * s; }                      \
  AR_FN Real avg(const T& a) {                                                 \
    Real s = a.M[0];                                                           \
    for (int i = 1; i < kLanes; ++i) s += a.M[i];                              \
    return s / Real(kLanes);                                                   \
  }                                                                            \
  AR_FN Real max_lane(const T& a) {                                            \
    Real m = a.M[0];                                                           \
    for (int i = 1; i < kLanes; ++i) m = max(m, a.M[i]);                       \
    return m;                                                                  \
  }                                                                            \
  AR_FN bool is_finite(const T& a) {                                           \
    bool ok = true;                                                            \
    for (int i = 0; i < kLanes; ++i) ok = ok && (a.M[i] - a.M[i] == Real(0));  \
    return ok;                                                                 \
  }

// Radiometric values, one per lane.
struct Spec {
  Real v[kLanes];
};
// Wavelengths in nm, one per lane.
struct Lambda {
  Real nm[kLanes];
};

AR_LANE_OPS(Spec, v)
AR_LANE_OPS(Lambda, nm)
#undef AR_LANE_OPS

// Every lane the same value.
AR_FN Spec spec_fill(Real x) {
  Spec s;
  for (int i = 0; i < kLanes; ++i) s.v[i] = x;
  return s;
}

static_assert(sizeof(Spec) == kLanes * sizeof(Real), "Spec must be exactly kLanes Reals");
static_assert(sizeof(Lambda) == kLanes * sizeof(Real), "Lambda must be exactly kLanes Reals");

AR_NS_END
