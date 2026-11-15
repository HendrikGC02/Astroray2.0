// SPDX-License-Identifier: Apache-2.0
// queue_build: one stable partition of both pools by next_stage, as three kernels (0002 D6).
//   count:   per tile of kQueueTile items, a histogram of next_stage (bin kStageCount counts
//            values that are no Stage of the pool: invalid items, in no segment);
//   scan:    one unit turns every histogram column into exclusive per-tile offsets, totals the
//            segments into the step table and finishes it (sched/step_table.h);
//   scatter: each tile writes its items' pool-local indices into their segments in item-index
//            order, so a segment is sorted by index and the partition is stable.
// Both pools are partitioned up to StepTable::high_water. Tiles are numbered main pool first,
// then shadow pool; tile g owns histogram row g of KernelGlobals::queue_counts. The functions
// are the one source of both targets: src/kernel_gpu/queue_build_*.cu runs them one tile per
// thread, src/kernel_cpu/queue_build_*.cpp one tile per loop iteration. Counts are integers, so
// the CPU and GPU tables are bit-identical.
// Host contract: pass `queue_build_grid(...)` as the launch count of count and scatter (on the
// CPU it is the item range the chunks split, on the GPU only a grid size) and any count >= 1 for
// scan, which runs once whatever the count (0002 D4: the device reads its work from the table).
// Ref: Blelloch 1990, "Prefix Sums and Their Applications" (reduce-then-scan); Cycles
// device/gpu/parallel_active_index.h and parallel_prefix_sum.h (Apache-2.0) as patterns;
// docs/decisions/0002-kernel-architecture.md D5, D6, D8.
#pragma once
#include <cstdint>

#include "../kernel_globals.h"
#include "../launch_config.h"
#include "../portability.h"
#include "../sched/step_table.h"
#include "../state/stage.h"

AR_NS_BEGIN

// One histogram row: a counter per Stage value and one for invalid values.
inline constexpr uint32_t kQueueBins = kStageCount + 1;
inline constexpr uint32_t kInvalidBin = kStageCount;

// Tiles needed for `high_water` items.
AR_FN uint32_t queue_tiles(uint32_t high_water) {
  return high_water / kQueueTile + (high_water % kQueueTile != 0 ? 1u : 0u);
}

AR_FN uint32_t queue_tile_count(const KernelGlobals& kg) {
  return queue_tiles(kg.step_table->high_water[static_cast<uint32_t>(Pool::MAIN)]) +
         queue_tiles(kg.step_table->high_water[static_cast<uint32_t>(Pool::SHADOW)]);
}

// The launch count a host passes to queue_build_count and queue_build_scatter for the given
// high-water marks: one virtual item per item of every tile.
AR_FN uint32_t queue_build_grid(uint32_t high_water_main, uint32_t high_water_shadow) {
  return (queue_tiles(high_water_main) + queue_tiles(high_water_shadow)) * kQueueTile;
}

// The histogram bin of a next_stage value in a pool: its Stage value if that stage belongs to
// the pool, else the invalid bin.
AR_FN uint32_t queue_bin(Pool pool, uint8_t value) {
  return (value < kStageCount && stage_pool(static_cast<Stage>(value)) == pool) ? value : kInvalidBin;
}

struct QueueTile {
  Pool pool;
  uint32_t first;  // pool-local index range [first, last)
  uint32_t last;
};

AR_FN QueueTile queue_tile(const KernelGlobals& kg, uint32_t g) {
  const uint32_t hw_main = kg.step_table->high_water[static_cast<uint32_t>(Pool::MAIN)];
  const uint32_t hw_shadow = kg.step_table->high_water[static_cast<uint32_t>(Pool::SHADOW)];
  const uint32_t tiles_main = queue_tiles(hw_main);
  QueueTile t;
  t.pool = g < tiles_main ? Pool::MAIN : Pool::SHADOW;
  const uint32_t local = g < tiles_main ? g : g - tiles_main;
  const uint32_t high = g < tiles_main ? hw_main : hw_shadow;
  t.first = local * kQueueTile;
  t.last = high - t.first < kQueueTile ? high : t.first + kQueueTile;
  return t;
}

AR_FN const uint8_t* queue_next_stage(const KernelGlobals& kg, Pool pool) {
  return pool == Pool::MAIN ? kg.path->next_stage : kg.shadow->next_stage;
}

