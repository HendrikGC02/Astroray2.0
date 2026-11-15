// SPDX-License-Identifier: Apache-2.0
// Ref: docs/decisions/0008-film-outputs-denoise.md d3; host/film/active_pixels.h.
#include "host/film/active_pixels.h"

#include <stdexcept>
#include <string>

namespace astroray::host {

ActivePixels::ActivePixels(Device& device, uint32_t max_width, uint32_t max_height, Owner owner)
    : device_(device), capacity_(max_width * max_height) {
  if (max_width == 0 || max_height == 0) throw std::invalid_argument("active pixels: zero film size");
  buffer_ = device.allocate(LedgerCategory::FILM, uint64_t(capacity_) * sizeof(uint32_t), owner);
}

ActivePixels::~ActivePixels() { device_.free(buffer_); }

uint32_t ActivePixels::plan(const ar::FilmData& film) {
  const uint64_t u = ar::film_build_active_count(film);
  if (u > capacity_)
    throw std::length_error("active pixels: the render rectangle has " + std::to_string(u) +
                            " pixels, the list holds " + std::to_string(capacity_));
  count_ = uint32_t(u);
  return count_;
}

}  // namespace astroray::host
