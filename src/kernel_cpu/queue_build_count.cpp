// SPDX-License-Identifier: Apache-2.0
// queue_build_count on the host thread pool.
// Ref: docs/decisions/0002-kernel-architecture.md D6 (block histogram); stages/queue_build.h.
#include <type_traits>

#include "../kernel/stages/queue_build.h"
#include "stage_cpu.h"

static_assert(std::is_same_v<decltype(&cpu_queue_build_count), CpuKernelFn>, "registered as a CpuKernelFn");

// The chunk [first, last) is a range of virtual items (queue_build_grid); a tile belongs to the
// chunk that holds its first virtual item, so every tile runs once whatever the chunking. One
// unit runs the whole tile (lanes = 1).
void cpu_queue_build_count(const ar::KernelGlobals& kg, uint32_t first, uint32_t last) {
  const uint32_t tiles = ar::queue_tile_count(kg);
  const uint32_t from = (first + ar::kQueueTile - 1) / ar::kQueueTile;
  uint32_t to = (last + ar::kQueueTile - 1) / ar::kQueueTile;
  if (to > tiles) to = tiles;
  for (uint32_t g = from; g < to; ++g) ar::queue_count_tile(kg, g, 0, 1);
}
