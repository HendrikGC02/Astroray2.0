// SPDX-License-Identifier: Apache-2.0
// The sRGB transfer function, encoded value in [0, 1] to linear light: the one definition. Linear
// TEX_IMAGE lookups decode their filtered value with it; Closest lookups read the 256-entry table
// the host builds from it, because powf is not bit-identical between the CUDA and MSVC libraries
// (measured: about 4% of Closest sRGB lookups differed by an ulp) and Closest must agree bit for
// bit, while GPU kernels do no double work. The host image store builds its means from the same
// table, so a mean is the mean of exactly the values the kernel returns.
// Ref: IEC 61966-2-1:1999 (sRGB), the piecewise decoding function.
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../portability.h"

AR_NS_BEGIN

inline constexpr uint32_t kSrgbTableSize = 256;  // one entry per 8-bit code value

AR_FN float srgb_to_linear(float c) {
  return c <= 0.04045f ? c / 12.92f : ar::pow((c + 0.055f) / 1.055f, 2.4f);
}

// Host only: table[b] = srgb_to_linear(b / 255) with b / 255 as load_texel reads it.
AR_FN void srgb_decode_table(float* table) {
  for (uint32_t b = 0; b < kSrgbTableSize; ++b) table[b] = srgb_to_linear(float(b) * (1.0f / 255.0f));
}

AR_NS_END
