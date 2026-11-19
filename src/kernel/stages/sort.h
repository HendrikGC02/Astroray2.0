// SPDX-License-Identifier: Apache-2.0
// sort: a stable counting sort of the SHADE_SURFACE segment by material id, as three kernels
// (0002 D7 step 3, D8). The host runs it when the segment holds kSortMinItems paths and the
// scene's materials run at least kSortMinMaterials distinct surface programs (launch_config.h,
// RULINGS 80) and then reads sort_queue instead of the segment of main_queue.
//   count:   per tile of kQueueTile segment items, a histogram of material ids (one row);
//   scan:    queue_build's single-pass look-back scan (scan_rows_tile, one source) down every
//            material column of the (tile x material) histogram: row g becomes tile g's
//            inclusive offset within each material's run, and the last row each run's length;
//   scatter: tile g starts material m at the sum of the earlier runs' lengths plus row g - 1's
//            offset, O(materials) per tile, and writes its items there in segment order, so
//            equal materials keep their queue_build order (stable).
// Counts are integers, so the CPU and GPU outputs are bit-identical. Count and scatter are
// written for `lanes` cooperating units, as queue_build: a GPU warp per tile, whose lanes take
// consecutive items and find the lanes with the same material with one ar::match_any (with
// hundreds of bins a ballot per bin, queue_build's vote, costs more than it saves); the CPU runs one
// unit per tile.
// Material id: the material table entry of the hit triangle (instance slice + triangle slot);
// the default material (an instance with no slice, or no material tables) is id
// material_header_count, so a scene has material_header_count + 1 bins.
// kSortMinMaterials is compared with the number of distinct surface programs among the real
// materials (sort_program_count); the default material does not count, so a scene with one
// program plus default-material hits stays unsorted. Sorting buys SIMT coherence only between
// different programs: materials that share a program and differ in constants gain none and lose
// path-state locality (RULINGS 80). This departs from Cycles, which sorts shade_surface by
// shader id (intersect_closest.h passes `shader` as the key of integrator_path_next_sorted,
// state_flow.h INTEGRATOR_SORT_KEY) whenever that kernel runs (path_trace_work_gpu.cpp,
// kernel_uses_sorting): a Cycles shader corresponds to a material header here, so Cycles would
// sort B-5M's 300 shaders. Task 120 measured that sort slower on B-5M, hence the program count.
// Host contract: after queue_build has finished, launch count and scatter with `sort_grid(count)`
// on the CPU (an item range) or gpu_tile_grid(count) on the GPU (queue_build.h, a warp per tile)
// and, between them, scan with `sort_scan_grid(count)`; reallocate sort_counts whenever
// material_header_count changes. kg.sort_counts holds 2 * queue_tiles(count) * sort_bins(kg)
// counters (the histogram rows, then one scratch row per scatter tile) and kg.sort_queue
// `count` indices.
// Ref: Cycles device/gpu/parallel_sorted_index.h (Apache-2.0) as the pattern; Cycles
// integrator/path_trace_work_gpu.cpp, kernel/integrator/intersect_closest.h and state_flow.h
// (Apache-2.0) for the shader-id key; RULINGS 80 for the program rule; Merrill and
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

// The number of distinct surface programs among headers [0, n) (pass material_header_count, so
// the default material is not counted). Every CONSTANT_SURFACE header is one shared program: its
// surface entry never runs (shade_surface resolves the constant block only). Any other header's
// program is the one at its program_offset; identical bytecode at two offsets counts twice. The
// material store appends programs in header order (material_store.cpp), so offsets of
// non-constant headers never decrease and equal offsets are adjacent among them.
AR_FN uint32_t sort_program_count(const MaterialHeader* headers, uint32_t n) {
  if (headers == nullptr) return 0u;
  bool constant = false, any = false;
  uint32_t programs = 0, last = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const MaterialHeader& h = headers[i];
    if ((h.flags & CONSTANT_SURFACE) != 0u) {
      constant = true;
      continue;
    }
    AR_ASSERT(!any || h.program_offset >= last);
    if (!any || h.program_offset != last) ++programs;
    any = true, last = h.program_offset;
  }
  return programs + (constant ? 1u : 0u);
}

