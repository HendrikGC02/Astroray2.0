// SPDX-License-Identifier: Apache-2.0
// sort: a stable counting sort of the SHADE_SURFACE segment by material id, as three kernels
// (0002 D7 step 3, D8). The host runs it when the segment holds kSortMinItems paths and the
// scene has kSortMinMaterials materials (launch_config.h) and then reads sort_queue instead of
// the segment of main_queue.
//   count:   per tile of kQueueTile segment items, a histogram of material ids (one row);
//   scan:    queue_build's single-pass look-back scan (scan_rows_tile, one source) down every
//            material column of the (tile x material) histogram: row g becomes tile g's
//            inclusive offset within each material's run, and the last row each run's length;
//   scatter: tile g starts material m at the sum of the earlier runs' lengths plus row g - 1's
//            offset, O(materials) per tile, and writes its items there in segment order, so
//            equal materials keep their queue_build order (stable).
// Counts are integers, so the CPU and GPU outputs are bit-identical. Count and scatter run one
// tile per thread: with hundreds of bins a warp vote per bin (queue_build) costs more than it
// saves.
// Material id: the material table entry of the hit triangle (instance slice + triangle slot);
// the default material (an instance with no slice, or no material tables) is id
// material_header_count, so a scene has material_header_count + 1 bins.
// kSortMinMaterials is compared with material_header_count (real materials); the default-material
// bin does not count, so a scene with one material plus default-material hits stays unsorted.
// Host contract: after queue_build has finished, launch count and scatter with `sort_grid(count)`
// and, between them, scan with `sort_scan_grid(count)`; reallocate sort_counts whenever
// material_header_count changes. kg.sort_counts holds 2 * queue_tiles(count) * sort_bins(kg)
// counters (the histogram rows, then one scratch row per scatter tile) and kg.sort_queue
// `count` indices.
// Ref: Cycles device/gpu/parallel_sorted_index.h (Apache-2.0) as the pattern; Merrill and
// Garland 2016, NVR-2016-002, sections 4-5, through queue_build.h's scan;
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
AR_FN bool sort_wanted(uint32_t segment_count, uint32_t material_header_count) {
  return segment_count >= kSortMinItems && material_header_count >= kSortMinMaterials;
}
// The same rule on the device-resident table and scene (CPU stages and tests); the host
// scheduler calls the pure form with the counts it already read.
AR_FN bool sort_wanted(const KernelGlobals& kg) {
  return sort_wanted(sort_segment_count(kg), scene_of(kg).material_header_count);
}

// The launch count of count and scatter for a segment of `count` items.
AR_FN uint32_t sort_grid(uint32_t count) { return queue_tiles(count) * kQueueTile; }

// The launch count of scan: kQueueLanes units per scan tile of kQueueScanRows histogram rows.
AR_FN uint32_t sort_scan_grid(uint32_t count) { return queue_scan_tiles(queue_tiles(count)) * kQueueLanes; }

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

// Kernel 2 for scan tile u (u < queue_scan_tiles(sort_tile_count)), by `lanes` units of which
// this is `lane`: the look-back scan of queue_build.h over the material histogram.
AR_FN void sort_scan_tile(const KernelGlobals& kg, uint32_t u, uint32_t lane, uint32_t lanes) {
  // A column counts items of the segment, at most the main pool's 2^21 (0002 D5), so it fits
  // the status word's value field (queue_build.h) for any number of materials.
  AR_ASSERT(sort_segment_count(kg) <= kScanValueMask);
  scan_rows_tile(kg.sort_counts, sort_tile_count(kg), sort_bins(kg), u, lane, lanes);
}

// Kernel 3 for tile g: writes the tile's items into the material runs of sort_queue. After the
// scan, row r holds the inclusive offsets through tile r (flagged on status rows, hence the
// mask) and the last row the run lengths.
AR_FN void sort_scatter_tile(const KernelGlobals& kg, uint32_t g) {
  const QueueTile t = sort_tile(kg, g);
  const uint32_t bins = sort_bins(kg);
  const uint32_t tiles = sort_tile_count(kg);
  const uint32_t* total = kg.sort_counts + uint64_t(tiles - 1u) * bins;
  const uint32_t* prev = kg.sort_counts + (uint64_t(g) - 1u) * bins;
  uint32_t* at = kg.sort_counts + uint64_t(tiles + g) * bins;  // this tile's own scratch row
  uint32_t run_start = 0;
  for (uint32_t m = 0; m < bins; ++m) {
    at[m] = run_start + (g == 0 ? 0u : prev[m] & kScanValueMask);
    run_start += total[m] & kScanValueMask;
  }
  for (uint32_t i = t.first; i < t.last; ++i)
    kg.sort_queue[at[sort_item_material(kg, i)]++] = kg.main_queue[sort_segment_start(kg) + i];
}

AR_NS_END

#if !AR_CUDA_BUILD
// CPU entry points (src/kernel_cpu/sort_*.cpp), CpuKernelFn chunk functions for
// KernelId::SORT_COUNT, SORT_SCAN and SORT_SCATTER; the host registers them on its CpuDevice.
void cpu_sort_count(const ar::KernelGlobals& kg, uint32_t first, uint32_t last);
void cpu_sort_scan(const ar::KernelGlobals& kg, uint32_t first, uint32_t last);
void cpu_sort_scatter(const ar::KernelGlobals& kg, uint32_t first, uint32_t last);
#endif
