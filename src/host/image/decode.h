// SPDX-License-Identifier: Apache-2.0
// Image decoding from memory: 8-bit PNG, JPEG, TGA and BMP and Radiance .hdr through stb_image
// (compiled in decode.cpp and nowhere else), and OpenEXR through tinyexr (decode_exr.cpp).
// Texels are kept at their source precision (8-bit, half or float; 1, 3 or 4 channels, no
// expansion except grey+alpha, which has no kernel layout) with rows bottom-up, Blender's order.
// Ref: docs/decisions/0007-blender-integration.md d13; 0001 D9.1 and R20 (stb_image), R34, R54
// (tinyexr reads only).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../../kernel/scene/texture_view.h"

namespace astroray::host {

struct DecodedImage {
  uint32_t width = 0, height = 0, channels = 0;  // channels is 1, 3 or 4
  ar::TexFormat format = ar::TexFormat::U8;      // the component type of texels
  // width * height * channels components of `format` (little endian), bottom row first.
  std::vector<uint8_t> texels;
};

// False with *error set to the decoder's reason. 16-bit integer PNGs, deep and multipart EXR,
// EXR channels of UINT type and half or float images holding a NaN or Inf are refused.
// Throws only std::bad_alloc.
bool decode_image_bytes(const uint8_t* bytes, uint64_t count, DecodedImage* out, std::string* error);

// The EXR half of decode_image_bytes (decode_exr.cpp); `bytes` starts with the EXR magic.
bool decode_exr_bytes(const uint8_t* bytes, uint64_t count, DecodedImage* out, std::string* error);

}  // namespace astroray::host
