// SPDX-License-Identifier: Apache-2.0
// sort: the SHADE_SURFACE segment comes out ordered by material id, items of one material keep
// their segment order (stability), and nothing outside the segment is touched. The expected
// order is std::stable_sort over material ids computed from the scene tables typed below, never
// read back from the code under test.
// Ref: docs/decisions/0002-kernel-architecture.md D6, D7 step 3 (sort rule), D8.
#include <doctest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "device/cpu/cpu_device.h"
#include "host/thread_pool.h"
#include "stages/sort.h"

using namespace astroray;
using namespace astroray::host;
using ar::Stage;

namespace {

constexpr uint32_t kSentinel = 0xFFFFFFFFu;
constexpr uint32_t kHeaders = 3;       // material table entries 0..2; the default material is id 3
constexpr uint32_t kTriangles = 64;
constexpr uint32_t kSegmentStart = 5;  // items before the segment must stay untouched

struct Rng {
  uint64_t x;
  uint32_t next() {
    uint64_t z = (x += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return static_cast<uint32_t>((z ^ (z >> 31)) >> 16);
  }
};

// Instances: 0 and 1 carry slices of the pool {2, 0, 2, 1} and {1, 1} ; 2 has no slice (default
// material). The mesh's triangle t uses slot t % 4 (instance 0), t % 2 (instance 1).
struct Rig {
  ContextState ctx;
  Ledger ledger;
  ThreadPool pool;
  CpuDevice dev;
  std::vector<int32_t> tri_slot4, tri_slot2;
  std::vector<ar::DeviceMeshView> meshes{2};
  std::vector<ar::DeviceInstanceRecord> instances{3};
  std::vector<uint32_t> slots{2, 0, 2, 1, 1, 1};
  std::vector<ar::MaterialHeader> headers{{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
  ar::SceneData scene{};
  std::vector<uint32_t> hit_inst, hit_prim, queue, sorted, counts;
  ar::StepTable table{};
  ar::PathStateSoA path{};
  ar::ShadowItemSoA shadow{};
  ar::KernelGlobals kg{};
  DeviceBuffer kg_buf;
  uint32_t count;

  Rig(uint32_t segment_count, bool with_materials, uint64_t seed) : pool(ctx, 8), dev(ledger, pool), count(segment_count) {
    for (uint32_t t = 0; t < kTriangles; ++t) tri_slot4.push_back(int32_t(t % 4)), tri_slot2.push_back(int32_t(t % 2));
    meshes[0] = {}, meshes[1] = {};
    meshes[0].tri_material = uint64_t(reinterpret_cast<uintptr_t>(tri_slot4.data())), meshes[0].triangle_count = kTriangles;
    meshes[1].tri_material = uint64_t(reinterpret_cast<uintptr_t>(tri_slot2.data())), meshes[1].triangle_count = kTriangles;
    for (auto& r : instances) r = {}, r.light_index = ar::kNoLightIndex;
    instances[0].mesh = 0, instances[0].material_offset = 0, instances[0].material_count = 4;
    instances[1].mesh = 1, instances[1].material_offset = 4, instances[1].material_count = 2;
    instances[2].mesh = 0, instances[2].material_count = 0;
    scene.instances = instances.data(), scene.instance_count = 3;
    scene.meshes = meshes.data(), scene.mesh_count = 2;
    if (with_materials) {
      scene.material_slots = slots.data(), scene.material_headers = headers.data();
      scene.material_header_count = kHeaders;
    }
    // Path slots 0..count-1 with random hits, queued in a random order (a permutation).
    Rng rng{seed};
    const uint32_t total = kSegmentStart + count;
    hit_inst.resize(count), hit_prim.resize(count);
    for (uint32_t i = 0; i < count; ++i) hit_inst[i] = rng.next() % 3, hit_prim[i] = rng.next() % kTriangles;
    queue.assign(total, kSentinel);
    std::vector<uint32_t> order(count);
    for (uint32_t i = 0; i < count; ++i) order[i] = i;
    for (uint32_t i = count; i > 1; --i) std::swap(order[i - 1], order[rng.next() % i]);
    for (uint32_t i = 0; i < count; ++i) queue[kSegmentStart + i] = order[i];
    sorted.assign(count + 1, kSentinel);
    const uint32_t tiles = ar::queue_tiles(count);
    counts.assign(size_t(2) * tiles * (scene.material_header_count + 1) + 1, 0);
    table.stage[uint32_t(Stage::SHADE_SURFACE)].start = kSegmentStart;
    table.stage[uint32_t(Stage::SHADE_SURFACE)].count = count;
    path.hit_inst = hit_inst.data(), path.hit_prim = hit_prim.data();
    kg.path = &path, kg.shadow = &shadow, kg.step_table = &table, kg.scene = &scene;
    kg.main_queue = queue.data(), kg.sort_queue = sorted.data(), kg.sort_counts = counts.data();
    dev.register_kernel(KernelId::SORT_COUNT, &cpu_sort_count);
    dev.register_kernel(KernelId::SORT_SCAN, &cpu_sort_scan);
    dev.register_kernel(KernelId::SORT_SCATTER, &cpu_sort_scatter);
    kg_buf = dev.allocate(LedgerCategory::TABLES, sizeof kg, {1, 1});
    dev.upload(kg_buf, 0, &kg, sizeof kg);
    dev.set_kernel_globals(kg_buf);
  }
  ~Rig() { dev.free(kg_buf); }

  void run() {
    const uint32_t grid = ar::sort_grid(count);
    dev.launch(KernelId::SORT_COUNT, grid);
    dev.launch(KernelId::SORT_SCAN, ar::sort_scan_grid(count));
    dev.launch(KernelId::SORT_SCATTER, grid);
  }

  // Material ids by the tables above (3 = default material).
  uint32_t material_of(uint32_t slot) const {
    if (scene.material_headers == nullptr) return kHeaders;
    const uint32_t inst = hit_inst[slot], prim = hit_prim[slot];
    if (inst == 0) return slots[prim % 4];
    if (inst == 1) return slots[4 + prim % 2];
    return kHeaders;
  }
};

std::vector<uint32_t> expected_order(const Rig& r) {
  std::vector<uint32_t> v(r.queue.begin() + kSegmentStart, r.queue.end());
  std::stable_sort(v.begin(), v.end(), [&](uint32_t a, uint32_t b) { return r.material_of(a) < r.material_of(b); });
  return v;
}

// A scene with kWideHeaders materials, all of them hit: one mesh whose triangle t has slot
// t % kWideSlots; instance 0's slice maps slot s to header s % kWideHeaders, instance 1 has no
// slice (the default material, id kWideHeaders). Used with a segment of over a million items,
// so the scan spans many scan tiles and every material run crosses tile boundaries.
constexpr uint32_t kWideHeaders = 300;
constexpr uint32_t kWideSlots = 512;
constexpr uint32_t kWideTriangles = 1024;

struct WideRig {
  ContextState ctx;
  Ledger ledger;
  ThreadPool pool;
  CpuDevice dev;
  std::vector<int32_t> tri_slot;
  ar::DeviceMeshView mesh{};
  std::vector<ar::DeviceInstanceRecord> instances{2};
  std::vector<uint32_t> slots;
  std::vector<ar::MaterialHeader> headers = std::vector<ar::MaterialHeader>(kWideHeaders, ar::MaterialHeader{0, 0, 0, 0});
  ar::SceneData scene{};
  std::vector<uint32_t> hit_inst, hit_prim, queue, sorted, counts;
  ar::StepTable table{};
  ar::PathStateSoA path{};
  ar::ShadowItemSoA shadow{};
  ar::KernelGlobals kg{};
  DeviceBuffer kg_buf;
  uint32_t count;

  WideRig(uint32_t segment_count, uint64_t seed) : pool(ctx, 8), dev(ledger, pool), count(segment_count) {
    for (uint32_t t = 0; t < kWideTriangles; ++t) tri_slot.push_back(int32_t(t % kWideSlots));
    for (uint32_t s = 0; s < kWideSlots; ++s) slots.push_back(s % kWideHeaders);
    mesh.tri_material = uint64_t(reinterpret_cast<uintptr_t>(tri_slot.data())), mesh.triangle_count = kWideTriangles;
    for (auto& r : instances) r = {}, r.light_index = ar::kNoLightIndex;
    instances[0].material_count = kWideSlots;
    scene.instances = instances.data(), scene.instance_count = 2;
    scene.meshes = &mesh, scene.mesh_count = 1;
    scene.material_slots = slots.data(), scene.material_headers = headers.data();
    scene.material_header_count = kWideHeaders;
    Rng rng{seed};
    hit_inst.resize(count), hit_prim.resize(count);
    for (uint32_t i = 0; i < count; ++i) hit_inst[i] = rng.next() % 8 == 0 ? 1u : 0u, hit_prim[i] = rng.next() % kWideTriangles;
    queue.assign(kSegmentStart + count, kSentinel);
    for (uint32_t i = 0; i < count; ++i) queue[kSegmentStart + i] = i;
    for (uint32_t i = count; i > 1; --i) std::swap(queue[kSegmentStart + i - 1], queue[kSegmentStart + rng.next() % i]);
    sorted.assign(count + 1, kSentinel);
    counts.assign(size_t(2) * ar::queue_tiles(count) * (kWideHeaders + 1), 0);
    table.stage[uint32_t(Stage::SHADE_SURFACE)].start = kSegmentStart;
    table.stage[uint32_t(Stage::SHADE_SURFACE)].count = count;
    path.hit_inst = hit_inst.data(), path.hit_prim = hit_prim.data();
    kg.path = &path, kg.shadow = &shadow, kg.step_table = &table, kg.scene = &scene;
    kg.main_queue = queue.data(), kg.sort_queue = sorted.data(), kg.sort_counts = counts.data();
    dev.register_kernel(KernelId::SORT_COUNT, &cpu_sort_count);
    dev.register_kernel(KernelId::SORT_SCAN, &cpu_sort_scan);
    dev.register_kernel(KernelId::SORT_SCATTER, &cpu_sort_scatter);
    kg_buf = dev.allocate(LedgerCategory::TABLES, sizeof kg, {1, 1});
    dev.upload(kg_buf, 0, &kg, sizeof kg);
    dev.set_kernel_globals(kg_buf);
  }
  ~WideRig() { dev.free(kg_buf); }

  uint32_t material_of(uint32_t slot) const {
    return hit_inst[slot] == 1 ? kWideHeaders : slots[uint32_t(tri_slot[hit_prim[slot]])];
  }
};

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("sort/stable by material") {
  // 5,000 items: four tiles, the last partial; random hits with many duplicate materials.
  Rig r(5000, true, 21);
  r.run();
  const std::vector<uint32_t> want = expected_order(r);
  REQUIRE(want.size() == 5000);
  uint32_t wrong = 0;
  for (uint32_t i = 0; i < 5000; ++i) wrong += r.sorted[i] != want[i];
  CHECK(wrong == 0);
  CHECK(r.sorted[5000] == kSentinel);  // nothing written past the segment
  // The reference really exercises ties: every one of the four ids occurs.
  std::vector<uint32_t> per(kHeaders + 1, 0);
  for (uint32_t s : want) ++per[r.material_of(s)];
  for (uint32_t c : per) CHECK(c > 100);
}

TEST_CASE("sort/single tile and exact tile boundary") {
  for (uint32_t n : {1u, 1023u, 1024u, 1025u}) {
    INFO(n);
    Rig r(n, true, 30 + n);
    r.run();
    const std::vector<uint32_t> want = expected_order(r);
    for (uint32_t i = 0; i < n; ++i) CHECK(r.sorted[i] == want[i]);
  }
}

TEST_CASE("sort/300 materials and a million items match a bucket reference") {
  // 1,000,003 items: 977 tiles (the last partial), 31 scan tiles; 301 bins. The reference
  // appends each segment item to its material's bucket in segment order and concatenates the
  // buckets: the stable counting sort's definition, computed without tiles or the scan.
  const uint32_t n = 1000003;
  WideRig r(n, 61);
  const uint32_t grid = ar::sort_grid(n);
  r.dev.launch(KernelId::SORT_COUNT, grid);
  r.dev.launch(KernelId::SORT_SCAN, ar::sort_scan_grid(n));
  r.dev.launch(KernelId::SORT_SCATTER, grid);
  std::vector<std::vector<uint32_t>> bucket(kWideHeaders + 1);
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t slot = r.queue[kSegmentStart + i];
    bucket[r.material_of(slot)].push_back(slot);
  }
  uint32_t at = 0, wrong = 0, empty = 0;
  for (const std::vector<uint32_t>& b : bucket) {
    empty += b.empty();
    for (uint32_t s : b) wrong += r.sorted[at++] != s;
  }
  CHECK(at == n);
  CHECK(wrong == 0);
  CHECK(empty == 0);                   // every material and the default bin occur
  CHECK(r.sorted[n] == kSentinel);     // nothing written past the segment
}

TEST_CASE("sort/no material tables keeps the segment order") {
  Rig r(3000, false, 41);
  r.run();
  for (uint32_t i = 0; i < 3000; ++i) CHECK(r.sorted[i] == r.queue[kSegmentStart + i]);
}

TEST_CASE("sort/thresholds are the provisional values of 0002 D7") {
  CHECK(ar::kSortMinItems == 65536);
  CHECK(ar::kSortMinMaterials == 2);
}

TEST_CASE("sort/one real material plus default hits stays unsorted") {
  // The default-material bin does not count toward kSortMinMaterials (lead ruling on 0002 D7).
  Rig r(ar::kSortMinItems, true, 51);
  r.scene.material_header_count = 1;
  CHECK(!ar::sort_wanted(r.kg));
  r.scene.material_header_count = 2;
  CHECK(ar::sort_wanted(r.kg));
  r.table.stage[uint32_t(Stage::SHADE_SURFACE)].count = ar::kSortMinItems - 1;
  CHECK(!ar::sort_wanted(r.kg));
}

}  // TEST_SUITE
