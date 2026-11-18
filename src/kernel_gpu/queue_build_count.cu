// SPDX-License-Identifier: Apache-2.0
// queue_build_count: the CUDA kernel of stages/queue_build.h, one tile per warp (kQueueLanes
// lanes), grid-stride over the tiles by warp, so every lane of a warp votes in every round.
// Ref: docs/decisions/0002-kernel-architecture.md D4 (one KernelGlobals pointer, grid-stride
// over the device-read count), D6.
#include "stages/queue_build.h"

static_assert(ar::kLaunch_k_queue_build_count.threads % ar::kQueueLanes == 0, "a block holds whole warps");

extern "C" __global__ void AR_LAUNCH_BOUNDS(ar::kLaunch_k_queue_build_count.threads,
                                            ar::kLaunch_k_queue_build_count.min_blocks)
    k_queue_build_count(const ar::KernelGlobals* __restrict__ kg) {
  const ar::KernelGlobals& g = *kg;
  const uint32_t thread = blockIdx.x * blockDim.x + threadIdx.x;
  const uint32_t lane = threadIdx.x % ar::kQueueLanes;
  const uint32_t warps = gridDim.x * blockDim.x / ar::kQueueLanes;
  const uint32_t tiles = ar::queue_tile_count(g);
  for (uint32_t t = thread / ar::kQueueLanes; t < tiles; t += warps) ar::queue_count_tile(g, t, lane, ar::kQueueLanes);
}
