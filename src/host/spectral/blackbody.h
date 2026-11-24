// SPDX-License-Identifier: Apache-2.0
// Y normalisation of a blackbody for the Blackbody node and lamp temperatures (0003 d11), and the
// one function that packs the BLACKBODY SpectrumRef both of them use.
#pragma once
#include "../../kernel/material/spectrum_ref.h"

namespace astroray::host {

// Y(Planck(T)) = sum_i Planck(lambda_i, T) ybar_i / Y_n over the observer table's 360-830 nm at
// 1 nm (0003 d11), in double; 0 when the visible radiance underflows (T of a few kelvin), which
// is a value and not an error so a caller can tell "no visible radiance" from a bad argument.
// Planck is the kernel's planck_radiance in W m^-2 sr^-1 nm^-1. Throws std::invalid_argument for
// a temperature that is not finite and positive, and std::runtime_error if the embedded
// observer data is missing or corrupt.
double blackbody_visible_y(double temperature_kelvin);

// The scale s = 1 / Y(Planck(T)), so Y(s Planck(T)) = 1 (0003 d11). Throws as
// blackbody_visible_y, and std::invalid_argument when Y is 0 or so small that 1 / Y is not a double
// (no visible radiance).
double blackbody_y_normalised(double temperature_kelvin);

// The ILLUMINANT ref {BLACKBODY, p[0] = T, p[1] = scale x y_norm} (0005 item 9); y_norm is
// blackbody_y_normalised(T), or 1 where the caller has decided a lamp cannot be normalised.
// It resolves analytically, with no illuminant multiply (0005 item 39). Throws
// std::invalid_argument when the stored scale, or the brightest band value scale x y_norm x
// `strength` x Planck at the Wien wavelength clamped to 360-830 nm, would overflow a float,
// so the kernel never emits inf or NaN.
ar::SpectrumRef make_blackbody_ref(double temperature_kelvin, double scale, double y_norm, double strength);

}  // namespace astroray::host
