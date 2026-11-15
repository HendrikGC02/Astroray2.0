// SPDX-License-Identifier: Apache-2.0
// Y normalisation of a blackbody for the Blackbody node and lamp temperatures (0003 d11).
#pragma once

namespace astroray::host {

// The scale s = 1 / Y(Planck(T)) with Y(L) = sum_i L(lambda_i) ybar_i / Y_n over the observer
// table's 360-830 nm at 1 nm, so Y(s Planck(T)) = 1 (0003 d11). Planck is the kernel's
// planck_radiance in W m^-2 sr^-1 nm^-1, evaluated in double. Throws std::invalid_argument
// for a temperature that is not finite and positive or whose visible radiance underflows
// (Y = 0), and std::runtime_error if the embedded observer data is missing or corrupt.
double blackbody_y_normalised(double temperature_kelvin);

}  // namespace astroray::host
