// SPDX-License-Identifier: Apache-2.0
// queue_build_scan: the CUDA kernel of stages/queue_build.h, one scan tile per warp, a lane per
// bin, grid-stride over the scan tiles.
// A scan tile waits only on scan tiles of lower index. The host launches queue_scan_grid units,
// one warp per scan tile: at most 192 warps (24 blocks) for the largest pools (2^21 + 2^22 items,
// 0002 D5), all resident at once, so the lowest unfinished scan tile always runs and the
// look-back cannot deadlock. Lanes of one warp may wait on each other (the finishing lane 0 on
// the other bins): independent thread scheduling (sm_70 and later) lets each make progress.
// Ref: Merrill and Garland 2016, NVR-2016-002, sections 4-5; docs/decisions/0002-kernel-architecture.md
// D4, D6, D7 remedy (c).
#include "stages/queue_build.h"

static_assert(ar::kLaunch_k_queue_build_scan.threads % ar::kQueueScanLanes == 0, "a block holds whole warps");

extern "C" __global__ void AR_LAUNCH_BOUNDS(ar::kLaunch_k_queue_build_scan.threads,
                                            ar::kLaunch_k_queue_build_scan.min_blocks)
    k_queue_build_scan(const ar::KernelGlobals* __restrict__ kg) {
  const ar::KernelGlobals& g = *kg;
  const uint32_t thread = blockIdx.x * blockDim.x + threadIdx.x;
  const uint32_t lane = threadIdx.x % ar::kQueueScanLanes;
  const uint32_t warps = gridDim.x * blockDim.x / ar::kQueueScanLanes;
  const uint32_t units = ar::queue_scan_tiles(ar::queue_tile_count(g));
  for (uint32_t u = thread / ar::kQueueScanLanes; u < units; u += warps)
    ar::queue_scan_tile(g, u, lane, ar::kQueueScanLanes);
}
