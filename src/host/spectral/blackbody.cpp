// SPDX-License-Identifier: Apache-2.0
// Ref: docs/decisions/0003-spectral-model-and-path-state.md d11 (Y definition, Y_n from the
// shipped CMF table), d25 (Planck).
#include "host/spectral/blackbody.h"

#include <cmath>
#include <stdexcept>
#include <string>

#include "host/data_resource.h"
#include "kernel/spectral/planck.h"

namespace astroray::host {

double blackbody_y_normalised(double temperature_kelvin) {
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
  if (!(y > 0.0) || !std::isfinite(y))
    throw std::invalid_argument("blackbody_y_normalised: no visible radiance at T = " + std::to_string(temperature_kelvin) + " K");
  return 1.0 / y;
}

}  // namespace astroray::host
