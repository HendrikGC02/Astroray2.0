// SPDX-License-Identifier: Apache-2.0
// The kernel-visible 8-bit image table: one view per image, texel bytes as stored with their
// encoding tag, so the filter (061) interpolates in the stored encoding and decodes afterwards.
// The host fills it from the image store (host arrays in the ledger textures category); the
// CUDA upload is task 102. One definition for the host, the CPU stages and the CUDA stages.
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

struct DeviceImageView {
  uint64_t texels;  // address of width * height * channels bytes, rows bottom-up
  uint32_t width, height;
  uint32_t channels;  // 1, 3 or 4, as stored
  uint32_t encoding;  // TexEncoding
};
static_assert(sizeof(DeviceImageView) == 24, "device layout");

// The first byte of texel (x, y); y = 0 is the bottom row. Callers keep x < width, y < height.
AR_FN const uint8_t* image_texel(const DeviceImageView& v, uint32_t x, uint32_t y) {
  AR_ASSERT(x < v.width && y < v.height);
  return reinterpret_cast<const uint8_t*>(uintptr_t(v.texels)) + (uint64_t(y) * v.width + x) * v.channels;
}

AR_NS_END
