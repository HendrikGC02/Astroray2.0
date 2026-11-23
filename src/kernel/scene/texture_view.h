// SPDX-License-Identifier: Apache-2.0
// The kernel-visible image table: one view per image, texels as stored (8-bit bytes with their
// encoding tag, or half and float components, which are always linear), so the filter (061)
// interpolates in the stored values and decodes afterwards. The host fills it from the image
// store (host arrays in the ledger textures category); the CUDA upload is a later step. One
// definition for the host, the CPU stages and the CUDA stages.
// Ref: docs/decisions/0005-material-model.md item 34; 0007 d13.
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../portability.h"

AR_NS_BEGIN

// How the stored bytes map to scene-linear values. SRGB bytes are decoded by the sRGB transfer
// function after filtering; LINEAR bytes are value/255; DATA is value/255 with no transform and
// no spectral lift.
enum class TexEncoding : uint32_t { LINEAR = 0, SRGB = 1, DATA = 2 };

// The component type of the stored texels. U8 is 0 so a zero-initialised view stays 8-bit.
enum class TexFormat : uint32_t { U8 = 0, HALF = 1, FLOAT = 2 };

struct DeviceImageView {
  uint64_t texels;  // address of width * height * channels components, rows bottom-up
  uint32_t width, height;
  uint32_t channels;  // 1, 3 or 4, as stored
  uint32_t encoding;  // TexEncoding; half and float images ignore it (they are linear)
  uint32_t format;    // TexFormat
  uint32_t reserved;  // zero
};
static_assert(sizeof(DeviceImageView) == 32, "device layout");

AR_FN uint32_t image_component_bytes(uint32_t format) {
  return format == uint32_t(TexFormat::FLOAT) ? 4u : (format == uint32_t(TexFormat::HALF) ? 2u : 1u);
}

// Bytes from one texel to the next.
AR_FN uint64_t image_texel_stride(const DeviceImageView& v) { return uint64_t(v.channels) * image_component_bytes(v.format); }

// The first byte of texel (x, y); y = 0 is the bottom row. Callers keep x < width, y < height.
AR_FN const uint8_t* image_texel(const DeviceImageView& v, uint32_t x, uint32_t y) {
  AR_ASSERT(x < v.width && y < v.height);
  return reinterpret_cast<const uint8_t*>(uintptr_t(v.texels)) + (uint64_t(y) * v.width + x) * image_texel_stride(v);
}

// IEEE 754 binary16 widened to binary32 exactly, so the CPU and the GPU return the same bits
// (zero, subnormals, infinities and NaNs included; a NaN keeps its payload shifted up). Integer
// arithmetic plus one float subtraction whose operands and result are exact normal floats (the
// kernels keep denormals, -ftz=false); never a target conversion intrinsic. No branches: a
// branchy or loop-based integer normalisation costs 0.4 s of shade_surface compile (B-05).
// Ref: IEEE 754-2008 section 3.6 (binary16: 1 sign, 5 exponent bits with bias 15, 10 fraction
// bits); the branchless widening of Giesen, "Fast half float conversions" (2012).
AR_FN float half_to_float(uint16_t h) {
  uint32_t o = (uint32_t(h) & 0x7fffu) << 13;  // exponent and fraction in float position
  const uint32_t e = o & 0x0f800000u;
  o += 0x38000000u;                             // rebias 15 -> 127
  o += e == 0x0f800000u ? 0x38000000u : 0u;     // Inf and NaN: exponent 31 must land on 255
  // Zero and subnormals: o + one exponent step is 2^-14 * (1 + frac * 2^-10); taking 2^-14 away
  // leaves frac * 2^-24 exactly (both operands and the result are normal floats).
  const float sub = bit_cast<float>(o + 0x00800000u) - bit_cast<float>(0x38800000u);
  return bit_cast<float>((e == 0u ? bit_cast<uint32_t>(sub) : o) | ((uint32_t(h) & 0x8000u) << 16));
}

AR_FN float load_component_wide(const uint8_t* p, uint32_t i, bool half) {
  return half ? half_to_float(load_ro(reinterpret_cast<const uint16_t*>(p) + i)) : load_ro(reinterpret_cast<const float*>(p) + i);
}

AR_FN Texel load_texel_wide(const uint8_t* p, uint32_t channels, uint32_t format) {
  const bool half = format == uint32_t(TexFormat::HALF);
  const bool grey = channels == 1u;
  Texel t;
  t.v[0] = load_component_wide(p, 0u, half);
  t.v[1] = load_component_wide(p, grey ? 0u : 1u, half);
  t.v[2] = load_component_wide(p, grey ? 0u : 2u, half);
  t.v[3] = channels == 4u ? load_component_wide(p, 3u, half) : 1.0f;
  return t;
}

AR_NS_END
