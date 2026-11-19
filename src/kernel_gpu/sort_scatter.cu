// SPDX-License-Identifier: Apache-2.0
// sort_scatter: the CUDA kernel of stages/sort.h, one tile per warp (kQueueLanes lanes), grid-stride
// over the tiles by warp, so every lane of a warp takes part in every match.
// Ref: docs/decisions/0002-kernel-architecture.md D4, D6.
#include "stages/sort.h"

static_assert(ar::kLaunch_k_sort_scatter.threads % ar::kQueueLanes == 0, "a block holds whole warps");

extern "C" __global__ void AR_LAUNCH_BOUNDS(ar::kLaunch_k_sort_scatter.threads,
                                            ar::kLaunch_k_sort_scatter.min_blocks)
    k_sort_scatter(const ar::KernelGlobals* __restrict__ kg) {
  const ar::KernelGlobals& g = *kg;
  const uint32_t thread = blockIdx.x * blockDim.x + threadIdx.x;
  const uint32_t lane = threadIdx.x % ar::kQueueLanes;
  const uint32_t warps = gridDim.x * blockDim.x / ar::kQueueLanes;
  const uint32_t tiles = ar::sort_tile_count(g);
  for (uint32_t t = thread / ar::kQueueLanes; t < tiles; t += warps) ar::sort_scatter_tile(g, t, lane, ar::kQueueLanes);
}
