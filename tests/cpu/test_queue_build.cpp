// SPDX-License-Identifier: Apache-2.0
// queue_build on the CPU device: the partition equals a reference stable partition, the
// step table is exactly what the records give for crafted inputs (empty segments, invalid
// values, tile boundaries), and the shadow-slot reservation covers every edge of 0002 D5.
// Expected values come from the reference partition below, from tables typed by hand from
// the Stage enum order (INTERSECT_CLOSEST 0 ... SHADOW_TERMINATED 14) and from the setups,
// never read back from the code under test.
// Ref: docs/decisions/0002-kernel-architecture.md D5 (slot assignment, shortage rule, the
// "pool below demand" verification row), D6 (partition, step table), D8 (Stage order).
#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "device/cpu/cpu_device.h"
#include "host/thread_pool.h"
#include "stages/queue_build.h"

using namespace astroray;
using namespace astroray::host;
using ar::Pool;
using ar::Stage;

namespace {

constexpr uint32_t kSentinel = 0xFFFFFFFFu;
constexpr uint32_t kStages = ar::kStageCount;

uint32_t S(Stage s) { return static_cast<uint32_t>(s); }
uint8_t U8(Stage s) { return static_cast<uint8_t>(s); }

std::vector<Stage> stages_of(Pool p) {
  std::vector<Stage> v;
  for (uint32_t s = 0; s < kStages; ++s)
    if (ar::stage_pool(static_cast<Stage>(s)) == p) v.push_back(static_cast<Stage>(s));
  return v;
}

// SplitMix64: a fixed generator for the bulk inputs.
struct Rng {
  uint64_t x;
  uint32_t next() {
    uint64_t z = (x += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return static_cast<uint32_t>((z ^ (z >> 31)) >> 16);
  }
};

std::vector<uint8_t> random_pool(uint32_t n, Pool p, uint64_t seed) {
  const std::vector<Stage> valid = stages_of(p);
  Rng rng{seed};
  std::vector<uint8_t> v(n);
  for (auto& x : v) x = U8(valid[rng.next() % valid.size()]);
  return v;
}

// A CpuDevice with one KernelGlobals over caller-owned next_stage arrays.
struct Rig {
  ContextState ctx;
  Ledger ledger;
  ThreadPool pool;
  CpuDevice dev;
  std::vector<uint8_t> next[2];
  std::vector<uint32_t> queue[2];
  std::vector<uint32_t> counts;
  ar::StepTable table{};
  ar::PathStateSoA path{};
  ar::ShadowItemSoA shadow{};
  ar::KernelGlobals kg{};
  DeviceBuffer kg_buf;

  Rig(std::vector<uint8_t> main_next, std::vector<uint8_t> shadow_next, unsigned workers = 8)
      : pool(ctx, workers), dev(ledger, pool) {
    next[0] = std::move(main_next);
    next[1] = std::move(shadow_next);
    for (int p = 0; p < 2; ++p) queue[p].assign(std::max<size_t>(next[p].size(), 1), kSentinel);
    counts.assign((ar::queue_tiles(uint32_t(next[0].size())) + ar::queue_tiles(uint32_t(next[1].size()))) * ar::kQueueBins + 1, 0);
    path.next_stage = next[0].data();
    shadow.next_stage = next[1].data();
    kg.path = &path;
    kg.shadow = &shadow;
    kg.step_table = &table;
    kg.main_queue = queue[0].data();
    kg.shadow_queue = queue[1].data();
    kg.queue_counts = counts.data();
    dev.register_kernel(KernelId::QUEUE_BUILD_COUNT, &cpu_queue_build_count);
    dev.register_kernel(KernelId::QUEUE_BUILD_SCAN, &cpu_queue_build_scan);
    dev.register_kernel(KernelId::QUEUE_BUILD_SCATTER, &cpu_queue_build_scatter);
    kg_buf = dev.allocate(LedgerCategory::TABLES, sizeof kg, {1, 1});
    dev.upload(kg_buf, 0, &kg, sizeof kg);
    dev.set_kernel_globals(kg_buf);
  }
  ~Rig() { dev.free(kg_buf); }

