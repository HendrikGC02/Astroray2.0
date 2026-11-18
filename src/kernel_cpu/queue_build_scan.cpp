// SPDX-License-Identifier: Apache-2.0
// queue_build_scan on the host thread pool.
// Ref: docs/decisions/0002-kernel-architecture.md D6 (scan, step table), D7 remedy (c);
// stages/queue_build.h; Blelloch 1990 (the sequential scan order).
#include <type_traits>

#include "../kernel/stages/queue_build.h"
#include "stage_cpu.h"

static_assert(std::is_same_v<decltype(&cpu_queue_build_scan), CpuKernelFn>, "registered as a CpuKernelFn");

// The chunk that holds item 0 runs every scan tile in order, one unit each, so each look-back
// finds its predecessor's inclusive prefix at once. Scan tiles in concurrent chunks could wait
// on a chunk the pool has not started, and the rewrite of the rows is not repeatable, so the
// other chunks do nothing.
void cpu_queue_build_scan(const ar::KernelGlobals& kg, uint32_t first, uint32_t /*last*/) {
  if (first != 0) return;
  const uint32_t units = ar::queue_scan_tiles(ar::queue_tile_count(kg));
  for (uint32_t u = 0; u < units; ++u) ar::queue_scan_tile(kg, u, 0, 1);
}
