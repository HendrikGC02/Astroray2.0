// SPDX-License-Identifier: Apache-2.0
// sort: a stable counting sort of the SHADE_SURFACE segment by material id, as two kernels
// (0002 D7 step 3). The host runs it when the segment holds kSortMinItems paths and the scene
// has kSortMinMaterials materials (launch_config.h) and then reads sort_queue instead of the
// segment of main_queue.
//   count:   per tile of kQueueTile segment items, a histogram of material ids;
//   scatter: each tile derives its start offset in every material's run from all tiles'
//            histograms and writes its items there in segment order, so equal materials keep
//            their queue_build order (stable).
// There is no scan kernel (D8 lists two), so a scatter tile sums the histogram columns itself:
// O(tiles x materials) per tile, small for the scenes the threshold admits. Counts are
// integers, so the CPU and GPU outputs are bit-identical.
// Material id: the material table entry of the hit triangle (instance slice + triangle slot);
// the default material (an instance with no slice, or no material tables) is id
// material_header_count, so a scene has material_header_count + 1 bins.
// kSortMinMaterials is compared with material_header_count (real materials); the default-material
// bin does not count, so a scene with one material plus default-material hits stays unsorted.
// Host contract: pass `sort_grid(count)` as the launch count of both kernels, after queue_build
// has finished; reallocate sort_counts whenever material_header_count changes. kg.sort_counts holds 2 * queue_tiles(count) * sort_bins(kg) counters and
// kg.sort_queue `count` indices.
// Ref: Cycles device/gpu/parallel_sorted_index.h (Apache-2.0) as the pattern;
// docs/decisions/0002-kernel-architecture.md D6, D7 step 3, D8; 0007 d10 (slot lookup).
#pragma once
#include <cstdint>

#include "../kernel_globals.h"
#include "../launch_config.h"
#include "../portability.h"
#include "../scene/scene_view.h"
#include "../state/stage.h"
#include "queue_build.h"

AR_NS_BEGIN

AR_FN uint32_t sort_bins(const KernelGlobals& kg) { return scene_of(kg).material_header_count + 1u; }

AR_FN uint32_t sort_segment_start(const KernelGlobals& kg) {
  return kg.step_table->stage[static_cast<uint32_t>(Stage::SHADE_SURFACE)].start;
}
AR_FN uint32_t sort_segment_count(const KernelGlobals& kg) {
  return kg.step_table->stage[static_cast<uint32_t>(Stage::SHADE_SURFACE)].count;
}

AR_FN uint32_t sort_tile_count(const KernelGlobals& kg) { return queue_tiles(sort_segment_count(kg)); }

// True when the scheduler should run the sort: a large enough segment and at least
// kSortMinMaterials real materials (0002 D7 step 3).
AR_FN bool sort_wanted(const KernelGlobals& kg) {
  return sort_segment_count(kg) >= kSortMinItems && scene_of(kg).material_header_count >= kSortMinMaterials;
}

// The launch count of both kernels for a segment of `count` items.
AR_FN uint32_t sort_grid(uint32_t count) { return queue_tiles(count) * kQueueTile; }

// Material id of the segment's item i.
AR_FN uint32_t sort_item_material(const KernelGlobals& kg, uint32_t i) {
  const uint32_t p = kg.main_queue[sort_segment_start(kg) + i];
  return hit_material_id(kg, ps_hit_inst(kg, p), ps_hit_prim(kg, p));
}

AR_FN QueueTile sort_tile(const KernelGlobals& kg, uint32_t g) {
  const uint32_t count = sort_segment_count(kg);
  QueueTile t;
  t.pool = Pool::MAIN;
  t.first = g * kQueueTile;
  t.last = count - t.first < kQueueTile ? count : t.first + kQueueTile;
  return t;
}

// Kernel 1 for tile g (g < sort_tile_count): the tile's histogram.
AR_FN void sort_count_tile(const KernelGlobals& kg, uint32_t g) {
  const QueueTile t = sort_tile(kg, g);
  const uint32_t bins = sort_bins(kg);
  uint32_t* row = kg.sort_counts + uint64_t(g) * bins;
  for (uint32_t b = 0; b < bins; ++b) row[b] = 0;
  for (uint32_t i = t.first; i < t.last; ++i) {
    const uint32_t m = sort_item_material(kg, i);
    AR_ASSERT(m < bins);
    row[m] += 1u;
  }
}

// Kernel 2 for tile g: writes the tile's items into the material runs of sort_queue.
AR_FN void sort_scatter_tile(const KernelGlobals& kg, uint32_t g) {
  const QueueTile t = sort_tile(kg, g);
  const uint32_t bins = sort_bins(kg);
  const uint32_t tiles = sort_tile_count(kg);
  const uint32_t* counts = kg.sort_counts;
  uint32_t* at = kg.sort_counts + uint64_t(tiles + g) * bins;  // this tile's own scratch row
  uint32_t run_start = 0;
  for (uint32_t m = 0; m < bins; ++m) {
    uint32_t before = 0, total = 0;
    for (uint32_t r = 0; r < tiles; ++r) {
      const uint32_t c = counts[uint64_t(r) * bins + m];
      before += r < g ? c : 0u;
      total += c;
    }
    at[m] = run_start + before;
    run_start += total;
  }
  for (uint32_t i = t.first; i < t.last; ++i)
    kg.sort_queue[at[sort_item_material(kg, i)]++] = kg.main_queue[sort_segment_start(kg) + i];
}

AR_NS_END

#if !AR_CUDA_BUILD
// CPU entry points (src/kernel_cpu/sort_*.cpp), CpuKernelFn chunk functions for
// KernelId::SORT_COUNT and SORT_SCATTER; the host registers them on its CpuDevice.
void cpu_sort_count(const ar::KernelGlobals& kg, uint32_t first, uint32_t last);
void cpu_sort_scatter(const ar::KernelGlobals& kg, uint32_t first, uint32_t last);
#endif
