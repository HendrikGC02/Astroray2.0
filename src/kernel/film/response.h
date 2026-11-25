// SPDX-License-Identifier: Apache-2.0
// Response-channel conversion at sample time: one function turns a 4-lane spectral
// sample at its film wavelengths into any response channel, and the display channel maps
// through the CIE CMFs and the working-space matrix, divided by Y_w, with film exposure.
//   x_k = sum_i L_i R_k(lambda_i) w_u(lambda_i) w_i,  w_u = 1 (ENERGY) or lambda / (h c) (PHOTON)
// where w_i is 0003's film_weights (1/(4 pf_i), or the terminated form), so the 1/N and 1/p of
// 0008 d7 are already inside it. Accumulation is in double on both targets: the resolve
// sums in double (0008 d3), and the float and double builds then differ only by the inputs.
// The functions take the arithmetic type T as their one template parameter, double by default
// (every resolve call). A shade kernel, which may not use double on the GPU (0002 D1, D8), calls
// the float instantiation: the albedo guide of an escaped path (0008 d15). Both read the same
// tables; the double ones (photon scale, Y_w, M_ws, film exposure) are read as T at the point of
// use through response_scalar, which converts without a double instruction.
// R is piecewise linear between its knots and 0 outside them.
// Ref: docs/decisions/0008-film-outputs-denoise.md d7 (channels, display, film_exposure), d3;
// 0003 d2, d5, d11 (Y_w = Y_n), d13 (lambda_f, h and c); conversion at sample time after
// pbrt-v4 src/pbrt/film.cpp RGBFilm::AddSample and PixelSensor (Apache-2.0).
// Includes are relative so the host library can include this file without src/kernel on its
// include path.
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../core/spectrum.h"
#include "../portability.h"

// These two do not depend on Real, so they live outside the Real namespace: the host table
// (compiled once) and both kernel builds then share one definition.
namespace ar {

// Ref: 0008 d7 (units ENERGY and PHOTON). The values are part of the host-device table.
enum ResponseUnit : uint32_t { RESPONSE_ENERGY = 0, RESPONSE_PHOTON = 1 };

// One channel's knots are knot_nm / knot_value[first .. first + count).
struct ResponseChannel {
  uint32_t first;
  uint32_t count;  // >= 2, knots strictly ascending in nm
  uint32_t unit;   // ResponseUnit
  uint32_t reserved;
};

}  // namespace ar

AR_NS_BEGIN

// Built by the host (host/film/response_table.h); the arrays outlive the view. The display
// group is channels display_first .. display_first + 2 = xbar, ybar, zbar.
struct ResponseTableView {
  const float* knot_nm;
  const float* knot_value;
  const ResponseChannel* channels;
  uint32_t channel_count;
  uint32_t display_first;
  double photon_scale;  // 1e-9 / (h c) with CODATA 2018 h, c: w_u = lambda_nm * photon_scale
  double y_w;           // luminance of the unit white emitter = Y_n (0003 d11)
  double m_ws[9];       // XYZ -> working space, row-major
};

// The float nearest to d (round to nearest, ties to even, as a float(d) conversion), computed from
// d's bits with integer arithmetic: ptxas rejects any double instruction in a shade kernel, a
// conversion included (0002 D1). Infinities and NaN keep their class; doubles below the float
// range round to signed zero or a subnormal.
// Ref: IEEE 754-2008 sections 3.4 (binary64 and binary32 encodings) and 4.3.1 (roundTiesToEven).
AR_FN float f64_bits_to_f32(double d) {
  const uint64_t b = bit_cast<uint64_t>(d);
  const uint32_t sign = uint32_t(b >> 63) << 31;
  const int32_t e64 = int32_t((b >> 52) & 0x7ffu);
  const uint64_t m64 = b & ((uint64_t(1) << 52) - 1u);
  if (e64 == 0x7ff) return bit_cast<float>(sign | 0x7f800000u | (m64 != 0u ? 0x00400000u : 0u));
  if (e64 == 0) return bit_cast<float>(sign);  // zero or a double subnormal: far below the float range
  int32_t e = e64 - 1023 + 127;
  // The significand with its leading 1, and the bits dropped to reach 24 significant bits (more for
  // a float subnormal).
  const uint64_t sig = m64 | (uint64_t(1) << 52);
  const int32_t shift = e > 0 ? 29 : 29 + 1 - e;
  if (shift > 53) return bit_cast<float>(sign);
  uint64_t m = sig >> shift;
  const uint64_t rem = sig & ((uint64_t(1) << shift) - 1u), half = uint64_t(1) << (shift - 1);
  if (rem > half || (rem == half && (m & 1u) != 0u)) ++m;
  if (e <= 0) return bit_cast<float>(sign | uint32_t(m));  // a subnormal; m = 2^23 is the smallest normal
  if (m == (uint64_t(1) << 24)) m >>= 1, ++e;  // the rounding carried into the exponent
  if (e >= 255) return bit_cast<float>(sign | 0x7f800000u);
  return bit_cast<float>(sign | (uint32_t(e) << 23) | (uint32_t(m) & 0x7fffffu));
}

