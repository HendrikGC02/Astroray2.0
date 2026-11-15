// SPDX-License-Identifier: Apache-2.0
// The sRGB transfer function, encoded value in [0, 1] to linear light: the one definition, used
// by TEX_IMAGE after filtering and by the host image store for the image mean. It is evaluated
// in double and rounded once to float, so the CPU and GPU builds agree bit for bit on the 256
// encoded byte values a Closest lookup can produce (single-precision powf differs between the
// CUDA and MSVC libraries by an ulp or two; a double result rounded to float does not).
// Ref: IEC 61966-2-1:1999 (sRGB), the piecewise decoding function.
#pragma once

#include "../core/real.h"
#include "../portability.h"

AR_NS_BEGIN

AR_FN double srgb_to_linear(double c) {
  return c <= double(0.04045) ? c / double(12.92) : ar::pow((c + double(0.055)) / double(1.055), double(2.4));
}

AR_FN float srgb_to_linear(float c) { return float(srgb_to_linear(double(c))); }

AR_NS_END
