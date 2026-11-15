// SPDX-License-Identifier: Apache-2.0
// 8-bit image decoding: PNG, JPEG, TGA and BMP from memory, through stb_image compiled in
// decode.cpp and nowhere else. Texels are kept as stored (1, 3 or 4 channels, no expansion
// except grey+alpha, which has no kernel layout) with rows bottom-up, Blender's order.
// Ref: docs/decisions/0007-blender-integration.md d13; 0001 D9.1 and R20 (stb_image only).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace astroray::host {

struct DecodedImage {
  uint32_t width = 0, height = 0, channels = 0;  // channels is 1, 3 or 4
  std::vector<uint8_t> texels;                   // width * height * channels, bottom row first
};

// False with *error set to the decoder's reason (also for 16-bit files, which this build does
// not decode: half and float images are M1b). Throws only std::bad_alloc.
bool decode_image_bytes(const uint8_t* bytes, uint64_t count, DecodedImage* out, std::string* error);

}  // namespace astroray::host
