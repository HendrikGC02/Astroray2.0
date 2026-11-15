// SPDX-License-Identifier: Apache-2.0
// queue_build_scan: the CUDA kernel of stages/queue_build.h, one thread does the whole scan.
// Ref: docs/decisions/0002-kernel-architecture.md D4, D6.
#include "stages/queue_build.h"

extern "C" __global__ void AR_LAUNCH_BOUNDS(ar::kLaunch_k_queue_build_scan.threads,
                                            ar::kLaunch_k_queue_build_scan.min_blocks)
    k_queue_build_scan(const ar::KernelGlobals* __restrict__ kg) {
  // The column scans are sequential and a kernel has no grid-wide barrier; D7 remedy (c) fuses
  // the partition if B-19 says this is too slow.
  if (blockIdx.x == 0 && threadIdx.x == 0) ar::queue_scan(*kg);
}
