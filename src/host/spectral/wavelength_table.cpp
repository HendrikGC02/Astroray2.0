// SPDX-License-Identifier: Apache-2.0
// Ref: docs/decisions/0003-spectral-model-and-path-state.md d3 (table, support,
// components, floor); pbrt-v4 src/pbrt/util/spectrum.h VisibleWavelengthsPDF,
// pdf(l) = 0.0039398042 / cosh^2(0.0072 (l - 538)) on [360, 830] (Apache-2.0);
// Hesterberg 1995, "Weighted average importance sampling and defensive mixture
// distributions", Technometrics 37(2) (the uniform floor).
#include "host/spectral/wavelength_table.h"

#include <cmath>
#include <stdexcept>
#include <string>

namespace astroray::host {

namespace {

constexpr double kSupportMin = 360.0;  // nm; the display channels' support is the observer table's
constexpr double kSupportMax = 830.0;
constexpr uint32_t kVisibleBins = 1024;
constexpr double kFloor = 0.05;
// pbrt's constants: the pdf peaks at kCentre and falls as sech^2 with rate kRate per nm.
constexpr double kCentre = 538.0;
constexpr double kRate = 0.0072;

// Integral of sech^2(kRate (l - kCentre)) from kSupportMin to l, over the same integral to
// kSupportMax: the CDF of pbrt's pdf, which is the pdf's own normalisation (the literal
// 0.0039398042 is 1 / that integral). tanh is the antiderivative of sech^2.
double visible_cdf(double l) {
  const double lo = std::tanh(kRate * (kSupportMin - kCentre));
  const double hi = std::tanh(kRate * (kSupportMax - kCentre));
  return (std::tanh(kRate * (l - kCentre)) - lo) / (hi - lo);
}

}  // namespace

void WavelengthTableBuilder::add_lines(const std::vector<LineComponent>& lines) {
  if (!lines.empty())
    throw std::invalid_argument("WavelengthTableBuilder::add_lines: line components are not implemented yet");
}

WavelengthTable WavelengthTableBuilder::build() const {
  const uint32_t k = kVisibleBins;
  WavelengthTable t;
  t.edges.resize(k + 1);
  t.cdf.resize(k + 1);
  for (uint32_t i = 0; i <= k; ++i) {
    const double e = kSupportMin + (kSupportMax - kSupportMin) * static_cast<double>(i) / static_cast<double>(k);
    const double u = (e - kSupportMin) / (kSupportMax - kSupportMin);
    t.edges[i] = static_cast<float>(e);
    t.cdf[i] = static_cast<float>((1.0 - kFloor) * visible_cdf(e) + kFloor * u);
  }
  t.cdf[0] = 0.0f;
  t.cdf[k] = 1.0f;  // exact: the sampler's binary search relies on cdf[bins] = 1 > u
  // Every display channel responds only inside the support, so a positive pdf in every bin
  // is positive pdf wherever a channel responds.
  for (uint32_t i = 0; i < k; ++i) {
    if (!(t.edges[i + 1] > t.edges[i]) || !(t.cdf[i + 1] > t.cdf[i]))
      throw std::runtime_error("wavelength table: zero pdf in bin " + std::to_string(i));
  }
  return t;
}

}  // namespace astroray::host
