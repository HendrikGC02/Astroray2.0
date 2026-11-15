// SPDX-License-Identifier: Apache-2.0
// A presentable RGBA float image, the only thing a denoiser writes. It has no conversion to a
// film type and the FITS writer has no overload for it, so a denoised pixel cannot become
// science data; tests/cpu/negative_compile proves both.
// Ref: docs/decisions/0008-film-outputs-denoise.md d16, d17.
#pragma once
#include <cstdint>
#include <vector>

namespace astroray::host {

struct DisplayImage {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<float> rgba;  // 4 floats per pixel, rows bottom-up like the film
  // True only when a denoiser changed the pixels (0008 d16: a failed denoiser leaves false).
  bool denoised = false;
};

}  // namespace astroray::host
