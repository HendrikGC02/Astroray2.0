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
#include <cstdint>
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

  // One queue_build over the first hw_main / hw_shadow items of the pools.
  void build(uint32_t hw_main, uint32_t hw_shadow) {
    for (int p = 0; p < 2; ++p) std::fill(queue[p].begin(), queue[p].end(), kSentinel);
    table.high_water[0] = hw_main;
    table.high_water[1] = hw_shadow;
    const uint32_t grid = ar::queue_build_grid(hw_main, hw_shadow);
    dev.launch(KernelId::QUEUE_BUILD_COUNT, grid);
    dev.launch(KernelId::QUEUE_BUILD_SCAN, 1);
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
