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
// EXR channels of UINT type and half or float images holding a texel the wide-texel rule below
// refuses are refused. Throws only std::bad_alloc.
bool decode_image_bytes(const uint8_t* bytes, uint64_t count, DecodedImage* out, std::string* error);

// The EXR half of decode_image_bytes (decode_exr.cpp); `bytes` starts with the EXR magic. It does
// not apply the wide-texel rule: decode_image_bytes does, for every format it decodes.
bool decode_exr_bytes(const uint8_t* bytes, uint64_t count, DecodedImage* out, std::string* error);

// The wide-texel rule: a half or float texel is refused when it is not finite or its magnitude is
// above kMaxTexelMagnitude. The limit is the largest float for which the Linear filter and the
// RGB lift stay finite: Linear sums four weighted texels whose float weights add to at most
// (1 + u)^3 <= 1 + 3.1u (u = 2^-24), each product and each of the three additions rounds once more, so
// |sum| <= |t|max (1 + 8u); the lift forms scale = 2 max(r, g, b) (0005 item 10), so
// 2 |t|max (1 + 8u) <= FLT_MAX. The largest float satisfying that conservative bound is bit pattern 0x7efffff7.
// Nothing is clamped. Half texels are at most 65504, so for them only non-finite values are
// refused. One definition for FILE, MEMORY and PIXELS images.
// Ref: src/kernel/vm/ops/tex_image.h tex_linear; src/kernel/color/rgb2spec.h (scale = 2m);
// IEEE 754-2008 section 4.3 (round to nearest: relative error at most u = 2^-24 per operation).
inline constexpr float kMaxTexelMagnitude = 0x1.ffffeep+126f;

struct BadTexel {
  uint64_t index = 0;    // texel number in storage order (bottom row first)
  uint32_t channel = 0;  // component within the texel
  float value = 0;       // the offending value as a float
};

// The first texel (storage order, then channel) that is not finite or, for float texels, whose
// magnitude is above kMaxTexelMagnitude. `texels` holds texel_count * channels components of
// `format`; 8-bit texels are never refused.
bool find_bad_texel(const uint8_t* texels, ar::TexFormat format, uint32_t channels, uint64_t texel_count, BadTexel* out);

// The report detail for a texel find_bad_texel returned; channels names the layout (1 is grey).
std::string bad_texel_message(const BadTexel& t, uint32_t width, uint32_t channels);

}  // namespace astroray::host
