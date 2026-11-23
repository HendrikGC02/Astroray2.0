// SPDX-License-Identifier: Apache-2.0
// Equal-area mapping between the unit square [0,1]^2 and the unit sphere, the environment
// table's grid (0004 D11). Area-preserving: a region of the square of area A covers a solid
// angle of 4 pi A, so a density over the square divides by 4 pi to become one over directions.
//
// Ref: Clarberg 2008, "Fast Equal-Area Mapping of the (Hemi)Sphere using SIMD"; transcribed from
// third_party/_cache/refs/pbrt-v4-092b23a47ad4/src/pbrt/util/math.cpp EqualAreaSquareToSphere
// and EqualAreaSphereToSquare (Apache-2.0). pbrt states no error figure for its 6th-degree
// minimax atan(x) 2/pi polynomial; tests/cpu/test_env_table.cpp measures it and the round trip.
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../core/vec.h"
#include "../portability.h"

AR_NS_BEGIN

// pstd::copysign: the magnitude of `mag` with the sign bit of `sgn` (-0 counts as negative,
// which the comparison `sgn < 0` would not do).
AR_FN float ea_copysign(float mag, float sgn) {
  return bit_cast<float>((bit_cast<uint32_t>(mag) & 0x7fffffffu) | (bit_cast<uint32_t>(sgn) & 0x80000000u));
}
AR_FN double ea_copysign(double mag, double sgn) {
  return bit_cast<double>((bit_cast<uint64_t>(mag) & 0x7fffffffffffffffull) |
                          (bit_cast<uint64_t>(sgn) & 0x8000000000000000ull));
}

// pbrt's SafeSqrt.
AR_FN Real ea_safe_sqrt(Real x) { return ar::sqrt(ar::max(Real(0), x)); }

// p in [0,1]^2 (not checked: the caller's sample lies there). phi = t pi / 4 is evaluated as
// cospi(t / 4) and sinpi(t / 4), the same angle without the GPU's double-precision slow path of
// cos and sin (portability.h).
AR_FN Real3 equal_area_square_to_sphere(Real2 p) {
  // Transform p to [-1,1]^2 and compute absolute values.
  const Real u = Real(2) * p.x - Real(1), v = Real(2) * p.y - Real(1);
  const Real up = ar::abs(u), vp = ar::abs(v);
  // Radius r as the signed distance from the diagonal.
  const Real signed_distance = Real(1) - (up + vp);
  const Real d = ar::abs(signed_distance);
  const Real r = Real(1) - d;
  // phi / pi for the square-to-sphere mapping.
  const Real t = (r == Real(0) ? Real(1) : (vp - up) / r + Real(1)) / Real(4);
  // z for the spherical direction.
  const Real z = ea_copysign(Real(1) - r * r, signed_distance);
  // cos phi and sin phi for the original quadrant.
  const Real cos_phi = ea_copysign(ar::cospi(t), u);
  const Real sin_phi = ea_copysign(ar::sinpi(t), v);
  const Real s = r * ea_safe_sqrt(Real(2) - r * r);
  return Real3{cos_phi * s, sin_phi * s, z};
}

// d a unit vector. The angle comes from pbrt's polynomial for atan(b) 2 / pi on b in [0,1],
// evaluated by Horner's rule with fma as pbrt's EvaluatePolynomial does.
AR_FN Real2 equal_area_sphere_to_square(Real3 d) {
  const Real x = ar::abs(d.x), y = ar::abs(d.y), z = ar::abs(d.z);
  // The radius r = sqrt(1 - |z|).
  const Real r = ea_safe_sqrt(Real(1) - z);
  // The argument to atan (a = 0 detected to avoid a division by zero).
  const Real a = ar::max(x, y);
  Real b = ar::min(x, y);
  b = a == Real(0) ? Real(0) : b / a;
  // Coefficients t1..t7 of the 6th-degree minimax approximation of atan(x) 2 / pi, x in [0,1].
  Real phi = Real(-0.251390972343483509333252996350e-1);
  phi = ar::fma(b, phi, Real(0.419038818029165735901852432784e-1));
  phi = ar::fma(b, phi, Real(0.881770664775316294736387951347e-1));
  phi = ar::fma(b, phi, Real(-0.247333733281268944196501420480));
  phi = ar::fma(b, phi, Real(0.61572017898280213493197203466e-2));
  phi = ar::fma(b, phi, Real(0.636226545274016134946890922156));
  phi = ar::fma(b, phi, Real(0.406758566246788489601959989e-5));
  // Extend phi to the 45-90 degree range.
  if (x < y) phi = Real(1) - phi;
  // (u, v) from (r, phi).
  Real v = phi * r;
  Real u = r - v;
  if (d.z < Real(0)) {
    // Southern hemisphere: mirror u and v.
    const Real w = u;
    u = Real(1) - v;
    v = Real(1) - w;
  }
  // Move (u, v) to the quadrant of the signs of (x, y).
  u = ea_copysign(u, d.x);
  v = ea_copysign(v, d.y);
  // From [-1,1] to [0,1].
  return Real2{Real(0.5) * (u + Real(1)), Real(0.5) * (v + Real(1))};
}

AR_NS_END
