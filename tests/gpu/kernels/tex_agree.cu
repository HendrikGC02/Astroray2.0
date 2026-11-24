// SPDX-License-Identifier: Apache-2.0
// Test-only module (never embedded): one TEX_IMAGE lookup per thread through the kernel library's
// op_tex_image, built no-FMA and production (027), so the test can compare the GPU lookups with
// the CPU build of the same header.
// Ref: docs/decisions/0005-material-model.md item 34, Verification 6; 0002 D12, D13; 0009 d17.
#include "vm/interp.h"

// Input row (4 floats): u, v, slot, mode (slot and mode are small integers held exactly).
// Output row (3 floats): the colour.
constexpr uint32_t kTexAgreeIn = 4;
constexpr uint32_t kTexAgreeOut = 3;

extern "C" __global__ void ar_tex_agree(const float* in, float* out, uint32_t n, const ar::DeviceImageView* images,
                                        uint32_t image_count, const float* srgb_decode) {
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  const float* row = in + kTexAgreeIn * i;
  ar::SceneTables tables = {};
  tables.images = images;
  tables.image_count = image_count;
  tables.srgb_decode = srgb_decode;
  const float uv[3] = {row[0], row[1], 0.0f};
  ar::op_tex_image(tables, uint32_t(row[2]), uint32_t(row[3]), uv, out + kTexAgreeOut * i);
}

// One TEX_ENVIRONMENT lookup per thread: the colour before any spectral lift, so the lookup and the
// direction mapping can be compared on their own. Input row (3 floats): the direction; output row
// (3 floats): the colour. Image slot 0.
extern "C" __global__ void ar_env_agree(const float* in, float* out, uint32_t n, const ar::DeviceImageView* images,
                                        uint32_t image_count, const float* srgb_decode, uint32_t mode) {
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  ar::SceneTables tables = {};
  tables.images = images;
  tables.image_count = image_count;
  tables.srgb_decode = srgb_decode;
  ar::op_tex_environment(tables, 0u, mode, in + 3 * i, out + 3 * i);
}
