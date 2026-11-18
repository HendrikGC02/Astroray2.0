// SPDX-License-Identifier: Apache-2.0
// queue_build on the GPU: the three kernels, loaded from the cubins embedded in this
// executable (RCDATA, as the DLL carries them) and launched through CudaDevice, produce
// the same segments and step table as the CPU device for the same input. The partition is
// integer code, so agreement is bit-exact (0009 d17(i)).
// Ref: docs/decisions/0002-kernel-architecture.md D4, D6, D14; 0009 d3, d17(i).
#include <doctest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "device/cpu/cpu_device.h"
#include "host/data_resource.h"
#include "host/device.h"
#include "host/thread_pool.h"
#include "stages/queue_build.h"

using namespace astroray;
using namespace astroray::host;
using ar::Pool;
using ar::Stage;

namespace {

struct Result {
  std::vector<uint32_t> queue[2];
  ar::StepTable table{};
};

// Runs queue_build on `dev` over the two next_stage arrays and reads everything back. The
// device must have the queue_build kernels (CPU: registered by the caller).
Result run_queue_build(Device& dev, const std::vector<uint8_t>& main_next, const std::vector<uint8_t>& shadow_next) {
  const uint32_t n[2] = {uint32_t(main_next.size()), uint32_t(shadow_next.size())};
  const uint64_t tiles = uint64_t(ar::queue_tiles(n[0])) + ar::queue_tiles(n[1]);
  DeviceBuffer next_buf[2], queue_buf[2], soa_buf[2];
  const std::vector<uint8_t>* next[2] = {&main_next, &shadow_next};
  for (int p = 0; p < 2; ++p) {
    // An empty pool still gets one slot: the device refuses zero-byte allocations.
    const uint32_t slots = n[p] ? n[p] : 1u;
    next_buf[p] = dev.allocate(p == 0 ? LedgerCategory::MAIN_PATH_POOL : LedgerCategory::SHADOW_POOL, slots, Owner{1, 0});
    if (n[p]) dev.upload(next_buf[p], 0, next[p]->data(), n[p]);
    queue_buf[p] = dev.allocate(LedgerCategory::QUEUE_SORT_INDICES, sizeof(uint32_t) * slots, Owner{1, 0});
    dev.memset(queue_buf[p], 0, 0xFF, queue_buf[p].bytes);
  }
  ar::PathStateSoA path{};
  ar::ShadowItemSoA shadow{};
  path.next_stage = static_cast<uint8_t*>(next_buf[0].ptr);
  shadow.next_stage = static_cast<uint8_t*>(next_buf[1].ptr);
  soa_buf[0] = dev.allocate(LedgerCategory::TABLES, sizeof path, Owner{1, 0});
  soa_buf[1] = dev.allocate(LedgerCategory::TABLES, sizeof shadow, Owner{1, 0});
  dev.upload(soa_buf[0], 0, &path, sizeof path);
  dev.upload(soa_buf[1], 0, &shadow, sizeof shadow);
  DeviceBuffer counts_buf = dev.allocate(LedgerCategory::QUEUE_SORT_INDICES, sizeof(uint32_t) * ar::kQueueBins * (tiles ? tiles : 1u), Owner{1, 0});
  DeviceBuffer table_buf = dev.allocate(LedgerCategory::TABLES, sizeof(ar::StepTable), Owner{1, 0});
  ar::StepTable table_in{};
  table_in.high_water[0] = n[0];
  table_in.high_water[1] = n[1];
  dev.upload(table_buf, 0, &table_in, sizeof table_in);

  ar::KernelGlobals kg{};
  kg.path = static_cast<const ar::PathStateSoA*>(soa_buf[0].ptr);
  kg.shadow = static_cast<const ar::ShadowItemSoA*>(soa_buf[1].ptr);
  kg.step_table = static_cast<const ar::StepTable*>(table_buf.ptr);
  kg.main_queue = static_cast<const uint32_t*>(queue_buf[0].ptr);
  kg.shadow_queue = static_cast<const uint32_t*>(queue_buf[1].ptr);
  kg.queue_counts = static_cast<uint32_t*>(counts_buf.ptr);
  DeviceBuffer kg_buf = dev.allocate(LedgerCategory::TABLES, sizeof kg, Owner{1, 0});
  dev.upload(kg_buf, 0, &kg, sizeof kg);
  dev.set_kernel_globals(kg_buf);

  const uint32_t grid = ar::queue_build_grid(n[0], n[1]);
  dev.launch(KernelId::QUEUE_BUILD_COUNT, grid);
  dev.launch(KernelId::QUEUE_BUILD_SCAN, ar::queue_scan_grid(n[0], n[1]));
  dev.launch(KernelId::QUEUE_BUILD_SCATTER, grid);
  dev.synchronize();

  Result r;
  dev.read_step_table(r.table);
  for (int p = 0; p < 2; ++p) {
    r.queue[p].resize(n[p]);
    if (n[p]) dev.download(r.queue[p].data(), queue_buf[p], 0, queue_buf[p].bytes);
  }
  for (int p = 0; p < 2; ++p) {
    dev.free(next_buf[p]);
    dev.free(queue_buf[p]);
    dev.free(soa_buf[p]);
  }
  dev.free(counts_buf);
  dev.free(table_buf);
  dev.free(kg_buf);
  return r;
}

std::vector<uint8_t> random_values(uint32_t n, uint64_t seed, bool shadow_pool, uint32_t invalid_every) {
  std::vector<uint8_t> stages;
  for (uint32_t s = 0; s < ar::kStageCount; ++s)
    if ((ar::stage_pool(static_cast<Stage>(s)) == Pool::SHADOW) == shadow_pool) stages.push_back(uint8_t(s));
  uint64_t x = seed;
  std::vector<uint8_t> v(n);
  for (uint32_t i = 0; i < n; ++i) {
    uint64_t z = (x += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    z ^= z >> 31;
    v[i] = stages[(z >> 16) % stages.size()];
    if (invalid_every != 0 && i % invalid_every == invalid_every - 1) v[i] = uint8_t(ar::kStageCount + (z & 0x3f));
  }
  return v;
}

// Compares the GPU result with the CPU device's for one input; returns the number of
// differing queue entries plus differing table bytes.
uint64_t differences(const std::vector<uint8_t>& main_next, const std::vector<uint8_t>& shadow_next) {
  Ledger gpu_ledger, cpu_ledger;
  std::unique_ptr<ModuleDevice> gpu = make_cuda_device(gpu_ledger, 0);
  ContextState ctx;
  ThreadPool pool(ctx, 8);
  CpuDevice cpu(cpu_ledger, pool);
  cpu.register_kernel(KernelId::QUEUE_BUILD_COUNT, &cpu_queue_build_count);
  cpu.register_kernel(KernelId::QUEUE_BUILD_SCAN, &cpu_queue_build_scan);
  cpu.register_kernel(KernelId::QUEUE_BUILD_SCATTER, &cpu_queue_build_scatter);
  const Result g = run_queue_build(*gpu, main_next, shadow_next);
  const Result c = run_queue_build(cpu, main_next, shadow_next);
  uint64_t diff = 0;
  for (int p = 0; p < 2; ++p) {
    REQUIRE(g.queue[p].size() == c.queue[p].size());
    for (size_t i = 0; i < g.queue[p].size(); ++i) diff += g.queue[p][i] != c.queue[p][i];
  }
  const auto* a = reinterpret_cast<const unsigned char*>(&g.table);
  const auto* b = reinterpret_cast<const unsigned char*>(&c.table);
  for (size_t i = 0; i < sizeof g.table; ++i) diff += a[i] != b[i];
  CHECK(gpu_ledger.total() == 0);
  CHECK(cpu_ledger.total() == 0);
  return diff;
}

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("queue_build/embedded kernels load") {
  // The release DLL's way to a kernel: RCDATA K_<NAME> of the stage TU, then k_<name>.
  for (const char* name : {"K_QUEUE_BUILD_COUNT", "K_QUEUE_BUILD_SCAN", "K_QUEUE_BUILD_SCATTER"}) {
    INFO(name);
    DataBlob blob;
    REQUIRE(find_data(name, &blob));
    CHECK(blob.size > 1000);
  }
  // k_queue_build_count is found and launched through the device (no items: tile count 0).
  Ledger ledger;
  auto dev = make_cuda_device(ledger, 0);
  const Result r = run_queue_build(*dev, {}, {});
  CHECK(r.table.refill_count == 0);
  CHECK(r.table.invalid_items == 0);
}

TEST_CASE("queue_build/cpu gpu agree") {
  // 10^6 random next_stage values per pool, with and without invalid values mixed in, and a
  // pool pair whose shadow pool is far below demand (shadow producers starved).
  constexpr uint32_t kN = 1000000;
  const uint64_t diff_random = differences(random_values(kN, 11, false, 0), random_values(kN, 12, true, 0));
  const uint64_t diff_invalid = differences(random_values(kN, 13, false, 997), random_values(kN, 14, true, 1013));
  // Starved: every main item is a shadow producer, the shadow pool has 1000 free slots among
  // 100 live items.
  std::vector<uint8_t> starved_main(kN, uint8_t(Stage::SHADE_SURFACE));
  for (uint32_t i = 0; i < kN; i += 3) starved_main[i] = uint8_t(Stage::SHADE_VOLUME);
  std::vector<uint8_t> starved_shadow(1100, uint8_t(Stage::SHADOW_TERMINATED));
  for (uint32_t i = 0; i < 100; ++i) starved_shadow[i * 11] = uint8_t(Stage::INTERSECT_SHADOW);
  const uint64_t diff_starved = differences(starved_main, starved_shadow);
  std::printf("queue_build: random differences=%llu invalid differences=%llu starved differences=%llu\n",
              static_cast<unsigned long long>(diff_random), static_cast<unsigned long long>(diff_invalid),
              static_cast<unsigned long long>(diff_starved));
  CHECK(diff_random == 0);
  CHECK(diff_invalid == 0);
  CHECK(diff_starved == 0);
  if (diff_random + diff_invalid + diff_starved == 0) std::printf("segments identical\n");
}

TEST_CASE("queue_build/cpu gpu agree at full pool size") {
  // The scheduler's largest pools, 2^21 main and 2^22 shadow items: 6,144 histogram rows, 192
  // scan tiles whose prefixes chain through the GPU look-back while their predecessors run.
  // Main items in runs of 100,000 of INTERSECT_CLOSEST, SHADE_SURFACE, TERMINATED with random
  // values between runs; shadow items in runs of INTERSECT_SHADOW and SHADOW_TERMINATED. Other
  // segments are empty. Both devices must give the reference stable partition, not only agree.
  constexpr uint32_t kMain = 1u << 21, kShadow = 1u << 22;
  const Stage main_runs[] = {Stage::INTERSECT_CLOSEST, Stage::SHADE_SURFACE, Stage::TERMINATED};
  std::vector<uint8_t> m(kMain), s(kShadow);
  for (uint32_t i = 0; i < kMain; ++i) m[i] = uint8_t(main_runs[(i / 100000) % 3]);
  for (uint32_t i = 0; i < kShadow; ++i)
    s[i] = uint8_t((i / 150000) % 2 == 0 ? Stage::INTERSECT_SHADOW : Stage::SHADOW_TERMINATED);
  const std::vector<uint8_t> noise = random_values(kMain, 21, false, 0);
  for (uint32_t i = 0; i < kMain; i += 7) m[i] = noise[i];

  Ledger ledger;
  std::unique_ptr<ModuleDevice> gpu = make_cuda_device(ledger, 0);
  const Result g = run_queue_build(*gpu, m, s);
  uint64_t wrong = 0;
  const std::vector<uint8_t>* next[2] = {&m, &s};
  for (int p = 0; p < 2; ++p) {
    // The reference: every stage of the pool in Stage order, its items in index order.
    std::vector<uint32_t> want;
    for (uint32_t st = 0; st < ar::kStageCount; ++st) {
      if ((ar::stage_pool(static_cast<Stage>(st)) == Pool::SHADOW) != (p == 1)) continue;
      CHECK(g.table.stage[st].start == want.size());
      for (uint32_t i = 0; i < next[p]->size(); ++i)
        if ((*next[p])[i] == st) want.push_back(i);
    }
    REQUIRE(want.size() == g.queue[p].size());
    for (size_t i = 0; i < want.size(); ++i) wrong += want[i] != g.queue[p][i];
  }
  CHECK(g.table.invalid_items == 0);
  CHECK(ledger.total() == 0);
  const uint64_t diff = differences(m, s);
  std::printf("queue_build full pools: reference differences=%llu cpu differences=%llu\n",
              static_cast<unsigned long long>(wrong), static_cast<unsigned long long>(diff));
  CHECK(wrong == 0);
  CHECK(diff == 0);
}

}  // TEST_SUITE
