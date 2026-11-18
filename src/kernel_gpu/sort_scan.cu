// SPDX-License-Identifier: Apache-2.0
// sort_scan: the CUDA kernel of stages/sort.h, one scan tile per warp, a lane per material bin
// (each lane takes every kQueueLanes-th bin), grid-stride over the scan tiles.
// A scan tile waits only on scan tiles of lower index. The segment is at most the main pool
// (2^21 items, 0002 D5): 2,048 histogram rows, 64 scan tiles, 64 warps (8 blocks), all resident
// at once, so the lowest unfinished scan tile always runs and the look-back cannot deadlock.
// Ref: Merrill and Garland 2016, NVR-2016-002, sections 4-5; docs/decisions/0002-kernel-architecture.md
// D4, D7 step 3, D7 remedy (c); stages/queue_build.h (scan_rows_tile).
#include "stages/sort.h"

static_assert(ar::kLaunch_k_sort_scan.threads % ar::kQueueLanes == 0, "a block holds whole warps");

extern "C" __global__ void AR_LAUNCH_BOUNDS(ar::kLaunch_k_sort_scan.threads, ar::kLaunch_k_sort_scan.min_blocks)
    k_sort_scan(const ar::KernelGlobals* __restrict__ kg) {
  const ar::KernelGlobals& g = *kg;
  const uint32_t thread = blockIdx.x * blockDim.x + threadIdx.x;
  const uint32_t lane = threadIdx.x % ar::kQueueLanes;
  const uint32_t warps = gridDim.x * blockDim.x / ar::kQueueLanes;
  const uint32_t units = ar::queue_scan_tiles(ar::sort_tile_count(g));
  for (uint32_t u = thread / ar::kQueueLanes; u < units; u += warps) ar::sort_scan_tile(g, u, lane, ar::kQueueLanes);
}
