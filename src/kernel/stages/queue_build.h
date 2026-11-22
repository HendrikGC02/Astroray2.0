// SPDX-License-Identifier: Apache-2.0
// queue_build: one stable partition of both pools by next_stage, as three kernels (0002 D6).
//   count:   per tile of kQueueTile items, a histogram of next_stage (bin kStageCount counts
//            values that are no Stage of the pool: invalid items, in no segment);
//   scan:    a single-pass scan with decoupled look-back over the histogram rows (0002 D7
//            remedy (c)): scan tile u, kQueueScanRows consecutive rows, sums its rows per bin,
//            publishes that aggregate, looks back over its predecessors' published values for
//            its exclusive prefix and rewrites its rows as inclusive per-tile offsets; the last
//            scan tile totals the segments into the step table and finishes it (sched/step_table.h);
//   scatter: each tile writes its items' pool-local indices into their segments in item-index
//            order, so a segment is sorted by index and the partition is stable; the one item of a pool's
//            TERMINATED segment that starts its run of entries equal to their positions writes the pool's
//            StepTable::live_end.
// Both pools are partitioned up to StepTable::high_water. Tiles are numbered main pool first,
// then shadow pool; tile g owns histogram row g of KernelGlobals::queue_counts. A tile bins the
// other pool's stages as invalid, so one scan over all rows gives every column its pool's
// offsets. The functions are the one source of both targets, written for `lanes` cooperating
// units with the warp vote of portability.h: src/kernel_gpu/queue_build_*.cu runs a tile or a
// scan tile per warp (kQueueLanes lanes; in the scan a lane per bin); src/kernel_cpu/
// queue_build_*.cpp runs one unit (ar::ballot of one lane) per tile, and the scan tiles in order,
// where every predecessor has published its inclusive prefix and the look-back stops at the
// first. Counts are integers, so the CPU and GPU tables are bit-identical.
// Host contract: launch count and scatter with `queue_build_grid(...)` on the CPU (the item range
// its chunks split into tiles) or the sum of gpu_tile_grid over the two high-water marks on the GPU
// (a grid size: one warp per tile), and scan with `queue_scan_grid(...)` on both (0002 D4: the
// device reads its work from the table).
// Ref: Merrill and Garland 2016, "Single-pass Parallel Prefix Scan with Decoupled Look-back"
// (NVIDIA NVR-2016-002), sections 4-5 (tile status: flag and value in one word, aggregate then
// inclusive prefix, look-back until the first inclusive prefix); Blelloch 1990, "Prefix Sums and
// Their Applications" (the sequential order the CPU runs); Cycles
// device/gpu/parallel_active_index.h (Apache-2.0; a lane's offset in its warp is the popcount of
// the ballot of the lanes below it, applied here per bin);
// docs/decisions/0002-kernel-architecture.md D5, D6, D7, D8.
// The look-back scan (scan_rows_tile) takes any (rows x bins) histogram; the material sort
// (sort.h) scans its (tile x material) histogram with it.
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

// The GPU launch count of the tiled kernels (queue_build count and scatter, the sort's count and
// scatter) for `items` items: one warp of kQueueLanes threads per tile, so no warp is launched only
// to find no tile. The CPU takes queue_build_grid / sort_grid instead (an item range).
AR_FN uint32_t gpu_tile_grid(uint32_t items) { return queue_tiles(items) * kQueueLanes; }

AR_FN uint32_t queue_tile_count(const KernelGlobals& kg) {
  return queue_tiles(kg.step_table->high_water[static_cast<uint32_t>(Pool::MAIN)]) +
         queue_tiles(kg.step_table->high_water[static_cast<uint32_t>(Pool::SHADOW)]);
}

// The launch count a host passes to queue_build_count and queue_build_scatter for the given
// high-water marks: one virtual item per item of every tile. The GPU needs only kQueueLanes
// threads per tile; the surplus warps find no tile and return.
AR_FN uint32_t queue_build_grid(uint32_t high_water_main, uint32_t high_water_shadow) {
  return (queue_tiles(high_water_main) + queue_tiles(high_water_shadow)) * kQueueTile;
}

// Scan tiles over `tiles` histogram rows; at least one, whose lane 0 finishes the step table
// even when there are no rows.
AR_FN uint32_t queue_scan_tiles(uint32_t tiles) {
  return tiles == 0 ? 1u : tiles / kQueueScanRows + (tiles % kQueueScanRows != 0 ? 1u : 0u);
}

