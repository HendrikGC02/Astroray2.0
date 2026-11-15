// SPDX-License-Identifier: Apache-2.0
// RGB to spectrum upsampling with the Jakob-Hanika coefficient table: s(l) = S(c0 l^2 + c1 l + c2),
// S(x) = 1/2 + x / (2 sqrt(1 + x^2)), l in nm. The table (data/spectral/rec709.jh) is made by
// tools/specgen from our own observer; this header reads it and evaluates it.
// Ref: Jakob and Hanika 2019, CGF 38(2); pbrt-v4 src/pbrt/util/color.h (RGBSigmoidPolynomial,
// RGBToSpectrumTable) and spectrum.h (RGBAlbedoSpectrum, RGBUnboundedSpectrum,
// RGBIlluminantSpectrum), Apache-2.0; docs/decisions/0003-spectral-model-and-path-state.md d16
// (method, table layout, conventions); docs/decisions/0005-material-model.md item 10 (which slot
// takes which convention), item 11 (one trilinear lookup, then the sigmoid per wavelength).
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../core/spectrum.h"
#include "../portability.h"
#include "observer.h"

AR_NS_BEGIN

// ---- the .jh file (0003 d18) -------------------------------------------------------------
// Little-endian. 0 magic "ARDATA\0\0"  8 version u32  12 kind u32 (2)  16 observer_hash[8]
// 24 res u32  28 illuminant_id u32  32 generator_version u32  36..63 zero
// 64 z_nodes f32[res]  then coeffs f32[3][res][res][res][3] (pbrt's layout:
// [largest component][z][y][x][c0 c1 c2]).
inline constexpr uint32_t kJhVersion = 1;
inline constexpr uint32_t kJhKind = 2;
inline constexpr uint32_t kJhIlluminantD65 = 1;  // the D65 SPD of observer.bin, Y = 1
inline constexpr uint32_t kJhGeneratorVersion = 1;
inline constexpr uint32_t kJhHeaderBytes = 64;
inline constexpr uint32_t kJhMaxRes = 256;  // keeps every index inside uint32_t

struct Rgb2SpecTable {
  int32_t res;
  uint32_t illuminant_id;
  const float* z_nodes;  // res nodes, smoothstep(smoothstep(k / (res - 1)))
  const float* coeffs;   // 3 * res^3 * 3 floats
  uint8_t observer_hash[8];
};

AR_FN uint32_t jh_u32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// nullptr on success, otherwise what is wrong with the blob (the caller shows it).
AR_FN const char* jh_view(const uint8_t* d, uint64_t size, Rgb2SpecTable* t) {
  const uint8_t magic[8] = {'A', 'R', 'D', 'A', 'T', 'A', 0, 0};
  if (size < kJhHeaderBytes) return "jh table: file shorter than its header";
  for (int i = 0; i < 8; ++i)
    if (d[i] != magic[i]) return "jh table: bad magic";
  if (jh_u32(d + 8) != kJhVersion || jh_u32(d + 12) != kJhKind) return "jh table: unsupported version or kind";
  const uint32_t res = jh_u32(d + 24);
  if (res < 2u || res > kJhMaxRes) return "jh table: resolution out of range";
  const uint64_t n = uint64_t(res);
  if (size != kJhHeaderBytes + 4u * n + 4u * 9u * n * n * n) return "jh table: size does not match its resolution";
  t->res = int32_t(res);
  t->illuminant_id = jh_u32(d + 28);
  for (int i = 0; i < 8; ++i) t->observer_hash[i] = d[16 + i];
  t->z_nodes = reinterpret_cast<const float*>(d + kJhHeaderBytes);
  t->coeffs = reinterpret_cast<const float*>(d + kJhHeaderBytes + 4u * n);
  return nullptr;
}

// ---- lookup --------------------------------------------------------------------------------
struct Rgb {
  Real v[3];
};

struct SigmoidCoeffs {
  Real c[3];  // c0 l^2 + c1 l + c2
};

// pbrt's IsInf guard needs an infinity without <limits>.
AR_FN float inf_of(float) { return bit_cast<float>(0x7f800000u); }
AR_FN double inf_of(double) { return bit_cast<double>(0x7ff0000000000000ull); }

