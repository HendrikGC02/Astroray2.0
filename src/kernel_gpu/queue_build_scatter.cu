// SPDX-License-Identifier: Apache-2.0
// queue_build_scatter: the CUDA kernel of stages/queue_build.h, one tile per thread.
// Ref: docs/decisions/0002-kernel-architecture.md D4, D6.
#include "stages/queue_build.h"

extern "C" __global__ void AR_LAUNCH_BOUNDS(ar::kLaunch_queue_build_scatter.threads,
                                            ar::kLaunch_queue_build_scatter.min_blocks)
    k_queue_build_scatter(const ar::KernelGlobals* __restrict__ kg) {
  const ar::KernelGlobals& g = *kg;
  const uint32_t tiles = ar::queue_tile_count(g);
  for (uint32_t t = blockIdx.x * blockDim.x + threadIdx.x; t < tiles; t += gridDim.x * blockDim.x)
    ar::queue_scatter_tile(g, t);
}
