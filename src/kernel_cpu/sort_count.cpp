// SPDX-License-Identifier: Apache-2.0
// sort_count on the host thread pool.
// Ref: docs/decisions/0002-kernel-architecture.md D6 (block histogram); stages/sort.h.
#include <type_traits>

#include "../kernel/stages/sort.h"
#include "stage_cpu.h"

static_assert(std::is_same_v<decltype(&cpu_sort_count), CpuKernelFn>, "registered as a CpuKernelFn");

// The chunk [first, last) is a range of virtual items (sort_grid); a tile belongs to the
// chunk that holds its first virtual item, so every tile runs once whatever the chunking.
void cpu_sort_count(const ar::KernelGlobals& kg, uint32_t first, uint32_t last) {
  const uint32_t tiles = ar::sort_tile_count(kg);
  const uint32_t from = (first + ar::kQueueTile - 1) / ar::kQueueTile;
  uint32_t to = (last + ar::kQueueTile - 1) / ar::kQueueTile;
  if (to > tiles) to = tiles;
  for (uint32_t g = from; g < to; ++g) ar::sort_count_tile(kg, g);
}
