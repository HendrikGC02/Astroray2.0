// SPDX-License-Identifier: Apache-2.0
// The film wavelengths of a camera sample, recomputed from its key and never stored. The one
// function both generate_camera (lambda_t = lambda_f at path start) and film_resolve_pixel (the
// weights and the conversion) call, so they cannot disagree.
// Ref: docs/decisions/0003-spectral-model-and-path-state.md d2, d23 (dimension 5 of stream 0).
#pragma once
#include <cstdint>

#include "../kernel_globals.h"
#include "../portability.h"
#include "../sample/dims.h"
#include "../sample/sobol.h"
#include "../spectral/wavelength.h"

AR_NS_BEGIN

// `pixel` is the pixel's plane index (camera_key's id) and `index` the sample index n + m.
AR_FN FilmWavelengths camera_film_wavelengths(const SessionUniforms& s, const WavelengthTableView& table,
                                              uint32_t pixel, uint32_t index) {
  return sample_film_wavelengths(table, draw(s.sampler_mode, pixel, index, STREAM_FIXED, DIM_CAM_WAVELENGTH, s.seed));
}

AR_NS_END
