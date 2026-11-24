// SPDX-License-Identifier: Apache-2.0
// Ref: docs/decisions/0003-spectral-model-and-path-state.md d11 (Y definition, Y_n from the
// shipped CMF table), d25 (Planck).
#include "host/spectral/blackbody.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

#include "host/data_resource.h"
#include "kernel/light/light.h"

namespace astroray::host {

double blackbody_visible_y(double temperature_kelvin) {
  if (!(temperature_kelvin > 0.0) || !std::isfinite(temperature_kelvin))
    throw std::invalid_argument("blackbody_y_normalised: temperature must be finite and positive");
  DataBlob blob;
  if (!find_data("spectral/observer.bin", &blob))
    throw std::runtime_error("blackbody_y_normalised: spectral/observer.bin is not embedded in this module");
  ObserverTables obs;
  const std::string err = parse_observer(blob.data, blob.size, &obs);
  if (!err.empty()) throw std::runtime_error("blackbody_y_normalised: " + err);
  double sum = 0.0;
  for (uint32_t i = 0; i < kObserverSamples; ++i) {
    const double lambda_nm = static_cast<double>(kObserverLambdaMin) + static_cast<double>(i);
    sum += ar::planck_radiance_t<double>(lambda_nm, temperature_kelvin) * static_cast<double>(obs.cmf[1][i]);
  }
  const double y = sum / obs.y_n;
  return y > 0.0 ? y : 0.0;
}

double blackbody_y_normalised(double temperature_kelvin) {
  const double y = blackbody_visible_y(temperature_kelvin);
  if (!(y > 0.0) || !std::isfinite(y) || !std::isfinite(1.0 / y))
    throw std::invalid_argument("blackbody_y_normalised: no visible radiance at T = " + std::to_string(temperature_kelvin) + " K");
  return 1.0 / y;
}

ar::SpectrumRef make_blackbody_ref(double temperature_kelvin, double scale, double y_norm, double strength) {
  const double fmax = double(std::numeric_limits<float>::max());
  // The stored scale and the brightest band value must fit a float, else the kernel would emit inf
  // or NaN (a cold T makes 1 / Y huge; a huge scale does the same).
  const double p1 = scale * y_norm;
  const double peak = ar::blackbody_peak(temperature_kelvin, p1) * strength;
  if (!(p1 <= fmax) || !(peak <= fmax))
    throw std::invalid_argument("the scale and 1 / Y(Planck(T)) overflow a float at this temperature");
  ar::SpectrumRef r = {};
  r.kind = uint8_t(ar::SpectrumKind::BLACKBODY);
  r.semantic = uint8_t(ar::SpectrumSemantic::ILLUMINANT);
  r.p[0] = float(temperature_kelvin);
  r.p[1] = float(p1);
  return r;
}

}  // namespace astroray::host
