// SPDX-License-Identifier: Apache-2.0
// Ref: docs/decisions/0008-film-outputs-denoise.md d7; 0003 d11 (Y_w = Y_n), d13 (h and c exact,
// folded in double here).
#include "host/film/response_table.h"

#include <cmath>
#include <stdexcept>
#include <string>

namespace astroray::host {

namespace {

// Ref: CODATA 2018 (exact since the 2019 SI redefinition), Tiesinga et al., Rev. Mod. Phys. 93,
// 025010 (2021): h in J s, c in m/s.
constexpr double kPlanckH = 6.62607015e-34;
constexpr double kSpeedOfLight = 299792458.0;
constexpr double kMetresPerNm = 1.0e-9;

static_assert(static_cast<uint32_t>(ResponseUnit::Energy) == ar::RESPONSE_ENERGY, "unit values must match the kernel");
static_assert(static_cast<uint32_t>(ResponseUnit::Photon) == ar::RESPONSE_PHOTON, "unit values must match the kernel");

}  // namespace

uint32_t add_response_channel(ResponseTable& table, const ResponseCurve& curve) {
  const size_t n = curve.nm.size();
  if (n < 2) throw std::invalid_argument("response channel needs at least two knots");
  if (curve.value.size() != n) throw std::invalid_argument("response channel: nm and value differ in size");
  if (curve.unit != ResponseUnit::Energy && curve.unit != ResponseUnit::Photon)
    throw std::invalid_argument("response channel: unknown unit");
  for (size_t i = 0; i < n; ++i) {
    if (!std::isfinite(curve.nm[i]) || !std::isfinite(curve.value[i]))
      throw std::invalid_argument("response channel: knot " + std::to_string(i) + " is not finite");
    if (curve.value[i] < 0.0f)
      throw std::invalid_argument("response channel: knot " + std::to_string(i) + " has a negative response");
    if (i > 0 && !(curve.nm[i] > curve.nm[i - 1]))
      throw std::invalid_argument("response channel: nm must be strictly ascending at knot " + std::to_string(i));
  }
  ar::ResponseChannel ch{};
  ch.first = static_cast<uint32_t>(table.knot_nm.size());
  ch.count = static_cast<uint32_t>(n);
  ch.unit = static_cast<uint32_t>(curve.unit);
  table.knot_nm.insert(table.knot_nm.end(), curve.nm.begin(), curve.nm.end());
  table.knot_value.insert(table.knot_value.end(), curve.value.begin(), curve.value.end());
  table.channels.push_back(ch);
  return static_cast<uint32_t>(table.channels.size() - 1);
}

ResponseTable make_response_table(const ObserverTables& obs) {
  ResponseTable t;
  t.photon_scale = kMetresPerNm / (kPlanckH * kSpeedOfLight);
  t.y_w = obs.y_n;
  for (int i = 0; i < 9; ++i) t.m_ws[i] = obs.m_ws[i];
  t.display_first = 0;
  for (int k = 0; k < 3; ++k) {
    ResponseCurve c;
    for (uint32_t i = 0; i < kObserverSamples; ++i) {
      c.nm.push_back(static_cast<float>(kObserverLambdaMin + i));
      c.value.push_back(obs.cmf[k][i]);
    }
    add_response_channel(t, c);
  }
  return t;
}

}  // namespace astroray::host
