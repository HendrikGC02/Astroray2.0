// SPDX-License-Identifier: Apache-2.0
// Host-side construction of the film-wavelength table (0003 d3). Built in double once
// per render epoch and immutable afterwards; the kernel reads it through
// ar::WavelengthTableView (kernel/spectral/wavelength.h), which the caller fills from
// edges, cdf and bins().
#pragma once
#include <cstdint>
#include <vector>

namespace astroray::host {

struct WavelengthTable {
  std::vector<float> edges;  // bins() + 1 bin edges in nm, strictly increasing
  std::vector<float> cdf;    // bins() + 1 values, cdf[0] = 0, cdf.back() = 1, strictly increasing
  uint32_t bins() const { return static_cast<uint32_t>(edges.size()) - 1u; }
};

// A line emitter's contribution to the table. The contents arrive with the line
// components; only the entry point exists now.
struct LineComponent {
  double lambda_nm;
  double weight;
};

class WavelengthTableBuilder {
 public:
  // An empty list is accepted and changes nothing. A non-empty list throws
  // std::invalid_argument: line components are not built yet, and dropping them silently
  // would leave the lamp's lines unsampled.
  void add_lines(const std::vector<LineComponent>& lines);

  // Support [360, 830] nm (display channels only), `visible` component on 1024 uniform
  // bins, 5% uniform floor. Throws std::runtime_error if any bin has zero pdf in float.
  WavelengthTable build() const;
};

}  // namespace astroray::host
