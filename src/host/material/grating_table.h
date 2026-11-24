// SPDX-License-Identifier: Apache-2.0
// Host builder of the grating efficiency table (0005 item 24): a sinusoidal groove profile by
// scalar thin-phase theory. The generator inputs (depth, coating) never reach the device; the
// result is the table kernel/material/ext/grating_table.h reads. Placing it in the TableArena is
// the routing task's.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace astroray::host {

struct GratingTableSpec {
  double pitch_nm = 0.0;
  double depth_nm = 0.0;  // peak-to-peak groove depth h
  int m_min = 0, m_max = 0;
  bool transmit = false;
  // Coating reflectance (reflection) or substrate transmittance (transmission) at (lambda_nm,
  // cos_i): finite and >= 0. A value above 1 is not refused; the cell clip catches its sum.
  std::function<double(double, double)> coating;
  // Substrate index at lambda_nm; transmission only.
  std::function<double(double)> substrate_ior;
};

struct GratingTableResult {
  std::vector<float> eta;  // [order][cos node][wavelength], grating_table.h's layout
  int32_t m_min = 0;
  int32_t order_count = 0;
  uint32_t clipped_cells = 0;
  // AR_REPORT_GRATING_TABLE_LIMIT and its detail when cells were clipped, else 0 and empty. Kept
  // free of kernel types so the double-precision test build can link against it.
  uint32_t report_code = 0;
  std::string report_detail;
};

// Peak-to-peak phase modulation of the profile at (lambda_nm, cos_i); n_sub is read in
// transmission only.
double grating_phase_pp(bool transmit, double depth_nm, double lambda_nm, double cos_i, double n_sub);

// Throws std::runtime_error for invalid inputs and for a profile outside scalar theory.
GratingTableResult build_grating_table(const GratingTableSpec& spec);

}  // namespace astroray::host