  // One queue_build over the first hw_main / hw_shadow items of the pools. A non-zero
  // shadow_capacity lays the shadow queue out as the scheduler does (the identity, so the entries
  // past the partition are the fresh slots; RULINGS 84) and lets the reservation take them.
  void build(uint32_t hw_main, uint32_t hw_shadow, uint32_t shadow_capacity = 0) {
    for (int p = 0; p < 2; ++p) std::fill(queue[p].begin(), queue[p].end(), kSentinel);
    if (shadow_capacity != 0)
      for (uint32_t j = 0; j < queue[1].size(); ++j) queue[1][j] = j;
    table.high_water[0] = hw_main;
    table.high_water[1] = hw_shadow;
    table.shadow_capacity = shadow_capacity;
    table.live_end[0] = table.live_end[1] = 0;  // the host clears them with the marks
    const uint32_t grid = ar::queue_build_grid(hw_main, hw_shadow);
    dev.launch(KernelId::QUEUE_BUILD_COUNT, grid);
    dev.launch(KernelId::QUEUE_BUILD_SCAN, ar::queue_scan_grid(hw_main, hw_shadow));
    dev.launch(KernelId::QUEUE_BUILD_SCATTER, grid);
  }
  void build() { build(uint32_t(next[0].size()), uint32_t(next[1].size())); }
};

// What the records say, computed the slow, obvious way.
struct Expected {
  std::vector<uint32_t> items[kStages];  // item indices per stage, ascending
  uint32_t start[kStages];
  uint32_t launch[kStages];
  uint32_t shadow_base[kStages];
  uint32_t invalid = 0;
  uint32_t refill = 0;
};

Expected reference(const Rig& r, uint32_t hw_main, uint32_t hw_shadow) {
  Expected e{};
  const uint32_t hw[2] = {hw_main, hw_shadow};
  for (int p = 0; p < 2; ++p) {
    uint32_t at = 0;
    for (Stage s : stages_of(p == 0 ? Pool::MAIN : Pool::SHADOW)) {
      for (uint32_t i = 0; i < hw[p]; ++i)
        if (r.next[p][i] == U8(s)) e.items[S(s)].push_back(i);
      e.start[S(s)] = at;
      at += uint32_t(e.items[S(s)].size());
    }
    for (uint32_t i = 0; i < hw[p]; ++i) {
      const uint32_t v = r.next[p][i];
      const bool ok = v < kStages && ar::stage_pool(static_cast<Stage>(v)) == (p == 0 ? Pool::MAIN : Pool::SHADOW);
      if (!ok) ++e.invalid;
    }
  }
  // Shadow reservation with k = 1 for the five producers of launch_config.h.
  const Stage producers[] = {Stage::SHADE_SURFACE, Stage::SHADE_SURFACE_EXT, Stage::SHADE_LIGHT_PATH,
                             Stage::SHADE_LIGHT_PATH_EXT, Stage::SHADE_VOLUME};
  uint32_t free_left = uint32_t(e.items[S(Stage::SHADOW_TERMINATED)].size());
  uint32_t base = e.start[S(Stage::SHADOW_TERMINATED)];
  for (uint32_t s = 0; s < kStages; ++s) e.launch[s] = uint32_t(e.items[s].size());
  for (Stage s : producers) {
    const uint32_t take = std::min(e.launch[S(s)], free_left);
    e.shadow_base[S(s)] = base;
    e.launch[S(s)] = take;
    base += take;
    free_left -= take;
  }
  e.refill = uint32_t(e.items[S(Stage::TERMINATED)].size());
  return e;
}

// Every stage's segment, launch count, shadow base, the refill count and the invalid count
// equal the reference; queue entries beyond the partitioned items were never written.
void check_against_reference(const Rig& r, uint32_t hw_main, uint32_t hw_shadow) {
  const Expected e = reference(r, hw_main, hw_shadow);
  for (uint32_t s = 0; s < kStages; ++s) {
    INFO("stage " << s);
    const int p = ar::stage_pool(static_cast<Stage>(s)) == Pool::MAIN ? 0 : 1;
    const ar::StepEntry& got = r.table.stage[s];
    CHECK(got.start == e.start[s]);
    CHECK(got.count == e.launch[s]);
    CHECK(got.shadow_base == e.shadow_base[s]);
    bool same = e.start[s] + e.items[s].size() <= r.queue[p].size();
    for (size_t j = 0; same && j < e.items[s].size(); ++j) same = r.queue[p][e.start[s] + j] == e.items[s][j];
    CHECK(same);
  }
  CHECK(r.table.refill_count == e.refill);
  CHECK(r.table.invalid_items == e.invalid);
  CHECK(r.table.high_water[0] == hw_main);
  CHECK(r.table.high_water[1] == hw_shadow);
  const uint32_t written_main = hw_main - [&] {
    uint32_t bad = 0;
    for (uint32_t i = 0; i < hw_main; ++i) {
      const uint32_t v = r.next[0][i];
      if (!(v < kStages && ar::stage_pool(static_cast<Stage>(v)) == Pool::MAIN)) ++bad;
    }
    return bad;
  }();
  for (uint32_t j = written_main; j < r.queue[0].size(); ++j)
    if (r.queue[0][j] != kSentinel) {
      FAIL("main queue entry " << j << " written past the partitioned items");
      break;
    }
}

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("queue_build/stable partition") {
  // 10^6 random next_stage values in each pool against a reference stable partition.
  constexpr uint32_t kN = 1000000;
  Rig r(random_pool(kN, Pool::MAIN, 1), random_pool(kN, Pool::SHADOW, 2));
  r.build();
  check_against_reference(r, kN, kN);
  // The main pool's free list is its TERMINATED segment: a twelfth of the items, give or take.
  CHECK(r.table.refill_count > kN / 14);
  CHECK(r.table.refill_count < kN / 10);

  // Tile boundaries and high-water marks below the pool size: items past the mark are ignored.
  const uint32_t marks[][2] = {{0, 0}, {1, 0}, {0, 1}, {1023, 1024}, {1024, 1025}, {1025, 2047}, {2048, 2049}, {777777, 1000000}};
  for (const auto& m : marks) {
    INFO("high water " << m[0] << " " << m[1]);
    r.build(m[0], m[1]);
    check_against_reference(r, m[0], m[1]);
  }
}

TEST_CASE("queue_build/stable partition is independent of the worker count") {
  // The partition is integer code, so one worker and sixteen give the same tables and queues.
  const std::vector<uint8_t> a = random_pool(50000, Pool::MAIN, 7), b = random_pool(30000, Pool::SHADOW, 8);
  Rig one(a, b, 1), many(a, b, 16);
  one.build();
  many.build();
  check_against_reference(one, 50000, 30000);
  CHECK(one.queue[0] == many.queue[0]);
  CHECK(one.queue[1] == many.queue[1]);
  CHECK(std::memcmp(&one.table, &many.table, sizeof one.table) == 0);
}

TEST_CASE("queue_build/single-pass scan over many scan tiles") {
  // 400,000 main and 200,000 shadow items: 587 histogram rows, so 19 scan tiles of 32 rows
  // chain their prefixes through the look-back. Main items come in runs of 50,000 of
  // INTERSECT_CLOSEST, SHADE_SURFACE, TERMINATED (three segments, every other main segment
  // empty), so most scan tiles hold zeros in two of the three columns; shadow items in runs of
  // 70,000 of INTERSECT_SHADOW and SHADOW_TERMINATED, SHADE_SHADOW empty.
  constexpr uint32_t kMain = 400000, kShadow = 200000;
  const Stage main_runs[] = {Stage::INTERSECT_CLOSEST, Stage::SHADE_SURFACE, Stage::TERMINATED};
  const Stage shadow_runs[] = {Stage::INTERSECT_SHADOW, Stage::SHADOW_TERMINATED};
  std::vector<uint8_t> m(kMain), s(kShadow);
  for (uint32_t i = 0; i < kMain; ++i) m[i] = U8(main_runs[(i / 50000) % 3]);
  for (uint32_t i = 0; i < kShadow; ++i) s[i] = U8(shadow_runs[(i / 70000) % 2]);
  Rig r(m, s);
  r.build();
  CHECK(ar::queue_scan_tiles(ar::queue_tiles(kMain) + ar::queue_tiles(kShadow)) == 19);
  check_against_reference(r, kMain, kShadow);
  // Typed from the runs: 8 runs of 50,000 in the order closest, surface, terminated, ...
  CHECK(r.table.stage[S(Stage::INTERSECT_CLOSEST)].start == 0);
  CHECK(r.table.stage[S(Stage::SHADE_SURFACE)].start == 150000);
  CHECK(r.table.stage[S(Stage::TERMINATED)].start == 300000);
  CHECK(r.table.refill_count == 100000);
  CHECK(r.table.stage[S(Stage::SHADE_SHADOW)].start == 130000);
  CHECK(r.table.stage[S(Stage::SHADOW_TERMINATED)].count == 70000);
  // The last item of each segment is the last index of its last run.
  CHECK(r.queue[0][149999] == 349999);
  CHECK(r.queue[0][299999] == 399999);
  CHECK(r.queue[0][399999] == 299999);
}

TEST_CASE("queue_build/look-back adds aggregates up to the first inclusive prefix") {
  // Five of six scan tiles' status words in bin 3, typed by hand: tile 0 inclusive 10, tile 1
  // aggregate 5, tile 2 inclusive 100, tile 3 aggregate 7, tile 4 aggregate 1. On the CPU the
  // scan tiles run in order and the look-back meets an inclusive prefix at once; this is the
  // walk the GPU takes when its predecessors are still running.
  const uint32_t tiles = 6 * ar::kQueueScanRows;
  std::vector<uint32_t> counts(tiles * ar::kQueueBins, 0);
  const uint32_t status[5] = {ar::kScanPrefix | 10u, ar::kScanAggregate | 5u, ar::kScanPrefix | 100u,
                              ar::kScanAggregate | 7u, ar::kScanAggregate | 1u};
  for (uint32_t u = 0; u < 5; ++u) counts[(ar::kQueueScanRows * (u + 1) - 1) * ar::kQueueBins + 3] = status[u];
  CHECK(ar::queue_lookback(counts.data(), tiles, 0, 3) == 0);
  CHECK(ar::queue_lookback(counts.data(), tiles, 1, 3) == 10);
  CHECK(ar::queue_lookback(counts.data(), tiles, 2, 3) == 15);   // 5 + 10
  CHECK(ar::queue_lookback(counts.data(), tiles, 3, 3) == 100);
  CHECK(ar::queue_lookback(counts.data(), tiles, 5, 3) == 108);  // 1 + 7 + 100
}

TEST_CASE("queue_build/look-back reads a published aggregate") {
  // Scan tiles out of order, as the GPU may run them: scan tile 1 runs on its own thread (one
  // unit, so it scans bin after bin) while tile 0 has not run. For each bin it must publish
  // kScanAggregate | (the sum of its 32 raw rows) and then wait, since tile 0's status for that
  // bin still holds a raw count. While it waits, a successor's look-back over that published
  // word and tile 0's inclusive prefix must add both; then this thread runs tile 0 for that bin
  // only, which releases tile 1 to its next bin. Only these two threads run, so the case does
  // not depend on the core count. Then tiles 2 and 3 run and the partition equals the reference.
  constexpr uint32_t kMain = 100000, kShadow = 1000;
  Rig r(random_pool(kMain, Pool::MAIN, 31), random_pool(kShadow, Pool::SHADOW, 32));
  for (int p = 0; p < 2; ++p) std::fill(r.queue[p].begin(), r.queue[p].end(), kSentinel);
  r.table.high_water[0] = kMain;
  r.table.high_water[1] = kShadow;
  const uint32_t grid = ar::queue_build_grid(kMain, kShadow);
  r.dev.launch(KernelId::QUEUE_BUILD_COUNT, grid);
  const uint32_t tiles = ar::queue_tiles(kMain) + ar::queue_tiles(kShadow);  // 98 + 1 rows
  REQUIRE(ar::queue_scan_tiles(tiles) == 4);
  const uint32_t R = ar::kQueueScanRows, B = ar::kQueueBins;
  // Expected values from the raw rows, before any scan tile has run.
  std::vector<uint32_t> incl0(B, 0), agg1(B, 0);
  for (uint32_t b = 0; b < B; ++b)
    for (uint32_t row = 0; row < 2 * R; ++row) (row < R ? incl0 : agg1)[b] += r.counts[row * B + b];

  std::thread tile1([&r] { ar::queue_scan_tile(r.kg, 1, 0, 1); });
  const uint32_t* status1 = r.counts.data() + (2 * R - 1) * B;
  std::vector<uint32_t> published(B, 0), lookback(B, 0);
  bool in_time = true;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  for (uint32_t b = 0; b < B; ++b) {
    while (in_time && ar::queue_status_load(status1 + b) < ar::kScanAggregate) {
      in_time = std::chrono::steady_clock::now() < deadline;
      std::this_thread::yield();
    }
    // Tile 1 waits on tile 0's bin b: its word is exactly what it published.
    published[b] = ar::queue_status_load(status1 + b);
    // A successor's view of bin b: tile 1's published word and tile 0's inclusive prefix.
    std::vector<uint32_t> snapshot(r.counts.size(), 0);
    snapshot[(2 * R - 1) * B + b] = published[b];
    snapshot[(R - 1) * B + b] = ar::kScanPrefix | incl0[b];
    lookback[b] = ar::queue_lookback(snapshot.data(), tiles, 2, b);
    ar::queue_scan_tile(r.kg, 0, b, B);  // tile 0, bin b only: publishes its inclusive prefix
  }
  tile1.join();
  REQUIRE(in_time);
  for (uint32_t b = 0; b < B; ++b) {
    INFO("bin " << b);
    CHECK(published[b] == (ar::kScanAggregate | agg1[b]));
    CHECK(lookback[b] == incl0[b] + agg1[b]);
  }
  ar::queue_scan_tile(r.kg, 2, 0, 1);
  ar::queue_scan_tile(r.kg, 3, 0, 1);
  r.dev.launch(KernelId::QUEUE_BUILD_SCATTER, grid);
  check_against_reference(r, kMain, kShadow);
}

TEST_CASE("queue_build/crafted step table") {
  // Main: INTERSECT_CLOSEST at 2 and 5, SHADE_SURFACE at 0 and 3, TERMINATED at 1 and 4.
  // Shadow: SHADOW_TERMINATED at 0 and 2, INTERSECT_SHADOW at 1, SHADE_SHADOW at 3.
  Rig r({U8(Stage::SHADE_SURFACE), U8(Stage::TERMINATED), U8(Stage::INTERSECT_CLOSEST), U8(Stage::SHADE_SURFACE),
         U8(Stage::TERMINATED), U8(Stage::INTERSECT_CLOSEST)},
        {U8(Stage::SHADOW_TERMINATED), U8(Stage::INTERSECT_SHADOW), U8(Stage::SHADOW_TERMINATED), U8(Stage::SHADE_SHADOW)});
  r.build();
  // {start, count, shadow_base} per Stage in enum order. Main segments follow each other in
  // Stage order, so the empty ones sit at the running offset; the shadow pool has its own
  // offsets. SHADE_SURFACE (k = 1) takes both free shadow slots, queue entries 2 and 3 of
  // shadow_queue; the producers after it get an empty range starting at the next free entry, 4.
  const ar::StepEntry want[kStages] = {
      {0, 2, 0},  // INTERSECT_CLOSEST
      {2, 0, 0},  // INTERSECT_SUBSURFACE
      {0, 1, 0},  // INTERSECT_SHADOW (shadow pool)
      {2, 0, 0},  // SHADE_BACKGROUND
      {2, 0, 0},  // SHADE_LIGHT
      {2, 2, 2},  // SHADE_SURFACE
      {4, 0, 4},  // SHADE_SURFACE_EXT
      {4, 0, 4},  // SHADE_LIGHT_PATH
      {4, 0, 4},  // SHADE_LIGHT_PATH_EXT
      {1, 1, 0},  // SHADE_SHADOW (shadow pool)
      {4, 0, 4},  // SHADE_VOLUME
      {4, 0, 0},  // SHADE_SUBSURFACE
      {4, 0, 0},  // ADVANCE_GEODESIC
      {4, 2, 0},  // TERMINATED
      {2, 2, 0},  // SHADOW_TERMINATED (shadow pool)
  };
  for (uint32_t s = 0; s < kStages; ++s) {
    INFO("stage " << s);
    CHECK(r.table.stage[s].start == want[s].start);
    CHECK(r.table.stage[s].count == want[s].count);
    CHECK(r.table.stage[s].shadow_base == want[s].shadow_base);
  }
  CHECK(r.table.refill_count == 2);
  CHECK(r.table.invalid_items == 0);
  CHECK(r.table.high_water[0] == 6);
  CHECK(r.table.high_water[1] == 4);
  // main_queue: INTERSECT_CLOSEST {2, 5}, SHADE_SURFACE {0, 3}, TERMINATED {1, 4}; the rest untouched.
  const std::vector<uint32_t> main_q = {2, 5, 0, 3, 1, 4};
  CHECK(std::equal(main_q.begin(), main_q.end(), r.queue[0].begin()));
  // shadow_queue: INTERSECT_SHADOW {1}, SHADE_SHADOW {3}, SHADOW_TERMINATED {0, 2}.
  const std::vector<uint32_t> shadow_q = {1, 3, 0, 2};
  CHECK(std::equal(shadow_q.begin(), shadow_q.end(), r.queue[1].begin()));
}

TEST_CASE("queue_build/invalid next_stage values are counted and placed nowhere") {
  // Main pool: 15 and 200 are no Stage, INTERSECT_SHADOW belongs to the shadow pool.
  // Shadow pool: INTERSECT_CLOSEST belongs to the main pool, 0xFF is no Stage.
  Rig r({U8(Stage::TERMINATED), 15, U8(Stage::INTERSECT_SHADOW), 200, U8(Stage::SHADE_SURFACE)},
        {U8(Stage::INTERSECT_CLOSEST), U8(Stage::SHADOW_TERMINATED), 0xFF});
  r.build();
  CHECK(r.table.invalid_items == 5);
  CHECK(r.table.stage[S(Stage::SHADE_SURFACE)].count == 1);
  CHECK(r.table.stage[S(Stage::TERMINATED)].count == 1);
  CHECK(r.table.stage[S(Stage::INTERSECT_CLOSEST)].count == 0);
  // The shadow pool's INTERSECT_SHADOW segment holds nothing: the main-pool item with that
  // value is invalid there, not a member of it.
  CHECK(r.table.stage[S(Stage::INTERSECT_SHADOW)].count == 0);
  CHECK(r.table.stage[S(Stage::SHADOW_TERMINATED)].count == 1);
  CHECK(r.queue[0][0] == 4);  // SHADE_SURFACE segment
  CHECK(r.queue[0][1] == 0);  // TERMINATED segment
  CHECK(r.queue[0][2] == kSentinel);
  CHECK(r.queue[1][0] == 1);
  CHECK(r.queue[1][1] == kSentinel);
  check_against_reference(r, 5, 3);
}

TEST_CASE("queue_build/shadow reservation edges") {
  // step_table_finish on hand-set segment lengths with k that the kernels' table does not use.
  // Shadow pool: INTERSECT_SHADOW 6, SHADE_SHADOW 2, so the free list starts at queue entry 8.
  const auto fill = [](uint32_t free_slots, uint32_t surface, uint32_t ext, uint32_t light_path, uint32_t volume,
                       uint32_t k_surface, uint32_t k_light_path, uint32_t k_volume) {
    ar::StepTable t{};
    t.stage[S(Stage::INTERSECT_SHADOW)].count = 6;
    t.stage[S(Stage::SHADE_SHADOW)].count = 2;
    t.stage[S(Stage::SHADOW_TERMINATED)].count = free_slots;
    t.stage[S(Stage::SHADE_SURFACE)].count = surface;
    t.stage[S(Stage::SHADE_SURFACE_EXT)].count = ext;
    t.stage[S(Stage::SHADE_LIGHT_PATH)].count = light_path;
    t.stage[S(Stage::SHADE_VOLUME)].count = volume;
    t.stage[S(Stage::TERMINATED)].count = 9;
    ar::step_table_finish(t, [&](Stage s) -> uint32_t {
      switch (s) {
        case Stage::SHADE_SURFACE: return k_surface;
        case Stage::SHADE_LIGHT_PATH: return k_light_path;
        case Stage::SHADE_VOLUME: return k_volume;
        default: return 0;  // SHADE_SURFACE_EXT: not a producer here, whatever its segment
      }
    });
    return t;
  };

  {  // 11 free; surface 5 x k=2 needs 10 and fits; light path 3 x k=3 needs 9 and 1 is left: it
     // launches nothing; volume 4 x k=1 gets the last slot.
    const ar::StepTable t = fill(11, 5, 0, 3, 4, 2, 3, 1);
    CHECK(t.stage[S(Stage::SHADOW_TERMINATED)].start == 8);
    CHECK(t.stage[S(Stage::SHADE_SURFACE)].count == 5);
    CHECK(t.stage[S(Stage::SHADE_SURFACE)].shadow_base == 8);
    CHECK(t.stage[S(Stage::SHADE_LIGHT_PATH)].count == 0);
    CHECK(t.stage[S(Stage::SHADE_LIGHT_PATH)].shadow_base == 18);
    CHECK(t.stage[S(Stage::SHADE_VOLUME)].count == 1);
    CHECK(t.stage[S(Stage::SHADE_VOLUME)].shadow_base == 18);
    CHECK(t.refill_count == 9);
  }
  {  // 7 free, surface 10 x k=2: floor(7 / 2) = 3 launch and one slot is left for the volume stage.
    const ar::StepTable t = fill(7, 10, 0, 0, 4, 2, 1, 1);
    CHECK(t.stage[S(Stage::SHADE_SURFACE)].count == 3);
    CHECK(t.stage[S(Stage::SHADE_SURFACE)].shadow_base == 8);
    CHECK(t.stage[S(Stage::SHADE_LIGHT_PATH)].count == 0);
    CHECK(t.stage[S(Stage::SHADE_LIGHT_PATH)].shadow_base == 14);
    CHECK(t.stage[S(Stage::SHADE_VOLUME)].count == 1);
    CHECK(t.stage[S(Stage::SHADE_VOLUME)].shadow_base == 14);
  }
  {  // Free slots cover the segment exactly: nothing is held back, none is left.
    const ar::StepTable t = fill(10, 5, 0, 7, 0, 2, 1, 1);
    CHECK(t.stage[S(Stage::SHADE_SURFACE)].count == 5);
    CHECK(t.stage[S(Stage::SHADE_LIGHT_PATH)].count == 0);
    CHECK(t.stage[S(Stage::SHADE_LIGHT_PATH)].shadow_base == 18);
  }
  {  // No free slot: every producer launches nothing, all with base = the end of the free list.
    const ar::StepTable t = fill(0, 5, 3, 7, 4, 1, 1, 1);
    for (Stage s : {Stage::SHADE_SURFACE, Stage::SHADE_LIGHT_PATH, Stage::SHADE_VOLUME}) {
      CHECK(t.stage[S(s)].count == 0);
      CHECK(t.stage[S(s)].shadow_base == 8);
    }
    // SHADE_SURFACE_EXT is no producer in this k: its segment is launched whole.
    CHECK(t.stage[S(Stage::SHADE_SURFACE_EXT)].count == 3);
  }
  {  // An empty producer segment reserves nothing and does not move the next producer's base.
    const ar::StepTable t = fill(5, 0, 0, 3, 0, 4, 1, 1);
    CHECK(t.stage[S(Stage::SHADE_SURFACE)].shadow_base == 8);
    CHECK(t.stage[S(Stage::SHADE_LIGHT_PATH)].shadow_base == 8);
    CHECK(t.stage[S(Stage::SHADE_LIGHT_PATH)].count == 3);
  }
}

TEST_CASE("queue_build/shadow shortage drops nothing") {
  // 0002 D5 verification: a shadow pool below demand. 1000 main paths wait at SHADE_SURFACE
  // (k = 1) and the shadow pool has 100 slots, all free. Every even path emits a shadow item,
  // every odd one emits none. A step launches exactly the free slots' worth of paths, the
  // lowest indices first, every path runs once, and unused reserved slots stay free.
  constexpr uint32_t kMain = 1000, kShadow = 100;
  Rig r(std::vector<uint8_t>(kMain, U8(Stage::SHADE_SURFACE)), std::vector<uint8_t>(kShadow, U8(Stage::SHADOW_TERMINATED)));
  std::vector<uint32_t> launched(kMain, 0);
  uint32_t next_lowest = 0;  // the lowest path index that has not run yet
  uint32_t steps = 0;
  while (next_lowest < kMain) {
    REQUIRE(++steps <= 100);
    const std::vector<uint8_t> shadow_before = r.next[1];
    r.build(kMain, kShadow);
    const ar::StepEntry surface = r.table.stage[S(Stage::SHADE_SURFACE)];
    const ar::StepEntry inter = r.table.stage[S(Stage::INTERSECT_SHADOW)];
    const uint32_t free_slots = r.table.stage[S(Stage::SHADOW_TERMINATED)].count;
    const uint32_t waiting = kMain - next_lowest;
    // The launch is the free slots' worth of waiting paths, and the queue still holds all of them.
    CHECK(surface.count == std::min(waiting, free_slots));
    CHECK(r.table.invalid_items == 0);
    uint32_t in_queue = 0;
    for (uint32_t j = 0; j < kMain; ++j) in_queue += r.queue[0][j] != kSentinel;
    CHECK(in_queue == kMain);
    // Stage order: INTERSECT_SHADOW consumes last step's shadow items (they were not free).
    for (uint32_t j = 0; j < inter.count; ++j) r.next[1][r.queue[1][inter.start + j]] = U8(Stage::SHADOW_TERMINATED);
    std::vector<bool> owned_seen(kShadow, false);
    for (uint32_t i = 0; i < surface.count; ++i) {
      const uint32_t path = r.queue[0][surface.start + i];
      CHECK(path == next_lowest + i);  // stable: item-index order, the lowest first
      ++launched[path];
      const uint32_t slot = r.queue[1][surface.shadow_base + i];  // k = 1: one slot per item
      REQUIRE(slot < kShadow);
      CHECK(shadow_before[slot] == U8(Stage::SHADOW_TERMINATED));  // a free slot
      CHECK_FALSE(owned_seen[slot]);                               // owned by one path only
      owned_seen[slot] = true;
      if (path % 2 == 0) r.next[1][slot] = U8(Stage::INTERSECT_SHADOW);
      r.next[0][path] = U8(Stage::TERMINATED);
    }
    // The odd paths' slots were reserved but not used: still free.
    for (uint32_t i = 0; i < surface.count; ++i)
      if ((next_lowest + i) % 2 == 1)
        CHECK(r.next[1][r.queue[1][surface.shadow_base + i]] == U8(Stage::SHADOW_TERMINATED));
    next_lowest += surface.count;
    if (steps == 1) {  // the first step, typed from the setup: all 100 free slots, queue entries 0..99
      CHECK(surface.count == 100);
      CHECK(surface.shadow_base == 0);
      CHECK(free_slots == 100);
      CHECK(inter.count == 0);
    }
  }
  CHECK(std::all_of(launched.begin(), launched.end(), [](uint32_t n) { return n == 1; }));
}

}  // TEST_SUITE


TEST_SUITE("gate") {

TEST_CASE("queue_build/shadow reservation takes fresh slots after the free segment") {
  // RULINGS 84: the free list is the SHADOW_TERMINATED segment, then the fresh slots
  // [high_water, capacity). INTERSECT_SHADOW 6, SHADE_SHADOW 2 and 5 free slots partitioned
  // (high-water 13), capacity 30: 5 + 17 free in all.
  const auto fill = [](uint32_t capacity, uint32_t surface, uint32_t light_path, uint32_t volume) {
    ar::StepTable t{};
    t.stage[S(Stage::INTERSECT_SHADOW)].count = 6;
    t.stage[S(Stage::SHADE_SHADOW)].count = 2;
    t.stage[S(Stage::SHADOW_TERMINATED)].count = 5;
    t.stage[S(Stage::SHADE_SURFACE)].count = surface;
    t.stage[S(Stage::SHADE_LIGHT_PATH)].count = light_path;
    t.stage[S(Stage::SHADE_VOLUME)].count = volume;
    t.high_water[1] = 13;
    t.shadow_capacity = capacity;
    ar::step_table_finish(t, [](Stage s) -> uint32_t {
      return s == Stage::SHADE_LIGHT_PATH ? 2u : (s == Stage::SHADE_SURFACE || s == Stage::SHADE_VOLUME) ? 1u : 0u;
    });
    return t;
  };
  {  // Within the segment: no fresh slot.
    const ar::StepTable t = fill(30, 4, 0, 0);
    CHECK(t.stage[S(Stage::SHADE_SURFACE)].count == 4);
    CHECK(t.stage[S(Stage::SHADE_SURFACE)].shadow_base == 8);
    CHECK(t.shadow_fresh == 0);
  }
  {  // Surface 12 x 1 takes the 5 free and 7 fresh; light path 3 x 2 takes 6 more fresh (base 20);
     // volume 20 x 1 finds 4 left and launches 4: the pool is full, fresh = 17 = 30 - 13.
    const ar::StepTable t = fill(30, 12, 3, 20);
    CHECK(t.stage[S(Stage::SHADE_SURFACE)].count == 12);
    CHECK(t.stage[S(Stage::SHADE_SURFACE)].shadow_base == 8);
    CHECK(t.stage[S(Stage::SHADE_LIGHT_PATH)].count == 3);
    CHECK(t.stage[S(Stage::SHADE_LIGHT_PATH)].shadow_base == 20);
    CHECK(t.stage[S(Stage::SHADE_VOLUME)].count == 4);
    CHECK(t.stage[S(Stage::SHADE_VOLUME)].shadow_base == 26);
    CHECK(t.shadow_fresh == 17);
  }
  {  // Light path 9 x 2 needs 18 of the 22 free and launches whole; 4 are left for volume 10 x 1.
    const ar::StepTable t = fill(30, 0, 9, 10);
    CHECK(t.stage[S(Stage::SHADE_LIGHT_PATH)].count == 9);
    CHECK(t.stage[S(Stage::SHADE_VOLUME)].count == 4);
    CHECK(t.shadow_fresh == 17);
  }
  {  // k = 2 across the boundary: surface 3 x 1 takes queue entries 8-10, so light path 12 x 2 starts
     // at entry 11 with 19 free (2 in the segment, 17 fresh): floor(19 / 2) = 9 launch, its slots
     // straddle the segment's end, and the one slot left goes to volume 10 x 1 at entry 29.
    const ar::StepTable t = fill(30, 3, 12, 10);
    CHECK(t.stage[S(Stage::SHADE_SURFACE)].count == 3);
    CHECK(t.stage[S(Stage::SHADE_LIGHT_PATH)].count == 9);
    CHECK(t.stage[S(Stage::SHADE_LIGHT_PATH)].shadow_base == 11);
    CHECK(t.stage[S(Stage::SHADE_VOLUME)].count == 1);
    CHECK(t.stage[S(Stage::SHADE_VOLUME)].shadow_base == 29);
    CHECK(t.shadow_fresh == 17);
  }
  {  // No capacity above the high-water mark (or none given): the segment alone, as in 0002 D5.
    for (uint32_t capacity : {0u, 13u, 10u}) {
      const ar::StepTable t = fill(capacity, 12, 0, 0);
      CHECK(t.stage[S(Stage::SHADE_SURFACE)].count == 5);
      CHECK(t.shadow_fresh == 0);
    }
  }
}

TEST_CASE("queue_build/shadow range grows by its fresh slots") {
  // The scheduler's shadow loop by hand over queue_build (RULINGS 84): 400 paths start at
  // INTERSECT_CLOSEST and 60 more of them move to SHADE_SURFACE (k = 1) before every step, in index
  // order; the shadow pool has 100 slots and its high-water mark starts at 0. Path p emits a shadow item unless p % 3 == 0; INTERSECT_SHADOW sends an item to SHADE_SHADOW
  // when its slot is even, else frees it; SHADE_SHADOW frees it. Every step: every live item is in
  // exactly one shadow segment, no live slot is at or above the mark, the reserved slots are the
  // free list (the free slots below the mark, then the mark, mark + 1, ...), the launch is
  // min(waiting, free below + capacity - mark), and the mark grows by the fresh slots, never past 100.
  constexpr uint32_t kMain = 400, kShadow = 100;
  Rig r(std::vector<uint8_t>(kMain, U8(Stage::INTERSECT_CLOSEST)), std::vector<uint8_t>(kShadow, U8(Stage::SHADOW_TERMINATED)));
  std::vector<uint32_t> launched(kMain, 0);
  uint32_t hw = 0, next_lowest = 0, released = 0, steps = 0, held_back_steps = 0, fresh_steps = 0;
  const uint8_t kTerm = U8(Stage::SHADOW_TERMINATED);
  bool live_left = false;
  while (next_lowest < kMain || live_left) {
    REQUIRE(++steps <= 100);
    for (const uint32_t end = std::min(kMain, released + 60); released < end; ++released)
      r.next[0][released] = U8(Stage::SHADE_SURFACE);
    r.build(kMain, hw, kShadow);
    REQUIRE(r.table.invalid_items == 0);
    // The partition: live items below the mark, each exactly once.
    std::vector<uint32_t> seen(kShadow, 0);
    for (Stage st : {Stage::INTERSECT_SHADOW, Stage::SHADE_SHADOW}) {
      const ar::StepEntry e = r.table.stage[S(st)];
      for (uint32_t j = 0; j < e.count; ++j) {
        const uint32_t slot = r.queue[1][e.start + j];
        REQUIRE(slot < hw);
        CHECK(r.next[1][slot] == U8(st));
        ++seen[slot];
      }
    }
    uint32_t free_below = 0;
    std::vector<uint32_t> free_list;
    for (uint32_t slot = 0; slot < kShadow; ++slot) {
      const bool live = r.next[1][slot] != kTerm;
      CHECK(seen[slot] == (live ? 1u : 0u));
      if (live) CHECK(slot < hw);
      if (!live && slot < hw) ++free_below, free_list.push_back(slot);
    }
    for (uint32_t slot = hw; slot < kShadow; ++slot) free_list.push_back(slot);
    const ar::StepEntry surface = r.table.stage[S(Stage::SHADE_SURFACE)];
    const uint32_t waiting = released - next_lowest;
    CHECK(surface.count == std::min(waiting, free_below + (kShadow - hw)));
    held_back_steps += surface.count < waiting;
    // Stages in Stage order: INTERSECT_SHADOW, then SHADE_SHADOW, then the producer.
    const ar::StepEntry inter = r.table.stage[S(Stage::INTERSECT_SHADOW)];
    const ar::StepEntry shade = r.table.stage[S(Stage::SHADE_SHADOW)];
    std::vector<uint32_t> shaded(r.queue[1].begin() + shade.start, r.queue[1].begin() + shade.start + shade.count);
    for (uint32_t j = 0; j < inter.count; ++j) {
      const uint32_t slot = r.queue[1][inter.start + j];
      r.next[1][slot] = slot % 2 == 0 ? U8(Stage::SHADE_SHADOW) : kTerm;
    }
    for (uint32_t slot : shaded) r.next[1][slot] = kTerm;
    for (uint32_t i = 0; i < surface.count; ++i) {
      const uint32_t path = r.queue[0][surface.start + i];
      CHECK(path == next_lowest + i);
      ++launched[path];
      const uint32_t slot = r.queue[1][surface.shadow_base + i];
      REQUIRE(i < free_list.size());
      CHECK(slot == free_list[i]);  // the free slots below the mark first, then fresh, in index order
      if (path % 3 != 0) r.next[1][slot] = U8(Stage::INTERSECT_SHADOW);
      r.next[0][path] = U8(Stage::TERMINATED);
    }
    next_lowest += surface.count;
    const uint32_t want_fresh = surface.count > free_below ? surface.count - free_below : 0;
    CHECK(r.table.shadow_fresh == want_fresh);
    fresh_steps += want_fresh > 0;
    hw += r.table.shadow_fresh;
    REQUIRE(hw <= kShadow);
    live_left = std::any_of(r.next[1].begin(), r.next[1].end(), [&](uint8_t v) { return v != kTerm; });
  }
  CHECK(std::all_of(launched.begin(), launched.end(), [](uint32_t n) { return n == 1; }));
  CHECK(fresh_steps >= 2);      // the mark grew over more than one step
  CHECK(held_back_steps >= 1);  // and the pool ran out, so producers were held back
  CHECK(hw == kShadow);
}

TEST_CASE("queue_build/live_end is the end of the last tile with a live slot") {
  // RULINGS 85: per pool, the end of the last kQueueTile tile below the mark holding a slot that is not
  // the pool's TERMINATED value (the largest such slot + 1 rounded up to its tile's end, the last tile
  // ending at the mark), computed here the slow way, on random pools whose tail from a cut on is
  // terminated. 40 tiles per pool make 80 histogram rows, three scan tiles of 32 with the pool
  // boundary inside the second, so the max runs over several scan tiles. Cuts and marks sit on and
  // beside tile and scan-tile boundaries. Then the queue entries from live_end up to the mark are
  // live_end, live_end + 1, ... (the fresh-entry contract a lowered mark relies on).
  constexpr uint32_t kN = 40 * ar::kQueueTile;
  const uint8_t dead[2] = {U8(Stage::TERMINATED), U8(Stage::SHADOW_TERMINATED)};
  const uint32_t cuts[] = {0, 1, 1023, 1024, 1025, 32767, 32768, 32769, kN - 1, kN};
  uint32_t lowered = 0;
  for (uint32_t cut : cuts) {
    std::vector<uint8_t> pools[2] = {random_pool(kN, Pool::MAIN, 11 + cut), random_pool(kN, Pool::SHADOW, 12 + cut)};
    for (int p = 0; p < 2; ++p) {
      std::fill(pools[p].begin() + cut, pools[p].end(), dead[p]);
      if (cut > 0) pools[p][cut - 1] = p == 0 ? U8(Stage::SHADE_SURFACE) : U8(Stage::INTERSECT_SHADOW);  // the last live slot
    }
    Rig r(pools[0], pools[1], 16);
    for (uint32_t mark : {kN, cut, cut + 1 < kN ? cut + 1 : kN, cut > 1 ? cut - 1 : 0u, 1024u}) {
      INFO("cut " << cut << " mark " << mark);
      r.build(mark, mark, kN);
      for (int p = 0; p < 2; ++p) {
        uint32_t last_live = 0;
        for (uint32_t i = 0; i < mark; ++i)
          if (r.next[p][i] != dead[p]) last_live = i + 1;
        const uint32_t tile_end = (last_live + ar::kQueueTile - 1) / ar::kQueueTile * ar::kQueueTile;
        const uint32_t want = std::min(tile_end, mark);
        CHECK(r.table.live_end[p] == want);
        lowered += want < mark;
      }
      for (int p = 0; p < 2; ++p) {
        bool identity = true;
        for (uint32_t j = r.table.live_end[p]; j < mark; ++j) identity = identity && r.queue[p][j] == j;
        CHECK(identity);
      }
    }
  }
  CHECK(lowered > 20);
}

}  // TEST_SUITE
