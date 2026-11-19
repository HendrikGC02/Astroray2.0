// SPDX-License-Identifier: Apache-2.0
// sort on the GPU: the three kernels, loaded from the cubins embedded in this executable and
// launched through CudaDevice, produce the same sorted segment as the CPU device for the same
// scene and queue. The sort is integer code, so agreement is bit-exact (0009 d17(i)).
// Ref: docs/decisions/0002-kernel-architecture.md D4, D6, D7 step 3, D14; 0009 d3, d17(i).
#include <doctest.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "device/cpu/cpu_device.h"
#include "host/data_resource.h"
#include "host/device.h"
#include "host/thread_pool.h"
#include "stages/sort.h"

using namespace astroray;
using namespace astroray::host;
using ar::Stage;

namespace {

constexpr uint32_t kHeaders = 5;
constexpr uint32_t kTriangles = 256;
constexpr uint32_t kSegmentStart = 7;
constexpr uint32_t kInstances = 6;

struct Rng {
  uint64_t x;
  uint32_t next() {
    uint64_t z = (x += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return static_cast<uint32_t>((z ^ (z >> 31)) >> 16);
  }
};

// One input: hit per path slot and the queue order. Scene: one mesh whose triangle t has slot
// t % per_instance; instance i has a slice of per_instance pool entries holding random header
// ids (or, with `cover`, entry k of the pool holds header k % headers, so every material
// occurs); the last instance has no slice (default material).
struct Input {
  uint32_t count, headers, per_instance;
  std::vector<uint32_t> hit_inst, hit_prim, queue;
  std::vector<int32_t> tri_slot;
  std::vector<uint32_t> slots;
  std::vector<ar::DeviceInstanceRecord> instances;
  std::vector<ar::MaterialHeader> header_table;