// Ref: pbrt-v4 RGBSigmoidPolynomial::S. Infinite x gives exactly 0 or 1, which is how a
// uniform 0 or 1 comes out exact.
AR_FN Real sigmoid_s(Real x) {
  if (x == inf_of(x)) return Real(1);
  if (x == -inf_of(x)) return Real(0);
  return Real(0.5) + x / (Real(2) * ar::sqrt(Real(1) + x * x));
}

// Ref: pbrt-v4 RGBSigmoidPolynomial::operator(): EvaluatePolynomial is Horner with FMA, so one
// rounding per step; that matters in float, where c2 and the lambda terms cancel to O(1).
AR_FN Real sigmoid_eval(const SigmoidCoeffs& s, Real lambda) {
  return sigmoid_s(ar::fma(lambda, ar::fma(lambda, s.c[0], s.c[1]), s.c[2]));
}

// Ref: pbrt-v4 FindInterval: the largest i in [0, n - 2] with z_nodes[i] < z, by bisection.
AR_FN int find_interval(const float* z_nodes, int n, Real z) {
  int size = n - 2, first = 1;
  while (size > 0) {
    const int half = size >> 1, middle = first + half;
    const bool below = Real(load_ro(z_nodes + middle)) < z;
    first = below ? middle + 1 : first;
    size = below ? size - (half + 1) : half;
  }
  int i = first - 1;
  if (i < 0) i = 0;
  if (i > n - 2) i = n - 2;
  return i;
}

// Coefficients of the spectrum for rgb with every component in [0, 1] (callers clamp).
// Ref: pbrt-v4 RGBToSpectrumTable::operator(): grey is analytic, anything else is a trilinear
// blend of the 8 surrounding table entries.
AR_FN SigmoidCoeffs fetch_coeffs(const Rgb2SpecTable& t, Rgb rgb) {
  SigmoidCoeffs out;
  if (rgb.v[0] == rgb.v[1] && rgb.v[1] == rgb.v[2]) {
    const Real v = rgb.v[0];
    out.c[0] = Real(0);
    out.c[1] = Real(0);
    out.c[2] = v <= Real(0) ? -inf_of(v) : (v >= Real(1) ? inf_of(v) : (v - Real(0.5)) / ar::sqrt(v * (Real(1) - v)));
    return out;
  }
  const int res = t.res;
  const int maxc = (rgb.v[0] > rgb.v[1]) ? ((rgb.v[0] > rgb.v[2]) ? 0 : 2) : ((rgb.v[1] > rgb.v[2]) ? 1 : 2);
  const Real z = rgb.v[maxc];
  const Real x = rgb.v[(maxc + 1) % 3] * Real(res - 1) / z;
  const Real y = rgb.v[(maxc + 2) % 3] * Real(res - 1) / z;
  int xi = static_cast<int>(x), yi = static_cast<int>(y);
  if (xi > res - 2) xi = res - 2;
  if (yi > res - 2) yi = res - 2;
  const int zi = find_interval(t.z_nodes, res, z);
  const Real z0 = Real(load_ro(t.z_nodes + zi)), z1 = Real(load_ro(t.z_nodes + zi + 1));
  const Real dx = x - Real(xi), dy = y - Real(yi), dz = (z - z0) / (z1 - z0);
  const uint32_t r = uint32_t(res);
  for (int i = 0; i < 3; ++i) {
    Real co[2][2][2];  // [dz][dy][dx]
    for (int a = 0; a < 2; ++a)
      for (int b = 0; b < 2; ++b)
        for (int c = 0; c < 2; ++c) {
          const uint32_t idx =
              (((uint32_t(maxc) * r + uint32_t(zi + a)) * r + uint32_t(yi + b)) * r + uint32_t(xi + c)) * 3u + uint32_t(i);
          co[a][b][c] = Real(load_ro(t.coeffs + idx));
        }
    // pbrt Lerp(t, a, b) = (1 - t) a + t b
    const Real x00 = (Real(1) - dx) * co[0][0][0] + dx * co[0][0][1];
    const Real x10 = (Real(1) - dx) * co[0][1][0] + dx * co[0][1][1];
    const Real x01 = (Real(1) - dx) * co[1][0][0] + dx * co[1][0][1];
    const Real x11 = (Real(1) - dx) * co[1][1][0] + dx * co[1][1][1];
    const Real y0 = (Real(1) - dy) * x00 + dy * x10;
    const Real y1 = (Real(1) - dy) * x01 + dy * x11;
    out.c[i] = (Real(1) - dz) * y0 + dz * y1;
  }
  return out;
}

