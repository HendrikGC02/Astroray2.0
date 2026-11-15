// SPDX-License-Identifier: Apache-2.0
// sort_scatter: the CUDA kernel of stages/sort.h, one tile per thread.
// Ref: docs/decisions/0002-kernel-architecture.md D4, D6.
#include "stages/sort.h"

extern "C" __global__ void AR_LAUNCH_BOUNDS(ar::kLaunch_k_sort_scatter.threads,
                                            ar::kLaunch_k_sort_scatter.min_blocks)
    k_sort_scatter(const ar::KernelGlobals* __restrict__ kg) {
  const ar::KernelGlobals& g = *kg;
  const uint32_t tiles = ar::sort_tile_count(g);
  for (uint32_t t = blockIdx.x * blockDim.x + threadIdx.x; t < tiles; t += gridDim.x * blockDim.x)
    ar::sort_scatter_tile(g, t);
}
