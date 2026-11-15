// SPDX-License-Identifier: Apache-2.0
// Response-channel conversion at sample time: one function turns a 4-lane spectral
// sample at its film wavelengths into any response channel, and the display channel maps
// through the CIE CMFs and the working-space matrix, divided by Y_w, with film exposure.
//   x_k = sum_i L_i R_k(lambda_i) w_u(lambda_i) w_i,  w_u = 1 (ENERGY) or lambda / (h c) (PHOTON)
// where w_i is 0003's film_weights (1/(4 pf_i), or the terminated form), so the 1/N and 1/p of
// 0008 d7 are already inside it. Accumulation is in double on both targets: the resolve
// sums in double (0008 d3), and the float and double builds then differ only by the inputs.
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

// R_k at lambda in nm; 0 outside [first knot, last knot] and for NaN.
AR_FN double response_at(const ResponseTableView& t, const ResponseChannel& ch, double lambda) {
  const float* nm = t.knot_nm + ch.first;
  const float* val = t.knot_value + ch.first;
  uint32_t lo = 0;
  uint32_t hi = ch.count - 1u;
  if (!(lambda >= double(load_ro(nm + lo)) && lambda <= double(load_ro(nm + hi)))) return double(0);
  // nm[lo] <= lambda <= nm[hi] holds throughout; lambda at the last knot ends with lo = count - 2.
  while (hi - lo > 1u) {
    const uint32_t mid = (lo + hi) >> 1;
    if (double(load_ro(nm + mid)) <= lambda) lo = mid;
    else hi = mid;
  }
  const double n0 = double(load_ro(nm + lo));
  const double n1 = double(load_ro(nm + hi));
  const double v0 = double(load_ro(val + lo));
  const double v1 = double(load_ro(val + hi));
  return v0 + (lambda - n0) / (n1 - n0) * (v1 - v0);
}

// channel < t.channel_count is the caller's contract (the host validates the request).
AR_FN double convert(const ResponseTableView& t, const Spec& L, const Lambda& lf, const Spec& w,
                     uint32_t channel) {
  AR_ASSERT(channel < t.channel_count);
  const ResponseChannel ch = t.channels[channel];
  double sum = double(0);
  for (int i = 0; i < kLanes; ++i) {
    const double lambda = double(lf.nm[i]);
    const double wu = ch.unit == RESPONSE_PHOTON ? lambda * t.photon_scale : double(1);
    sum += double(L.v[i]) * response_at(t, ch, lambda) * wu * double(w.v[i]);
  }
  return sum;
}

struct DisplayRgb {
  double v[3];
};

// M_ws * XYZ / Y_w * film_exposure, XYZ_k = convert(xbar, ybar, zbar) (0003 d11, 0008 d7).
// film_exposure scales display and light-pass channels only, never science channels.
AR_FN DisplayRgb display_rgb(const ResponseTableView& t, const Spec& L, const Lambda& lf,
                             const Spec& w, double film_exposure) {
  double xyz[3];
  for (int k = 0; k < 3; ++k) xyz[k] = convert(t, L, lf, w, t.display_first + uint32_t(k));
  const double s = film_exposure / t.y_w;
  DisplayRgb out;
  for (int r = 0; r < 3; ++r)
    out.v[r] = s * (t.m_ws[3 * r] * xyz[0] + t.m_ws[3 * r + 1] * xyz[1] + t.m_ws[3 * r + 2] * xyz[2]);
  return out;
}

AR_NS_END
