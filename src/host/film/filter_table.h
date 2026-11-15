// SPDX-License-Identifier: Apache-2.0
// Host-side construction of the pixel-filter tables (0008 d5).
#pragma once
#include <cstdint>

#include "kernel/film/filter.h"

namespace astroray::host {

enum class FilterType : uint32_t { Box = 0, Gaussian = 1, BlackmanHarris = 2 };

// Blender's defaults: Blackman-Harris at 1.5 px.
struct FilterSettings {
  FilterType type = FilterType::BlackmanHarris;
  float width = 1.5f;  // Blender's filter_width, in pixels
};

// Throws std::invalid_argument for an unknown type or a width that is not a
// positive finite number. Uploaded once per settings change.
ar::FilterTable build_filter_table(const FilterSettings& settings);

}  // namespace astroray::host
