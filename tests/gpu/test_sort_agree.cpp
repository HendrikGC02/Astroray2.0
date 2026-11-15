// SPDX-License-Identifier: Apache-2.0
// sort on the GPU: the two kernels, loaded from the cubins embedded in this executable and
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
// t % 4; instance i has a slice of 4 pool entries holding random header ids; the last instance
// has no slice (default material).
struct Input {
  uint32_t count;
  std::vector<uint32_t> hit_inst, hit_prim, queue;
  std::vector<int32_t> tri_slot;
  std::vector<uint32_t> slots;
  std::vector<ar::DeviceInstanceRecord> instances;
  std::vector<ar::MaterialHeader> headers;
};

constexpr uint32_t kInstances = 6;

Input make_input(uint32_t count, uint64_t seed) {
  Rng rng{seed};
  Input in;
  in.count = count;
  for (uint32_t t = 0; t < kTriangles; ++t) in.tri_slot.push_back(int32_t(t % 4));
  in.instances.resize(kInstances);
  for (uint32_t i = 0; i < kInstances; ++i) {
    auto& r = in.instances[i];
    r = {};
    r.light_index = ar::kNoLightIndex;
    r.material_offset = i * 4;
    r.material_count = i + 1 == kInstances ? 0u : 4u;
    for (uint32_t s = 0; s < 4; ++s) in.slots.push_back(rng.next() % kHeaders);
  }
  in.headers.assign(kHeaders, ar::MaterialHeader{0, 0, 0, 0});
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

// Uploads the input to `dev`, runs both sort kernels and reads the sorted segment back.
std::vector<uint32_t> run_sort(Device& dev, const Input& in) {
  const uint32_t tiles = ar::queue_tiles(in.count);
  DeviceBuffer inst = put(dev, LedgerCategory::TABLES, in.instances);
  DeviceBuffer slots = put(dev, LedgerCategory::TABLES, in.slots);
  DeviceBuffer headers = put(dev, LedgerCategory::TABLES, in.headers);
  DeviceBuffer tri = put(dev, LedgerCategory::TABLES, in.tri_slot);
  DeviceBuffer hit_inst = put(dev, LedgerCategory::MAIN_PATH_POOL, in.hit_inst);
  DeviceBuffer hit_prim = put(dev, LedgerCategory::MAIN_PATH_POOL, in.hit_prim);
  DeviceBuffer queue = put(dev, LedgerCategory::QUEUE_SORT_INDICES, in.queue);
  std::vector<uint32_t> blank(in.count, 0xFFFFFFFFu);
  DeviceBuffer sorted = put(dev, LedgerCategory::QUEUE_SORT_INDICES, blank);
  DeviceBuffer counts = dev.allocate(LedgerCategory::QUEUE_SORT_INDICES, sizeof(uint32_t) * 2 * tiles * (kHeaders + 1), Owner{1, 0});

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
  scene.material_header_count = kHeaders;
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

  const uint32_t grid = ar::sort_grid(in.count);
  dev.launch(KernelId::SORT_COUNT, grid);
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
  for (const char* name : {"K_SORT_COUNT", "K_SORT_SCATTER"}) {
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
  cpu.register_kernel(KernelId::SORT_SCATTER, &cpu_sort_scatter);
  // 300,000 items (293 tiles, the last partial) and a segment of one partial tile.
  uint64_t diff = 0;
  for (uint32_t n : {300000u, 777u}) {
    const Input in = make_input(n, 50 + n);
    const std::vector<uint32_t> g = run_sort(*gpu, in);
    const std::vector<uint32_t> c = run_sort(cpu, in);
    REQUIRE(g.size() == c.size());
    for (size_t i = 0; i < g.size(); ++i) diff += g[i] != c[i];
  }
  std::printf("sort: cpu gpu differences=%llu\n", static_cast<unsigned long long>(diff));
  CHECK(diff == 0);
  CHECK(gpu_ledger.total() == 0);
  CHECK(cpu_ledger.total() == 0);
}

}  // TEST_SUITE
