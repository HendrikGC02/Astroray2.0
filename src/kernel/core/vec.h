// SPDX-License-Identifier: Apache-2.0
// Small vector types, passed by value. No padding: Real3 is 12 B (float) or
// 24 B (double).
// Ref: docs/decisions/0002-kernel-architecture.md D1, D3; operation set after
// pbrt-v4 util/vecmath.h (Apache-2.0).
#pragma once
#include <cstdint>

#include "real.h"
#include "../portability.h"

AR_NS_BEGIN

struct Real2 {
  Real x, y;
};
struct Real3 {
  Real x, y, z;
};

AR_FN Real2 operator+(Real2 a, Real2 b) { return Real2{a.x + b.x, a.y + b.y}; }
AR_FN Real2 operator-(Real2 a, Real2 b) { return Real2{a.x - b.x, a.y - b.y}; }
AR_FN Real2 operator-(Real2 a) { return Real2{-a.x, -a.y}; }
AR_FN Real2 operator*(Real2 a, Real s) { return Real2{a.x * s, a.y * s}; }
AR_FN Real2 operator*(Real s, Real2 a) { return a * s; }
AR_FN Real2 operator/(Real2 a, Real s) { return Real2{a.x / s, a.y / s}; }
AR_FN Real dot(Real2 a, Real2 b) { return a.x * b.x + a.y * b.y; }

AR_FN Real3 operator+(Real3 a, Real3 b) { return Real3{a.x + b.x, a.y + b.y, a.z + b.z}; }
AR_FN Real3 operator-(Real3 a, Real3 b) { return Real3{a.x - b.x, a.y - b.y, a.z - b.z}; }
AR_FN Real3 operator-(Real3 a) { return Real3{-a.x, -a.y, -a.z}; }
AR_FN Real3 operator*(Real3 a, Real s) { return Real3{a.x * s, a.y * s, a.z * s}; }
AR_FN Real3 operator*(Real s, Real3 a) { return a * s; }
AR_FN Real3 operator/(Real3 a, Real s) { return Real3{a.x / s, a.y / s, a.z / s}; }

AR_FN Real dot(Real3 a, Real3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
AR_FN Real abs_dot(Real3 a, Real3 b) { return ar::abs(dot(a, b)); }
AR_FN Real3 cross(Real3 a, Real3 b) {
  return Real3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
AR_FN Real length(Real3 v) { return ar::sqrt(dot(v, v)); }
// A zero vector gives NaN, which the non-finite counters report; no silent guard.
AR_FN Real3 normalize(Real3 v) { return v / length(v); }
// n, or -n when n points away from v.
AR_FN Real3 face_forward(Real3 n, Real3 v) { return dot(n, v) < Real(0) ? -n : n; }

static_assert(sizeof(Real2) == 2 * sizeof(Real), "Real2 must have no padding");
static_assert(sizeof(Real3) == 3 * sizeof(Real), "Real3 must have no padding");

AR_NS_END