// A double table value as the arithmetic type T.
template <class T>
AR_FN T response_scalar(double d) {
  if constexpr (sizeof(T) == sizeof(double)) return T(d);
  else return T(f64_bits_to_f32(d));
}

// R_k at lambda in nm; 0 outside [first knot, last knot] and for NaN.
template <class T = double>
AR_FN T response_at(const ResponseTableView& t, const ResponseChannel& ch, T lambda) {
  const float* nm = t.knot_nm + ch.first;
  const float* val = t.knot_value + ch.first;
  uint32_t lo = 0;
  uint32_t hi = ch.count - 1u;
  if (!(lambda >= T(load_ro(nm + lo)) && lambda <= T(load_ro(nm + hi)))) return T(0);
  // nm[lo] <= lambda <= nm[hi] holds throughout; lambda at the last knot ends with lo = count - 2.
  while (hi - lo > 1u) {
    const uint32_t mid = (lo + hi) >> 1;
    if (T(load_ro(nm + mid)) <= lambda) lo = mid;
    else hi = mid;
  }
  const T n0 = T(load_ro(nm + lo));
  const T n1 = T(load_ro(nm + hi));
  const T v0 = T(load_ro(val + lo));
  const T v1 = T(load_ro(val + hi));
  return v0 + (lambda - n0) / (n1 - n0) * (v1 - v0);
}

// channel < t.channel_count is the caller's contract (the host validates the request).
template <class T = double>
AR_FN T convert(const ResponseTableView& t, const Spec& L, const Lambda& lf, const Spec& w, uint32_t channel) {
  AR_ASSERT(channel < t.channel_count);
  const ResponseChannel ch = t.channels[channel];
  T sum = T(0);
  for (int i = 0; i < kLanes; ++i) {
    const T lambda = T(lf.nm[i]);
    const T wu = ch.unit == RESPONSE_PHOTON ? lambda * response_scalar<T>(t.photon_scale) : T(1);
    sum += T(L.v[i]) * response_at<T>(t, ch, lambda) * wu * T(w.v[i]);
  }
  return sum;
}

template <class T>
struct DisplayRgbOf {
  T v[3];
};
using DisplayRgb = DisplayRgbOf<double>;

// M_ws * XYZ / Y_w * film_exposure, XYZ_k = convert(xbar, ybar, zbar) (0003 d11, 0008 d7).
// film_exposure scales display and light-pass channels only, never science channels.
template <class T = double>
AR_FN DisplayRgbOf<T> display_rgb(const ResponseTableView& t, const Spec& L, const Lambda& lf, const Spec& w,
                                  double film_exposure) {
  T xyz[3];
  for (int k = 0; k < 3; ++k) xyz[k] = convert<T>(t, L, lf, w, t.display_first + uint32_t(k));
  const T s = response_scalar<T>(film_exposure) / response_scalar<T>(t.y_w);
  DisplayRgbOf<T> out;
  for (int r = 0; r < 3; ++r)
    out.v[r] = s * (response_scalar<T>(t.m_ws[3 * r]) * xyz[0] + response_scalar<T>(t.m_ws[3 * r + 1]) * xyz[1] +
                    response_scalar<T>(t.m_ws[3 * r + 2]) * xyz[2]);
  return out;
}

AR_NS_END