// The launch count a host passes to queue_build_scan: kQueueLanes units per scan tile.
AR_FN uint32_t queue_scan_grid(uint32_t high_water_main, uint32_t high_water_shadow) {
  return queue_scan_tiles(queue_tiles(high_water_main) + queue_tiles(high_water_shadow)) * kQueueLanes;
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

// The bin of item `i` of tile t, or kQueueBins (no bin) past the tile's end.
AR_FN uint32_t queue_item_bin(const QueueTile& t, const uint8_t* next, uint32_t i) {
  return i < t.last ? queue_bin(t.pool, next[i]) : kQueueBins;
}

// Kernel 1 for tile g (g < queue_tile_count), by `lanes` units of which this is `lane`: the
// tile's histogram. The units take consecutive items, and every unit counts every bin from the
// votes, so all of them hold the whole histogram; unit b % lanes writes bin b.
AR_FN void queue_count_tile(const KernelGlobals& kg, uint32_t g, uint32_t lane, uint32_t lanes) {
  const QueueTile t = queue_tile(kg, g);
  const uint8_t* next = queue_next_stage(kg, t.pool);
  uint32_t* row = kg.queue_counts + uint64_t(g) * kQueueBins;
  AR_ASSERT(next != nullptr);
  uint32_t hist[kQueueBins];
  for (uint32_t b = 0; b < kQueueBins; ++b) hist[b] = 0;
  for (uint32_t base = t.first; base < t.last; base += lanes) {
    const uint32_t bin = queue_item_bin(t, next, base + lane);
    for (uint32_t b = 0; b < kQueueBins; ++b) hist[b] += popc(ballot(bin == b));
  }
  for (uint32_t b = 0; b < kQueueBins; ++b)
    if (b % lanes == lane) row[b] = hist[b];
}

// Tile status of the look-back, one 32-bit word per scan tile and bin: the last histogram row
// of the scan tile, so it needs no memory of its own. The count kernel leaves a row count there,
// below 2^30, which reads as "nothing published"; flag and value share one word, so a reader
// sees both or neither and no fence is needed (Merrill and Garland 2016, section 4). Values
// stay below 2^30 because a column counts the items of one pool or of a segment of one: a pool
// holds at most 2^21 (main) or 2^22 (shadow) items (0002 D5). queue_build's columns span one
// pool; the material sort's span the SHADE_SURFACE segment, whatever the number of materials.
// The scheduler clamps the main pool to kMaxPoolCapacity (2^21, scheduler.h), which bounds the
// segment and so every column; sort.h asserts the 2^30 - 1 packing limit.
inline constexpr uint32_t kScanValueMask = (1u << 30) - 1u;
inline constexpr uint32_t kScanAggregate = 1u << 30;  // the scan tile's own sum
inline constexpr uint32_t kScanPrefix = 2u << 30;     // the inclusive prefix through the scan tile

// Status words go through volatile accesses: on the GPU they bypass the incoherent L1, so a
// spinning reader sees another block's store; on the CPU the scan tiles run in order.
AR_FN uint32_t queue_status_load(const uint32_t* p) { return *static_cast<const volatile uint32_t*>(p); }
AR_FN void queue_status_store(uint32_t* p, uint32_t v) { *static_cast<volatile uint32_t*>(p) = v; }

// The histogram row that holds scan tile u's status (its last row) in a histogram of `rows` rows.
AR_FN uint32_t queue_scan_status_row(uint32_t u, uint32_t rows) {
  const uint32_t end = (u + 1u) * kQueueScanRows;
  return (end < rows ? end : rows) - 1u;
}

// The exclusive prefix of `bin` for scan tile u of a (rows x bins) histogram: the predecessors'
// published values from u - 1 down, adding aggregates until the first inclusive prefix, and
// waiting on a predecessor that has published nothing yet (it is running: a predecessor has a
// lower unit index).
AR_FN uint32_t scan_lookback(const uint32_t* counts, uint32_t rows, uint32_t bins, uint32_t u, uint32_t bin) {
  uint32_t exclusive = 0;
  for (uint32_t p = u; p-- > 0;) {
    const uint32_t* status = counts + uint64_t(queue_scan_status_row(p, rows)) * bins + bin;
    uint32_t w = queue_status_load(status);
    while (w < kScanAggregate) w = queue_status_load(status);
    exclusive += w & kScanValueMask;
    if (w >= kScanPrefix) break;
  }
  return exclusive;
}

// The same over queue_build's histogram (kQueueBins columns).
AR_FN uint32_t queue_lookback(const uint32_t* counts, uint32_t tiles, uint32_t u, uint32_t bin) {
  return scan_lookback(counts, tiles, kQueueBins, u, bin);
}

// Scan tile u of a (rows x bins) histogram of per-tile counts, by `lanes` cooperating units of
// which this is `lane` (a GPU warp, or one CPU loop): lane l scans the bins l, l + lanes, ...
// The scan tile's rows become inclusive offsets down each column (the status row keeps its
// flag, so readers mask with kScanValueMask); once every scan tile has run, the last row holds
// every column's total.
AR_FN void scan_rows_tile(uint32_t* counts, uint32_t rows, uint32_t bins, uint32_t u, uint32_t lane,
                          uint32_t lanes) {
  const uint32_t first = u * kQueueScanRows;
  const uint32_t last = first + kQueueScanRows < rows ? first + kQueueScanRows : rows;
  for (uint32_t b = lane; first < last && b < bins; b += lanes) {
    uint32_t aggregate = 0;
    for (uint32_t r = first; r < last; ++r) aggregate += counts[uint64_t(r) * bins + b];
    uint32_t* status = counts + uint64_t(last - 1u) * bins + b;
    if (u != 0) queue_status_store(status, kScanAggregate | aggregate);
    const uint32_t exclusive = scan_lookback(counts, rows, bins, u, b);
    AR_ASSERT(exclusive + aggregate <= kScanValueMask);
    queue_status_store(status, kScanPrefix | (exclusive + aggregate));
    uint32_t running = exclusive;
    for (uint32_t r = first; r + 1u < last; ++r) {
      uint32_t& c = counts[uint64_t(r) * bins + b];
      running += c;
      c = running;
    }
  }
}

// Kernel 2 for scan tile u, by `lanes` units as in scan_rows_tile, over queue_counts. The last
// scan tile's lane 0 waits for every bin's total and finishes the step table with the
// producers' k from launch_config.h; high_water is left as the host wrote it.
AR_FN void queue_scan_tile(const KernelGlobals& kg, uint32_t u, uint32_t lane, uint32_t lanes) {
  uint32_t* counts = kg.queue_counts;
  const uint32_t tiles = queue_tile_count(kg);
  scan_rows_tile(counts, tiles, kQueueBins, u, lane, lanes);
  if (u + 1u != queue_scan_tiles(tiles) || lane != 0) return;
  StepTable& table = *const_cast<StepTable*>(kg.step_table);
  uint32_t total[kQueueBins];
  for (uint32_t b = 0; b < kQueueBins; ++b) {
    total[b] = 0;
    if (tiles == 0) continue;
    const uint32_t* status = counts + uint64_t(tiles - 1u) * kQueueBins + b;
    uint32_t w = queue_status_load(status);
    while (w < kScanPrefix) w = queue_status_load(status);
    total[b] = w & kScanValueMask;
  }
  for (uint32_t s = 0; s < kStageCount; ++s) table.stage[s].count = total[s];
  table.invalid_items = total[kInvalidBin];
  step_table_finish(table, StageShadowItems{});
}

// Kernel 3 for tile g, by `lanes` units as in queue_count_tile: writes the tile's valid items
// into their segments. The step table's segment starts (not its launch counts) and the tile's
// exclusive offsets, the previous row's inclusive ones, place every item, so a shadow producer's
// held-back items are in the queue too. Within a round of `lanes` items an item goes after the
// items of its bin in lower units, so the order stays the item order.
// live_end (step_table.h): the pool's TERMINATED segment starts at position L, the number of items
// in the pool's other segments, and lists the terminated slots in index order, so an entry holds
// its own position p exactly when every live item lies below slot p. The first such entry is the
// one whose slot is 0 or follows a live slot (at slot L every slot below is live); its item, the only
// one, stores itself as live_end.
// With no such entry the last slot below the mark is live and live_end keeps the mark the host
// wrote. One writer, so no atomic and the same value on both targets.
AR_FN void queue_scatter_tile(const KernelGlobals& kg, uint32_t g, uint32_t lane, uint32_t lanes) {
  const QueueTile t = queue_tile(kg, g);
  const uint8_t* next = queue_next_stage(kg, t.pool);
  uint32_t* queue = const_cast<uint32_t*>(t.pool == Pool::MAIN ? kg.main_queue : kg.shadow_queue);
  const uint32_t* prev = kg.queue_counts + (uint64_t(g) - 1u) * kQueueBins;
  AR_ASSERT(next != nullptr && queue != nullptr);
  const uint32_t dead = static_cast<uint32_t>(t.pool == Pool::MAIN ? Stage::TERMINATED : Stage::SHADOW_TERMINATED);
  uint32_t at[kStageCount];
  for (uint32_t b = 0; b < kStageCount; ++b)
    at[b] = kg.step_table->stage[b].start + (g == 0 ? 0u : prev[b] & kScanValueMask);
  const uint32_t below = (1u << lane) - 1u;  // the units before this one in its round
  for (uint32_t base = t.first; base < t.last; base += lanes) {
    const uint32_t i = base + lane;
    const uint32_t bin = queue_item_bin(t, next, i);
    // The TERMINATED segment's next entry before this round (constant indices keep at[] in registers).
    const uint32_t at_dead = t.pool == Pool::MAIN ? at[static_cast<uint32_t>(Stage::TERMINATED)]
                                                  : at[static_cast<uint32_t>(Stage::SHADOW_TERMINATED)];
    for (uint32_t b = 0; b < kStageCount; ++b) {
      const uint32_t votes = ballot(bin == b);
      if (bin == b) queue[at[b] + popc(votes & below)] = i;
      at[b] += popc(votes);
    }
    // A dead entry's position is at least its slot (every live item counts before it), so only a round
    // whose TERMINATED entries start below base + lanes can hold an entry equal to its position.
    if (at_dead < base + lanes) {
      const uint32_t dead_votes = ballot(bin == dead);
      if (bin == dead && at_dead + popc(dead_votes & below) == i &&
          (i == 0 || queue_bin(t.pool, next[i - 1u]) != dead)) {
        StepTable& table = *const_cast<StepTable*>(kg.step_table);
        table.live_end[static_cast<uint32_t>(t.pool)] = i;
      }
    }
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
