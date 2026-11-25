// SPDX-License-Identifier: Apache-2.0
// Shared by the grating table tests: the largest sum of eta over a cell's orders in a host table
// (grating_table.h's layout). Every cell of a generated table sums to at most 1 (0005 item 24).
#pragma once
#include <cmath>
#include <vector>

#include "material/ext/grating_table.h"

namespace astroray::testing {

inline double grating_max_cell_sum(const std::vector<float>& eta, int orders) {
  double worst = 0.0;
  for (int c = 0; c < ar::kGratingCosBins; ++c)
    for (int w = 0; w < ar::kGratingWavelengths; ++w) {
      double sum = 0.0;
      for (int o = 0; o < orders; ++o) sum += double(eta[ar::grating_table_index(o, c, w)]);
      worst = std::fmax(worst, sum);
    }
  return worst;
}

}  // namespace astroray::testing