// True when the scheduler should run the sort: a large enough segment and at least
// kSortMinMaterials distinct surface programs (0002 D7 step 3, RULINGS 80).
AR_FN bool sort_wanted(uint32_t segment_count, uint32_t program_count) {
  return segment_count >= kSortMinItems && program_count >= kSortMinMaterials;
}
// The same rule on the device-resident table and scene (CPU stages and tests), counting the
// programs from the headers; the host scheduler calls the pure form with the count the scene
// tables computed at upload.
AR_FN bool sort_wanted(const KernelGlobals& kg) {
  const SceneData& s = scene_of(kg);
  return sort_wanted(sort_segment_count(kg), sort_program_count(s.material_headers, s.material_header_count));
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

// Kernel 1 for tile g (g < sort_tile_count), by `lanes` units of which this is `lane`: the tile's
// histogram. Each round of `lanes` consecutive items, the lowest lane of each group of equal
// materials adds the group's size; integer adds, so the counts do not depend on their order.
AR_FN void sort_count_tile(const KernelGlobals& kg, uint32_t g, uint32_t lane, uint32_t lanes) {
  const QueueTile t = sort_tile(kg, g);
  const uint32_t bins = sort_bins(kg);
  uint32_t* row = kg.sort_counts + uint64_t(g) * bins;
  for (uint32_t b = lane; b < bins; b += lanes) row[b] = 0;
  sync_lanes();  // the zeroed row before any lane adds to it
  const uint32_t below = (1u << lane) - 1u;
  for (uint32_t base = t.first; base < t.last; base += lanes) {
    const uint32_t i = base + lane;
    const bool valid = i < t.last;
    const uint32_t m = valid ? sort_item_material(kg, i) : bins;  // bins: no material, past the tile
    AR_ASSERT(m <= bins);
    const uint32_t peers = match_any(m);
    if (valid && (peers & below) == 0u) atomic_add(row + m, popc(peers));
  }
}
AR_FN void sort_count_tile(const KernelGlobals& kg, uint32_t g) { sort_count_tile(kg, g, 0u, 1u); }

// Kernel 2 for scan tile u (u < queue_scan_tiles(sort_tile_count)), by `lanes` units of which
// this is `lane`: the look-back scan of queue_build.h over the material histogram.
AR_FN void sort_scan_tile(const KernelGlobals& kg, uint32_t u, uint32_t lane, uint32_t lanes) {
  // A column counts items of the segment, at most the main pool's 2^21 (0002 D5), so it fits
  // the status word's value field (queue_build.h) for any number of materials.
  AR_ASSERT(sort_segment_count(kg) <= kScanValueMask);
  scan_rows_tile(kg.sort_counts, sort_tile_count(kg), sort_bins(kg), u, lane, lanes);
}

// Kernel 3 for tile g, by `lanes` units as in sort_count_tile: writes the tile's items into the
// material runs of sort_queue. After the scan, row r holds the inclusive offsets through tile r
// (flagged on status rows, hence the mask) and the last row the run lengths. Within a round an
// item goes after the items of its material in lower lanes, so the order stays the segment order.
AR_FN void sort_scatter_tile(const KernelGlobals& kg, uint32_t g, uint32_t lane, uint32_t lanes) {
  const QueueTile t = sort_tile(kg, g);
  const uint32_t bins = sort_bins(kg);
  const uint32_t tiles = sort_tile_count(kg);
  const uint32_t* total = kg.sort_counts + uint64_t(tiles - 1u) * bins;
  const uint32_t* prev = kg.sort_counts + (uint64_t(g) - 1u) * bins;
  uint32_t* at = kg.sort_counts + uint64_t(tiles + g) * bins;  // this tile's own scratch row
  if (lane == 0u) {  // a running sum over the materials: one unit, O(materials)
    uint32_t run_start = 0;
    for (uint32_t m = 0; m < bins; ++m) {
      at[m] = run_start + (g == 0 ? 0u : prev[m] & kScanValueMask);
      run_start += total[m] & kScanValueMask;
    }
  }
  sync_lanes();  // every lane reads the starts lane 0 wrote
  const uint32_t below = (1u << lane) - 1u;
  const uint32_t start = sort_segment_start(kg);
  for (uint32_t base = t.first; base < t.last; base += lanes) {
    const uint32_t i = base + lane;
    const bool valid = i < t.last;
    const uint32_t m = valid ? sort_item_material(kg, i) : bins;
    const uint32_t peers = match_any(m);
    const uint32_t pos = valid ? at[m] + popc(peers & below) : 0u;
    sync_lanes();  // every lane has read at[] before the group leaders advance it
    if (valid) {
      kg.sort_queue[pos] = kg.main_queue[start + i];
      if ((peers & below) == 0u) at[m] += popc(peers);
    }
    sync_lanes();  // the advanced starts before the next round reads them
  }
}
AR_FN void sort_scatter_tile(const KernelGlobals& kg, uint32_t g) { sort_scatter_tile(kg, g, 0u, 1u); }

AR_NS_END

#if !AR_CUDA_BUILD
// CPU entry points (src/kernel_cpu/sort_*.cpp), CpuKernelFn chunk functions for
// KernelId::SORT_COUNT, SORT_SCAN and SORT_SCATTER; the host registers them on its CpuDevice.
void cpu_sort_count(const ar::KernelGlobals& kg, uint32_t first, uint32_t last);
void cpu_sort_scan(const ar::KernelGlobals& kg, uint32_t first, uint32_t last);
void cpu_sort_scatter(const ar::KernelGlobals& kg, uint32_t first, uint32_t last);
#endif