// ---- conventions (0003 d16, 0005 item 10) -----------------------------------------------------
// Flag bits a caller reports (0003 d16: "reported").
inline constexpr uint32_t kRgbNegativeClamped = 1u;  // a component was negative or NaN and became 0
inline constexpr uint32_t kRgbAboveOneClamped = 2u;  // albedo only: a component above 1 became 1

// value(l) = scale * S(c; l); the illuminant convention multiplies by the illuminant on top.
struct Upsampled {
  SigmoidCoeffs c;
  Real scale;
};

AR_FN Rgb rgb_clamp_negative(Rgb rgb, uint32_t* flags) {
  for (int i = 0; i < 3; ++i)
    if (!(rgb.v[i] >= Real(0))) {
      rgb.v[i] = Real(0);
      *flags |= kRgbNegativeClamped;
    }
  return rgb;
}

// REFLECTANCE: clamp each channel to [0, 1], then the albedo sigmoid.
AR_FN Upsampled albedo_coeffs(const Rgb2SpecTable& t, Rgb rgb, uint32_t* flags) {
  rgb = rgb_clamp_negative(rgb, flags);
  for (int i = 0; i < 3; ++i)
    if (rgb.v[i] > Real(1)) {
      rgb.v[i] = Real(1);
      *flags |= kRgbAboveOneClamped;
    }
  Upsampled u;
  u.c = fetch_coeffs(t, rgb);
  u.scale = Real(1);
  return u;
}

// UNBOUNDED and the base of ILLUMINANT: m = max(r, g, b), s = 2m S(rgb / 2m). Zero gives scale 0.
// Ref: pbrt-v4 RGBUnboundedSpectrum.
AR_FN Upsampled unbounded_coeffs(const Rgb2SpecTable& t, Rgb rgb, uint32_t* flags) {
  rgb = rgb_clamp_negative(rgb, flags);
  const Real m = max(rgb.v[0], max(rgb.v[1], rgb.v[2]));
  Upsampled u;
  u.scale = Real(2) * m;
  if (u.scale == Real(0)) {
    u.c.c[0] = Real(0);
    u.c.c[1] = Real(0);
    u.c.c[2] = Real(0);
    return u;
  }
  for (int i = 0; i < 3; ++i) rgb.v[i] = rgb.v[i] / u.scale;
  u.c = fetch_coeffs(t, rgb);
  return u;
}

AR_FN Spec upsampled_eval(const Upsampled& u, const Lambda& lam) {
  Spec s;
  for (int i = 0; i < kLanes; ++i) s.v[i] = u.scale == Real(0) ? Real(0) : u.scale * sigmoid_eval(u.c, lam.nm[i]);
  return s;
}

AR_FN Spec albedo(const Rgb2SpecTable& t, Rgb rgb, const Lambda& lam, uint32_t* flags) {
  return upsampled_eval(albedo_coeffs(t, rgb, flags), lam);
}

AR_FN Spec unbounded(const Rgb2SpecTable& t, Rgb rgb, const Lambda& lam, uint32_t* flags) {
  return upsampled_eval(unbounded_coeffs(t, rgb, flags), lam);
}

// ILLUMINANT: the unbounded spectrum times the working space's illuminant (Y = 1) at lambda.
// Ref: pbrt-v4 RGBIlluminantSpectrum.
AR_FN Spec illuminant(const Rgb2SpecTable& t, const ObserverView& obs, Rgb rgb, const Lambda& lam, uint32_t* flags) {
  Spec s = unbounded(t, rgb, lam, flags);
  for (int i = 0; i < kLanes; ++i) s.v[i] = s.v[i] * illuminant_at(obs, lam.nm[i]);
  return s;
}

AR_NS_END