// Kernel 1 for tile g (g < queue_tile_count): the tile's histogram.
AR_FN void queue_count_tile(const KernelGlobals& kg, uint32_t g) {
  const QueueTile t = queue_tile(kg, g);
  const uint8_t* next = queue_next_stage(kg, t.pool);
  uint32_t* row = kg.queue_counts + uint64_t(g) * kQueueBins;
  AR_ASSERT(next != nullptr);
  for (uint32_t b = 0; b < kQueueBins; ++b) row[b] = 0;
  for (uint32_t i = t.first; i < t.last; ++i) row[queue_bin(t.pool, next[i])] += 1u;
}

// The exclusive prefix of histogram column `bin` over the `n` rows from `first_row`, in place;
// returns the column total.
AR_FN uint32_t queue_column_scan(uint32_t* counts, uint32_t first_row, uint32_t n, uint32_t bin) {
  uint32_t running = 0;
  for (uint32_t r = 0; r < n; ++r) {
    uint32_t& c = counts[uint64_t(first_row + r) * kQueueBins + bin];
    const uint32_t v = c;
    c = running;
    running += v;
  }
  return running;
}

// Kernel 2, run by exactly one unit: every column of both pools becomes exclusive per-tile
// offsets, the totals become the segment lengths, and the step table is finished with the
// producers' k from launch_config.h. high_water is left as the host wrote it.
AR_FN void queue_scan(const KernelGlobals& kg) {
  StepTable& table = *const_cast<StepTable*>(kg.step_table);
  uint32_t* counts = kg.queue_counts;
  const uint32_t tiles_main = queue_tiles(table.high_water[static_cast<uint32_t>(Pool::MAIN)]);
  const uint32_t tiles_shadow = queue_tiles(table.high_water[static_cast<uint32_t>(Pool::SHADOW)]);
  for (uint32_t s = 0; s < kStageCount; ++s) {
    const bool shadow = stage_pool(static_cast<Stage>(s)) == Pool::SHADOW;
    table.stage[s].count = queue_column_scan(counts, shadow ? tiles_main : 0u, shadow ? tiles_shadow : tiles_main, s);
  }
  table.invalid_items = queue_column_scan(counts, 0u, tiles_main, kInvalidBin) +
                        queue_column_scan(counts, tiles_main, tiles_shadow, kInvalidBin);
  step_table_finish(table, StageShadowItems{});
}

// Kernel 3 for tile g: writes the tile's valid items into their segments. The step table's
// segment starts (not its launch counts) and the tile's offsets place every item, so a
// shadow producer's held-back items are in the queue too.
AR_FN void queue_scatter_tile(const KernelGlobals& kg, uint32_t g) {
  const QueueTile t = queue_tile(kg, g);
  const uint8_t* next = queue_next_stage(kg, t.pool);
  uint32_t* queue = const_cast<uint32_t*>(t.pool == Pool::MAIN ? kg.main_queue : kg.shadow_queue);
  const uint32_t* row = kg.queue_counts + uint64_t(g) * kQueueBins;
  AR_ASSERT(next != nullptr && queue != nullptr);
  uint32_t at[kQueueBins];
  for (uint32_t b = 0; b < kStageCount; ++b) at[b] = kg.step_table->stage[b].start + row[b];
  at[kInvalidBin] = 0;
  for (uint32_t i = t.first; i < t.last; ++i) {
    const uint32_t bin = queue_bin(t.pool, next[i]);
    if (bin == kInvalidBin) continue;
    queue[at[bin]++] = i;
  }
}

AR_NS_END

#if !AR_CUDA_BUILD
// CPU entry points (src/kernel_cpu/queue_build_*.cpp), CpuKernelFn chunk functions for
// KernelId::QUEUE_BUILD_COUNT, _SCAN and _SCATTER; the host registers them on its CpuDevice.
void cpu_queue_build_count(const ar::KernelGlobals& kg, uint32_t first, uint32_t last);
void cpu_queue_build_scan(const ar::KernelGlobals& kg, uint32_t first, uint32_t last);
void cpu_queue_build_scatter(const ar::KernelGlobals& kg, uint32_t first, uint32_t last);
#endif
