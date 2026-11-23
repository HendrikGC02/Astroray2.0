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

// IEEE 754 binary16 widened to binary32 by integer arithmetic only, so the CPU and the GPU return
// the same bits (zero, subnormals, infinities and NaNs included; a NaN keeps its payload shifted
// up). Never a target conversion intrinsic.
// Ref: IEEE 754-2008 section 3.6 (binary16: 1 sign, 5 exponent bits with bias 15, 10 fraction bits).
AR_FN float half_to_float(uint16_t h) {
  const uint32_t sign = (uint32_t(h) & 0x8000u) << 16;
  uint32_t e = (uint32_t(h) >> 10) & 0x1fu;
  uint32_t m = uint32_t(h) & 0x3ffu;
  uint32_t bits;
  if (e == 0x1fu) {
    bits = sign | 0x7f800000u | (m << 13);
  } else if (e != 0u) {
    bits = sign | ((e + 112u) << 23) | (m << 13);  // rebias 15 -> 127
  } else if (m == 0u) {
    bits = sign;
  } else {
    // A subnormal is m * 2^-24: shift the leading one up to bit 10 and lower the exponent per shift.
    int exp = 1;
    while ((m & 0x400u) == 0u) {
      m <<= 1;
      --exp;
    }
    bits = sign | (uint32_t(exp + 112) << 23) | ((m & 0x3ffu) << 13);
  }
  return bit_cast<float>(bits);
}

// One half or float texel as RGBA floats, raw: grey expands to (g, g, g, 1) and RGB gets alpha 1,
// as load_texel does for bytes. `p` is the texel's first byte (image_texel); stored components are
// naturally aligned (every image buffer starts on a 16-byte boundary).
AR_FN Texel load_texel_wide(const uint8_t* p, uint32_t channels, uint32_t format) {
  const bool half = format == uint32_t(TexFormat::HALF);
  const uint16_t* h = reinterpret_cast<const uint16_t*>(p);
  const float* f = reinterpret_cast<const float*>(p);
  Texel t;
  const uint32_t n = channels == 1u ? 1u : (channels == 4u ? 4u : 3u);
  for (uint32_t c = 0; c < n; ++c) t.v[c] = half ? half_to_float(load_ro(h + c)) : load_ro(f + c);
  if (n == 1u) t.v[1] = t.v[2] = t.v[0];
  if (n != 4u) t.v[3] = 1.0f;
  return t;
}

AR_NS_END
