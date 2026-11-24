// SPDX-License-Identifier: Apache-2.0
// The grating efficiency table: eta_m(lambda, cos theta_i) for every order of one grating, built on
// the host only (0005 item 24) and read here.
// Layout: f32, [order][cos bin][wavelength], order index m - m_min; 95 wavelengths at 5 nm from
// 360 to 830 nm; 16 cos theta_i nodes at k / 15, so normal incidence (cos = 1) and grazing (0) are
// nodes; at most 17 orders (|m| <= 8): 17 * 16 * 95 * 4 B = 103360 B.
// Lookup: linear in wavelength and in cos theta_i, clamped at the ends of both axes.
// Ref: docs/decisions/0005-material-model.md items 23-24.
#pragma once
#include <cstdint>

#include "../../core/real.h"
#include "../../portability.h"

AR_NS_BEGIN

inline constexpr int kGratingWavelengths = 95;
inline constexpr int kGratingCosBins = 16;
inline constexpr int kGratingMaxOrders = 17;
inline constexpr Real kGratingLambdaMin = Real(360);
inline constexpr Real kGratingLambdaStep = Real(5);

static_assert(kGratingLambdaMin + kGratingLambdaStep * Real(kGratingWavelengths - 1) == Real(830), "360-830 nm");
static_assert(sizeof(float) * kGratingMaxOrders * kGratingCosBins * kGratingWavelengths == 103360, "item 23 size");

// A view: `eta` holds order_count * kGratingCosBins * kGratingWavelengths floats.
struct GratingTable {
  const float* eta;
  int32_t m_min;
  int32_t order_count;
};

AR_FN uint32_t grating_table_index(int order_index, int cos_bin, int wavelength) {
  return (uint32_t(order_index) * uint32_t(kGratingCosBins) + uint32_t(cos_bin)) * uint32_t(kGratingWavelengths) +
         uint32_t(wavelength);
}

// eta_m at (lambda_nm, cos_i); 0 for an order outside the table.
AR_FN Real grating_table_eta(const GratingTable& t, int m, Real lambda_nm, Real cos_i) {
  const int o = m - t.m_min;
  if (o < 0 || o >= t.order_count) return Real(0);
  // Written so a NaN coordinate lands on the first node instead of reaching the int conversion.
  Real x = (lambda_nm - kGratingLambdaMin) / kGratingLambdaStep;
  x = x >= Real(0) ? (x < Real(kGratingWavelengths - 1) ? x : Real(kGratingWavelengths - 1)) : Real(0);
  Real y = cos_i * Real(kGratingCosBins - 1);
  y = y >= Real(0) ? (y < Real(kGratingCosBins - 1) ? y : Real(kGratingCosBins - 1)) : Real(0);
  const int x0 = int(x) < kGratingWavelengths - 2 ? int(x) : kGratingWavelengths - 2;
  const int y0 = int(y) < kGratingCosBins - 2 ? int(y) : kGratingCosBins - 2;
  const Real fx = x - Real(x0);
  const Real fy = y - Real(y0);
  const Real e00 = Real(load_ro(t.eta + grating_table_index(o, y0, x0)));
  const Real e01 = Real(load_ro(t.eta + grating_table_index(o, y0, x0 + 1)));
  const Real e10 = Real(load_ro(t.eta + grating_table_index(o, y0 + 1, x0)));
  const Real e11 = Real(load_ro(t.eta + grating_table_index(o, y0 + 1, x0 + 1)));
  const Real a = e00 + (e01 - e00) * fx;
  const Real b = e10 + (e11 - e10) * fx;
  return a + (b - a) * fy;
}

AR_NS_END