  // The material id of path slot p by the tables above (headers = the default material).
  uint32_t material_of(uint32_t p) const {
    if (hit_inst[p] + 1 == kInstances) return headers;
    return slots[hit_inst[p] * per_instance + uint32_t(tri_slot[hit_prim[p]])];
  }
};

Input make_input(uint32_t count, uint64_t seed, uint32_t headers = kHeaders, uint32_t per_instance = 4,
                 bool cover = false) {
  Rng rng{seed};
  Input in;
  in.count = count, in.headers = headers, in.per_instance = per_instance;
  for (uint32_t t = 0; t < kTriangles; ++t) in.tri_slot.push_back(int32_t(t % per_instance));
  in.instances.resize(kInstances);
  for (uint32_t i = 0; i < kInstances; ++i) {
    auto& r = in.instances[i];
    r = {};
    r.light_index = ar::kNoLightIndex;
    r.material_offset = i * per_instance;
    r.material_count = i + 1 == kInstances ? 0u : per_instance;
    for (uint32_t s = 0; s < per_instance; ++s)
      in.slots.push_back(cover ? uint32_t(in.slots.size()) % headers : rng.next() % headers);
  }
  in.header_table.assign(headers, ar::MaterialHeader{0, 0, 0, 0});
  in.hit_inst.resize(count), in.hit_prim.resize(count);
  for (uint32_t i = 0; i < count; ++i) in.hit_inst[i] = rng.next() % kInstances, in.hit_prim[i] = rng.next() % kTriangles;
  in.queue.assign(kSegmentStart + count, 0xFFFFFFFFu);
  std::vector<uint32_t> order(count);
  for (uint32_t i = 0; i < count; ++i) order[i] = i;
  for (uint32_t i = count; i > 1; --i) std::swap(order[i - 1], order[rng.next() % i]);
  for (uint32_t i = 0; i < count; ++i) in.queue[kSegmentStart + i] = order[i];
  return in;
}

template <class T>
DeviceBuffer put(Device& dev, LedgerCategory cat, const std::vector<T>& v) {
  DeviceBuffer b = dev.allocate(cat, sizeof(T) * (v.empty() ? 1 : v.size()), Owner{1, 0});
  if (!v.empty()) dev.upload(b, 0, v.data(), sizeof(T) * v.size());
  return b;
}

// Uploads the input to `dev`, runs the sort kernels and reads the sorted segment back. Count and
// scatter get `grid` (the CPU's item range, or the GPU's grid size).
std::vector<uint32_t> run_sort(Device& dev, const Input& in, uint32_t grid) {
  const uint32_t tiles = ar::queue_tiles(in.count);
  DeviceBuffer inst = put(dev, LedgerCategory::TABLES, in.instances);
  DeviceBuffer slots = put(dev, LedgerCategory::TABLES, in.slots);
  DeviceBuffer headers = put(dev, LedgerCategory::TABLES, in.header_table);
  DeviceBuffer tri = put(dev, LedgerCategory::TABLES, in.tri_slot);
  DeviceBuffer hit_inst = put(dev, LedgerCategory::MAIN_PATH_POOL, in.hit_inst);
  DeviceBuffer hit_prim = put(dev, LedgerCategory::MAIN_PATH_POOL, in.hit_prim);
  DeviceBuffer queue = put(dev, LedgerCategory::QUEUE_SORT_INDICES, in.queue);
  std::vector<uint32_t> blank(in.count, 0xFFFFFFFFu);
  DeviceBuffer sorted = put(dev, LedgerCategory::QUEUE_SORT_INDICES, blank);
  DeviceBuffer counts = dev.allocate(LedgerCategory::QUEUE_SORT_INDICES, sizeof(uint32_t) * 2 * tiles * (in.headers + 1), Owner{1, 0});

  ar::DeviceMeshView mesh{};
  mesh.tri_material = reinterpret_cast<uintptr_t>(tri.ptr);
  mesh.triangle_count = kTriangles;
  DeviceBuffer mesh_buf = put(dev, LedgerCategory::TABLES, std::vector<ar::DeviceMeshView>{mesh});
  ar::SceneData scene{};
  scene.instances = static_cast<const ar::DeviceInstanceRecord*>(inst.ptr);
  scene.instance_count = kInstances;
  scene.meshes = static_cast<const ar::DeviceMeshView*>(mesh_buf.ptr);
  scene.mesh_count = 1;
  scene.material_slots = static_cast<const uint32_t*>(slots.ptr);
  scene.material_headers = static_cast<const ar::MaterialHeader*>(headers.ptr);
  scene.material_header_count = in.headers;
  DeviceBuffer scene_buf = put(dev, LedgerCategory::TABLES, std::vector<ar::SceneData>{scene});

  ar::PathStateSoA path{};
  path.hit_inst = static_cast<uint32_t*>(hit_inst.ptr);
  path.hit_prim = static_cast<uint32_t*>(hit_prim.ptr);
  DeviceBuffer path_buf = put(dev, LedgerCategory::TABLES, std::vector<ar::PathStateSoA>{path});
  ar::StepTable table_in{};
  table_in.stage[uint32_t(Stage::SHADE_SURFACE)].start = kSegmentStart;
  table_in.stage[uint32_t(Stage::SHADE_SURFACE)].count = in.count;
  DeviceBuffer table_buf = put(dev, LedgerCategory::TABLES, std::vector<ar::StepTable>{table_in});

  ar::KernelGlobals kg{};
  kg.path = static_cast<const ar::PathStateSoA*>(path_buf.ptr);
  kg.step_table = static_cast<const ar::StepTable*>(table_buf.ptr);
  kg.scene = static_cast<const ar::SceneData*>(scene_buf.ptr);
  kg.main_queue = static_cast<const uint32_t*>(queue.ptr);
  kg.sort_queue = static_cast<uint32_t*>(sorted.ptr);
  kg.sort_counts = static_cast<uint32_t*>(counts.ptr);
  DeviceBuffer kg_buf = put(dev, LedgerCategory::TABLES, std::vector<ar::KernelGlobals>{kg});
  dev.set_kernel_globals(kg_buf);

  dev.launch(KernelId::SORT_COUNT, grid);
  dev.launch(KernelId::SORT_SCAN, ar::sort_scan_grid(in.count));
  dev.launch(KernelId::SORT_SCATTER, grid);
  dev.synchronize();
  std::vector<uint32_t> out(in.count);
  dev.download(out.data(), sorted, 0, sizeof(uint32_t) * in.count);
  for (DeviceBuffer* b : {&inst, &slots, &headers, &tri, &hit_inst, &hit_prim, &queue, &sorted, &counts, &mesh_buf,
                          &scene_buf, &path_buf, &table_buf, &kg_buf})
    dev.free(*b);
  return out;
}

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("sort/embedded kernels load") {
  for (const char* name : {"K_SORT_COUNT", "K_SORT_SCAN", "K_SORT_SCATTER"}) {
    INFO(name);
    DataBlob blob;
    REQUIRE(find_data(name, &blob));
    CHECK(blob.size > 1000);
  }
}

TEST_CASE("sort/cpu gpu agree") {
  Ledger gpu_ledger, cpu_ledger;
  std::unique_ptr<ModuleDevice> gpu = make_cuda_device(gpu_ledger, 0);
  ContextState ctx;
  ThreadPool pool(ctx, 8);
  CpuDevice cpu(cpu_ledger, pool);
  cpu.register_kernel(KernelId::SORT_COUNT, &cpu_sort_count);
  cpu.register_kernel(KernelId::SORT_SCAN, &cpu_sort_scan);
  cpu.register_kernel(KernelId::SORT_SCATTER, &cpu_sort_scatter);
  // 300,000 items (293 tiles, the last partial) and a segment of one partial tile.
  uint64_t diff = 0;
  for (uint32_t n : {300000u, 777u}) {
    const Input in = make_input(n, 50 + n);
    const std::vector<uint32_t> g = run_sort(*gpu, in, ar::gpu_tile_grid(n));  // as the scheduler launches it
    const std::vector<uint32_t> c = run_sort(cpu, in, ar::sort_grid(n));
    REQUIRE(g.size() == c.size());
    for (size_t i = 0; i < g.size(); ++i) diff += g[i] != c[i];
  }
  std::printf("sort: cpu gpu differences=%llu\n", static_cast<unsigned long long>(diff));
  CHECK(diff == 0);
  CHECK(gpu_ledger.total() == 0);
  CHECK(cpu_ledger.total() == 0);
}

TEST_CASE("sort/300 materials cpu gpu agree with a bucket reference") {
  // 1,000,003 items (977 tiles, 31 scan tiles, 301 bins, every material hit): the GPU's
  // look-back scan runs across many warps. Both devices must equal the reference, which appends
  // each segment item to its material's bucket in segment order and concatenates the buckets.
  Ledger gpu_ledger, cpu_ledger;
  std::unique_ptr<ModuleDevice> gpu = make_cuda_device(gpu_ledger, 0);
  ContextState ctx;
  ThreadPool pool(ctx, 8);
  CpuDevice cpu(cpu_ledger, pool);
  cpu.register_kernel(KernelId::SORT_COUNT, &cpu_sort_count);
  cpu.register_kernel(KernelId::SORT_SCAN, &cpu_sort_scan);
  cpu.register_kernel(KernelId::SORT_SCATTER, &cpu_sort_scatter);
  const Input in = make_input(1000003u, 71, 300, 64, true);
  std::vector<std::vector<uint32_t>> bucket(in.headers + 1);
  for (uint32_t i = 0; i < in.count; ++i) {
    const uint32_t p = in.queue[kSegmentStart + i];
    bucket[in.material_of(p)].push_back(p);
  }
  std::vector<uint32_t> want;
  uint32_t empty = 0;
  for (const std::vector<uint32_t>& b : bucket) empty += b.empty(), want.insert(want.end(), b.begin(), b.end());
  CHECK(empty == 0);
  const std::vector<uint32_t> g = run_sort(*gpu, in, ar::gpu_tile_grid(in.count));
  const std::vector<uint32_t> c = run_sort(cpu, in, ar::sort_grid(in.count));
  REQUIRE(g.size() == want.size());
  REQUIRE(c.size() == want.size());
  uint64_t gpu_wrong = 0, cpu_wrong = 0;
  for (size_t i = 0; i < want.size(); ++i) gpu_wrong += g[i] != want[i], cpu_wrong += c[i] != want[i];
  std::printf("sort 300 materials: gpu wrong=%llu cpu wrong=%llu\n", static_cast<unsigned long long>(gpu_wrong),
              static_cast<unsigned long long>(cpu_wrong));
  CHECK(gpu_wrong == 0);
  CHECK(cpu_wrong == 0);
  CHECK(gpu_ledger.total() == 0);
  CHECK(cpu_ledger.total() == 0);
}

TEST_CASE("sort/a warp strides over several tiles") {
  // One block of 8 warps for 293 tiles: each warp sorts its tile, then strides by the warp count to
  // the next, so a warp handles about 37 tiles. The result must equal the CPU's.
  Ledger gpu_ledger, cpu_ledger;
  std::unique_ptr<ModuleDevice> gpu = make_cuda_device(gpu_ledger, 0);
  ContextState ctx;
  ThreadPool pool(ctx, 8);
  CpuDevice cpu(cpu_ledger, pool);
  cpu.register_kernel(KernelId::SORT_COUNT, &cpu_sort_count);
  cpu.register_kernel(KernelId::SORT_SCAN, &cpu_sort_scan);
  cpu.register_kernel(KernelId::SORT_SCATTER, &cpu_sort_scatter);
  constexpr uint32_t n = 300000u;
  REQUIRE(ar::queue_tiles(n) > ar::kLaunch_k_sort_count.threads / ar::kQueueLanes);
  const Input in = make_input(n, 913, 300, 64, true);
  const std::vector<uint32_t> g = run_sort(*gpu, in, 1u);  // one block
  const std::vector<uint32_t> c = run_sort(cpu, in, ar::sort_grid(n));
  REQUIRE(g.size() == c.size());
  uint64_t diff = 0;
  for (size_t i = 0; i < g.size(); ++i) diff += g[i] != c[i];
  CHECK(diff == 0);
  CHECK(gpu_ledger.total() == 0);
  CHECK(cpu_ledger.total() == 0);
}

}  // TEST_SUITE
