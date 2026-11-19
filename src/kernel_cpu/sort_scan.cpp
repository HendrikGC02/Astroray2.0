// SPDX-License-Identifier: Apache-2.0
// sort_scan on the host thread pool.
// Ref: docs/decisions/0002-kernel-architecture.md D7 step 3, D7 remedy (c); stages/sort.h,
// stages/queue_build.h (scan_rows_tile); Blelloch 1990 (the sequential scan order).
#include <type_traits>

#include "../kernel/stages/sort.h"
#include "stage_cpu.h"

static_assert(std::is_same_v<decltype(&cpu_sort_scan), CpuKernelFn>, "registered as a CpuKernelFn");

// As queue_build_scan.cpp: the chunk that holds item 0 runs every scan tile in order, one unit
// each, so each look-back finds its predecessor's inclusive prefix at once; the rewrite of the
// rows is not repeatable, so the other chunks do nothing.
void cpu_sort_scan(const ar::KernelGlobals& kg, uint32_t first, uint32_t /*last*/) {
  if (first != 0) return;
  const uint32_t units = ar::queue_scan_tiles(ar::sort_tile_count(kg));
  for (uint32_t u = 0; u < units; ++u) ar::sort_scan_tile(kg, u, 0, 1);
}
