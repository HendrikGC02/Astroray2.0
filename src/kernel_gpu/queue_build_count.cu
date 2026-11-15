// SPDX-License-Identifier: Apache-2.0
// queue_build_count: the CUDA kernel of stages/queue_build.h, one tile per thread.
// Ref: docs/decisions/0002-kernel-architecture.md D4 (one KernelGlobals pointer, grid-stride
// over the device-read count), D6.
#include "stages/queue_build.h"

extern "C" __global__ void AR_LAUNCH_BOUNDS(ar::kLaunch_queue_build_count.threads,
                                            ar::kLaunch_queue_build_count.min_blocks)
    k_queue_build_count(const ar::KernelGlobals* __restrict__ kg) {
  const ar::KernelGlobals& g = *kg;
  const uint32_t tiles = ar::queue_tile_count(g);
  for (uint32_t t = blockIdx.x * blockDim.x + threadIdx.x; t < tiles; t += gridDim.x * blockDim.x)
    ar::queue_count_tile(g, t);
}
