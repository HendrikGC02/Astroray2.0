// SPDX-License-Identifier: Apache-2.0
// Filter functions, width mapping and CDF construction are copied from Cycles
// intern/cycles/scene/film.cpp (filter_func_box, filter_func_gaussian,
// filter_func_blackman_harris, filter_table) and src/util/math_cdf.h
// (util_cdf_evaluate): SPDX-FileCopyrightText: 2011-2022 Blender Foundation,
// Apache-2.0. Changed: the CDF is inverted by inverse_symmetric() below instead
// of util_cdf_invert's symmetric branch, and a density table is added.
//
// Why the inversion differs: util_cdf_invert (make_symmetric) takes
// upper_bound without the "- 1", places entry 511 + i at u = (511 + i) / 1023
// while its position is the quantile of u = 0.5 + i / 1022, and never writes
// the last entry (left at 0). That moves about 1/1023 of the probability mass
// into the extreme tail; a chi-squared test of 2^20 offsets against the filter
// density fails by orders of magnitude (measured on a float32 numpy copy of
// util_cdf_invert). Inverse-transform sampling by linear interpolation of the
// tabulated CDF at u_j = j / (size - 1) (Ernst et al. 2006) is used instead.
#include "host/film/filter_table.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace astroray::host {

namespace {

constexpr float kTwoPi = 6.283185307179586476925286766559f;  // Cycles M_2PI_F

float filter_func_box(float /*v*/, float /*width*/) { return 1.0f; }

float filter_func_gaussian(float v, float width) {
  v *= 6.0f / width;
  return expf(-2.0f * v * v);
}

float filter_func_blackman_harris(float v, float width) {
  v = kTwoPi * (v / width + 0.5f);
  return 0.35875f - 0.48829f * cosf(v) + 0.14128f * cosf(2.0f * v) - 0.01168f * cosf(3.0f * v);
}

// util_cdf_evaluate over [0, range] with kFilterTableSize - 1 intervals: the
// function is sampled at kFilterTableSize - 1 points, so the CDF has
// kFilterTableSize entries, cdf[0] = 0 and cdf.back() = 1.
std::vector<float> cdf_evaluate(float (*func)(float, float), float width, float range) {
  const int resolution = ar::kFilterTableSize - 1;
  std::vector<float> cdf(static_cast<size_t>(resolution) + 1);
  cdf[0] = 0.0f;
  for (int i = 0; i < resolution; ++i) {
    const float x = 0.0f + range * (float)i / (float)(resolution - 1);
    cdf[static_cast<size_t>(i) + 1] = cdf[static_cast<size_t>(i)] + fabsf(func(x, width));
  }
  const float sum = cdf[static_cast<size_t>(resolution)];
  const float inv_sum = sum > 0.0f ? 1.0f / sum : 0.0f;
  for (float& c : cdf) c *= inv_sum;
  cdf[static_cast<size_t>(resolution)] = 1.0f;
  return cdf;
}

// Offsets from the pixel corner for u_j = j / (size - 1). The half CDF is
// inverted at s = |2 u - 1| and mirrored; position index / (size - 1) of the
// CDF entry is Cycles' own convention (util_cdf_invert, y = (index + t) /
// (resolution - 1) * 2 range).
void inverse_symmetric(const std::vector<float>& cdf, float range, float* table) {
  const int n = ar::kFilterTableSize;
  const int cdf_size = static_cast<int>(cdf.size());
  for (int j = 0; j < n; ++j) {
    const float u = (float)j / (float)(n - 1);
    const float s = fabsf(2.0f * u - 1.0f);
    int index = static_cast<int>(std::upper_bound(cdf.begin(), cdf.end(), s) - cdf.begin()) - 1;
    float t;
    if (index < cdf_size - 1) {
      t = (s - cdf[static_cast<size_t>(index)]) /
          (cdf[static_cast<size_t>(index) + 1] - cdf[static_cast<size_t>(index)]);
    } else {
      t = 0.0f;
      index = cdf_size - 1;
    }
    const float y = ((index + t) / (float)(n - 1)) * (2.0f * range);
    table[j] = j < n / 2 ? 0.5f * (1.0f - y) : 0.5f * (1.0f + y);
  }
}

}  // namespace

ar::FilterTable build_filter_table(const FilterSettings& settings) {
  float width = settings.width;
  if (!(width > 0.0f) || !std::isfinite(width))
    throw std::invalid_argument("pixel filter width must be a positive finite number of pixels");
  float (*func)(float, float) = nullptr;
  switch (settings.type) {
    case FilterType::Box:
      func = filter_func_box;
      break;
    case FilterType::Gaussian:
      func = filter_func_gaussian;
      width *= 3.0f;
      break;
    case FilterType::BlackmanHarris:
      func = filter_func_blackman_harris;
      width *= 2.0f;
      break;
  }
  if (func == nullptr) throw std::invalid_argument("unknown pixel filter type");

  ar::FilterTable table;
  const float range = width * 0.5f;
  const std::vector<float> cdf = cdf_evaluate(func, width, range);
  inverse_symmetric(cdf, range, table.inv_cdf);

  // The density table is the same function on [0, range] scaled to unit
  // integral over [-range, range] (trapezoid rule, double).
  const int n = ar::kFilterDensitySize;
  const double h = static_cast<double>(range) / (n - 1);
  double values[ar::kFilterDensitySize];
  double trapezoid = 0.0;
  for (int i = 0; i < n; ++i) {
    values[i] = static_cast<double>(func(range * (float)i / (float)(n - 1), width));
    trapezoid += (i == 0 || i == n - 1 ? 0.5 : 1.0) * values[i] * h;
  }
  for (int i = 0; i < n; ++i) table.density[i] = static_cast<float>(values[i] / (2.0 * trapezoid));
  table.radius = range;
  return table;
}

}  // namespace astroray::host
