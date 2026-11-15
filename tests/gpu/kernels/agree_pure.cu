// SPDX-License-Identifier: Apache-2.0
// Test-only module (never embedded): the pure functions of the agreement harness, run on
// host-generated inputs and built twice, no-FMA and production (027). The functions are the
// production headers' own, so the harness measures them and not a copy.
// Ref: docs/decisions/0002-kernel-architecture.md D12; 0009 d17.
#include "../agree.h"
#include "color/rgb2spec.h"
#include "spectral/planck.h"

// Inputs per row: planck (lambda nm x4, T K), sigmoid (rgb x3, lambda nm x4), cancel (a, b, c, d
// x4 each). Outputs: 4 lanes per input.
extern "C" __global__ void ar_agree_planck(const float* in, float* out, uint32_t n) {
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  const float* p = in + 5u * i;
  for (int j = 0; j < 4; ++j) out[4u * i + j] = ar::planck_radiance(p[j], p[4]);
}

// The Jakob-Hanika table arrives as its two device arrays and its resolution.
extern "C" __global__ void ar_agree_sigmoid(const float* in, float* out, uint32_t n, const float* z_nodes,
                                            const float* coeffs, int32_t res) {
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  const float* p = in + 7u * i;
  ar::Rgb2SpecTable t = {};
  t.res = res;
  t.z_nodes = z_nodes;
  t.coeffs = coeffs;
  const ar::SigmoidCoeffs c = ar::fetch_coeffs(t, ar::Rgb{{p[0], p[1], p[2]}});
  for (int j = 0; j < 4; ++j) out[4u * i + j] = ar::sigmoid_eval(c, p[3 + j]);
}

extern "C" __global__ void ar_agree_cancel(const float* in, float* out, uint32_t n) {
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  const float* p = in + 16u * i;
  for (int j = 0; j < 4; ++j) out[4u * i + j] = ar::agree_cancel(p[j], p[4 + j], p[8 + j], p[12 + j]);
}
