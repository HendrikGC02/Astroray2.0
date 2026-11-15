// SPDX-License-Identifier: Apache-2.0
// The thread pool and CpuDevice: parallel_for coverage, a registered test-only stage
// that receives its step-table segment, every refusal path with its exact message,
// and allocation through the ledger. Expected values are typed from the setups below.
// Ref: docs/decisions/0002-kernel-architecture.md D4 (chunks of 1,024), D7 (two-method
// device interface); 0007 d14 (ledger, refusal text); pbrt-v4 ParallelFor pattern.
#include <doctest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "device/cpu/cpu_device.h"
#include "host/thread_pool.h"

using namespace astroray;
using namespace astroray::host;

namespace {

constexpr uint64_t kGiB = 1ull << 30;

// What the test stages write into, reached through KernelGlobals::film (an opaque
// pointer to the kernel, a Sink here).
struct Sink {
  std::vector<std::atomic<uint32_t>> visits;
  std::atomic<uint64_t> queue_sum{0};
  std::atomic<uint32_t> wrong_step{0};
  uint32_t want_start = 0, want_count = 0, want_shadow_base = 0;
  explicit Sink(size_t n) : visits(n) { reset(); }
  void reset() {
    for (auto& v : visits) v.store(0);
    queue_sum.store(0);
    wrong_step.store(0);
  }
};

Sink* sink_of(const ar::KernelGlobals& kg) { return reinterpret_cast<Sink*>(kg.film); }

void film_resolve_test(const ar::KernelGlobals& kg, uint32_t first, uint32_t last) {
  for (uint32_t i = first; i < last; ++i) sink_of(kg)->visits[i].fetch_add(1);
}

}  // namespace

namespace ar {
// Test-only stage: records the item, the queue entry it reads and the step entry it got.
AR_FN void stage_test_dummy(const KernelGlobals& kg, uint32_t item, const StepEntry& step) {
  Sink* s = sink_of(kg);
  s->visits[item].fetch_add(1);
  s->queue_sum.fetch_add(kg.main_queue[step.start + item]);
  if (step.start != s->want_start || step.count != s->want_count || step.shadow_base != s->want_shadow_base)
    s->wrong_step.fetch_add(1);
}
}  // namespace ar

AR_CPU_STAGE(test_dummy)

namespace {

// Everything a launch test needs: pool, device, a KernelGlobals uploaded through the device.
struct Rig {
  ContextState ctx;
  Ledger ledger;
  ThreadPool pool;
  CpuDevice dev;
  std::vector<uint32_t> queue;
  ar::StepTable table{};
  Sink sink;
  ar::KernelGlobals kg{};
  DeviceBuffer kg_buf;

  explicit Rig(unsigned workers = 8) : pool(ctx, workers), dev(ledger, pool), queue(4096), sink(4096) {
    for (uint32_t i = 0; i < queue.size(); ++i) queue[i] = 2 * i + 1;
    // The dummy stage owns SHADE_VOLUME's segment: 3000 items starting at queue entry 100.
    table.stage[static_cast<uint32_t>(ar::Stage::SHADE_VOLUME)] = {100, 3000, 7};
    sink.want_start = 100;
    sink.want_count = 3000;
    sink.want_shadow_base = 7;
    kg.step_table = &table;
    kg.main_queue = queue.data();
    kg.film = reinterpret_cast<ar::FilmData*>(&sink);
    kg_buf = dev.allocate(LedgerCategory::TABLES, sizeof kg, {1, 1});
    dev.upload(kg_buf, 0, &kg, sizeof kg);
  }
  ~Rig() { dev.free(kg_buf); }
};

// A device whose reserve rule refuses anything above 1 GiB, reporting 1.5 GiB available.
class RefusingDevice : public CpuDevice {
 public:
  using CpuDevice::CpuDevice;

 protected:
  bool may_allocate(uint64_t need, uint64_t* available_bytes) override {
    *available_bytes = kGiB + kGiB / 2;
    return need <= kGiB;
  }
};

std::string num(uint64_t v) { return std::to_string(v); }

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("cpu_device/parallel_for covers every item") {
  for (unsigned workers : {1u, 8u}) {
    INFO("workers " << workers);
    ContextState ctx;
    ThreadPool pool(ctx, workers);
    CHECK(pool.workers() == workers);
    // Counts that are not a multiple of the chunk, for the default chunk and a small one.
    for (uint32_t chunk : {0u, 64u}) {
      constexpr uint32_t kCount = 10000 + 17;
      std::vector<std::atomic<uint32_t>> hits(kCount);
      for (auto& h : hits) h.store(0);
      std::atomic<uint32_t> bad_range{0};
      const uint32_t chunk_size = chunk ? chunk : kDefaultChunk;
      const ThreadPool::Body body = [&](uint32_t first, uint32_t last) {
        if (!(first < last) || last - first > chunk_size || last > kCount) bad_range.fetch_add(1);
        for (uint32_t i = first; i < last && i < kCount; ++i) hits[i].fetch_add(1);
      };
      if (chunk) pool.parallel_for(kCount, chunk, body);
      else pool.parallel_for(kCount, body);
      uint32_t wrong = 0;
      for (auto& h : hits) wrong += h.load() != 1;
      CHECK(wrong == 0);
      CHECK(bad_range.load() == 0);
    }
  }
}

TEST_CASE("cpu_device/parallel_for edge cases") {
  ContextState ctx;
  ThreadPool pool(ctx, 4);
  std::atomic<uint32_t> calls{0};
  pool.parallel_for(0, [&](uint32_t, uint32_t) { calls.fetch_add(1); });
  CHECK(calls.load() == 0);
  // Fewer items than a chunk: one call with the whole range.
  std::atomic<uint32_t> first_seen{99}, last_seen{99};
  pool.parallel_for(5, [&](uint32_t f, uint32_t l) {
    calls.fetch_add(1);
    first_seen.store(f);
    last_seen.store(l);
  });
  CHECK(calls.load() == 1);
  CHECK(first_seen.load() == 0);
  CHECK(last_seen.load() == 5);
  CHECK_THROWS_WITH_AS(pool.parallel_for(10, 0, [](uint32_t, uint32_t) {}), "parallel_for: chunk size zero",
                       std::invalid_argument);
  CHECK_THROWS_WITH_AS(ThreadPool(ctx, 0), "thread pool: zero workers", std::invalid_argument);
}

TEST_CASE("cpu_device/parallel_for rethrows the first body exception and stays usable") {
  ContextState ctx;
  ThreadPool pool(ctx, 8);
  CHECK_THROWS_WITH_AS(pool.parallel_for(10000, 64,
                                         [](uint32_t first, uint32_t last) {
                                           if (first <= 5000 && 5000 < last) throw std::runtime_error("boom");
                                         }),
                       "boom", std::runtime_error);
  CHECK_FALSE(ctx.poisoned.load());
  std::atomic<uint32_t> sum{0};
  pool.parallel_for(3000, [&](uint32_t first, uint32_t last) { sum.fetch_add(last - first); });
  CHECK(sum.load() == 3000);
}

TEST_CASE("cpu_device/parallel_for calls from two threads take turns") {
  ContextState ctx;
  ThreadPool pool(ctx, 4);
  std::atomic<uint32_t> a{0}, b{0};
  std::thread other([&] { pool.parallel_for(20000, 100, [&](uint32_t f, uint32_t l) { b.fetch_add(l - f); }); });
  pool.parallel_for(30000, 100, [&](uint32_t f, uint32_t l) { a.fetch_add(l - f); });
  other.join();
  CHECK(a.load() == 30000);
  CHECK(b.load() == 20000);
}

TEST_CASE("cpu_device/parallel_for does not hang on a poisoned context") {
  ContextState ctx;
  std::promise<void> release;
  std::shared_future<void> released = release.get_future().share();
  ThreadPool pool(ctx, 1);  // declared last: joins before `released` and `ctx` die
  // The worker's body poisons the context (as a faulting engine thread would) and then
  // stays inside its chunk, so only the poisoned check can end the wait.
  CHECK_THROWS_WITH_AS(pool.parallel_for(10,
                                         [&](uint32_t, uint32_t) {
                                           ctx.poison_message = "Engine fault, test";
                                           ctx.poisoned.store(true, std::memory_order_release);
                                           released.wait();
                                         }),
                       "Engine fault, test", std::runtime_error);
  // A poisoned context refuses further loops at once.
  CHECK_THROWS_WITH_AS(pool.parallel_for(10, [](uint32_t, uint32_t) {}), "Engine fault, test", std::runtime_error);
  release.set_value();
}

TEST_CASE("cpu_device/stage launch receives its segment") {
  Rig r;
  r.dev.register_stage(ar::Stage::SHADE_VOLUME, &cpu_stage_test_dummy);
  r.dev.set_kernel_globals(r.kg_buf);

  r.dev.launch(ar::Stage::SHADE_VOLUME, 3000);
  uint32_t wrong = 0;
  for (uint32_t i = 0; i < 3000; ++i) wrong += r.sink.visits[i].load() != 1;
  CHECK(wrong == 0);
  CHECK(r.sink.visits[3000].load() == 0);
  // queue entries 100..3099 hold 2i+1: sum = 9,600,000.
  CHECK(r.sink.queue_sum.load() == 9600000);
  CHECK(r.sink.wrong_step.load() == 0);

  // (a) The host grid only sizes a GPU grid: a grid below the segment still covers all of it.
  r.sink.reset();
  r.dev.launch(ar::Stage::SHADE_VOLUME, 1500);
  wrong = 0;
  for (uint32_t i = 0; i < 3000; ++i) wrong += r.sink.visits[i].load() != 1;
  CHECK(wrong == 0);
  CHECK(r.sink.queue_sum.load() == 9600000);
  CHECK(r.sink.wrong_step.load() == 0);

  // (b) A grid above the segment runs exactly the segment (no item past it, no queue read past it).
  r.sink.reset();
  r.dev.launch(ar::Stage::SHADE_VOLUME, 4000);
  wrong = 0;
  for (uint32_t i = 0; i < 4096; ++i) wrong += r.sink.visits[i].load() != (i < 3000 ? 1u : 0u);
  CHECK(wrong == 0);
  CHECK(r.sink.queue_sum.load() == 9600000);
  CHECK(r.sink.wrong_step.load() == 0);

  // A partial shadow-producer launch (0002 D5) is a smaller step count written by the scan:
  // items 0..1499, whatever the grid.
  r.table.stage[static_cast<uint32_t>(ar::Stage::SHADE_VOLUME)].count = 1500;
  r.sink.want_count = 1500;
  r.sink.reset();
  r.dev.launch(ar::Stage::SHADE_VOLUME, 3000);
  wrong = 0;
  for (uint32_t i = 0; i < 3000; ++i) wrong += r.sink.visits[i].load() != (i < 1500 ? 1u : 0u);
  CHECK(wrong == 0);
  CHECK(r.sink.queue_sum.load() == 2550000);  // entries 100..1599
  CHECK(r.sink.wrong_step.load() == 0);

  // (c) An empty segment launches nothing, even with a large grid.
  r.table.stage[static_cast<uint32_t>(ar::Stage::SHADE_VOLUME)].count = 0;
  r.sink.want_count = 0;
  r.sink.reset();
  r.dev.launch(ar::Stage::SHADE_VOLUME, 3000);
  wrong = 0;
  for (uint32_t i = 0; i < 4096; ++i) wrong += r.sink.visits[i].load() != 0;
  CHECK(wrong == 0);
  CHECK(r.sink.queue_sum.load() == 0);
  r.dev.synchronize();
}

TEST_CASE("cpu_device/kernel launch runs every item once") {
  Rig r;
  r.dev.register_kernel(KernelId::FILM_RESOLVE, &film_resolve_test);
  r.dev.set_kernel_globals(r.kg_buf);
  r.dev.launch(KernelId::FILM_RESOLVE, 2500);
  uint32_t wrong = 0;
  for (uint32_t i = 0; i < 4096; ++i) wrong += r.sink.visits[i].load() != (i < 2500 ? 1u : 0u);
  CHECK(wrong == 0);
}

TEST_CASE("cpu_device/launch and registration refusals") {
  Rig r(2);
  const std::string unbound = "CpuDevice: no KernelGlobals bound (call set_kernel_globals first)";
  ar::StepTable out;
  CHECK_THROWS_WITH_AS(r.dev.launch(ar::Stage::SHADE_VOLUME, 1), unbound.c_str(), std::logic_error);
  CHECK_THROWS_WITH_AS(r.dev.launch(KernelId::FILM_RESOLVE, 1), unbound.c_str(), std::logic_error);
  CHECK_THROWS_WITH_AS(r.dev.read_step_table(out), unbound.c_str(), std::logic_error);

  const std::string need = ", need " + num(sizeof(ar::KernelGlobals));
  DeviceBuffer small = r.dev.allocate(LedgerCategory::TABLES, 16, {1, 1});
  CHECK_THROWS_WITH_AS(r.dev.set_kernel_globals(small), ("CpuDevice: the KernelGlobals buffer holds 16 bytes" + need).c_str(),
                       std::invalid_argument);
  CHECK_THROWS_WITH_AS(r.dev.set_kernel_globals(DeviceBuffer()),
                       ("CpuDevice: the KernelGlobals buffer holds 0 bytes" + need).c_str(), std::invalid_argument);
  r.dev.free(small);

  r.dev.register_stage(ar::Stage::SHADE_VOLUME, &cpu_stage_test_dummy);
  r.dev.set_kernel_globals(r.kg_buf);
  CHECK_THROWS_WITH_AS(r.dev.launch(ar::Stage::SHADE_SURFACE, 1), "CpuDevice: no entry point registered for stage 5",
                       std::runtime_error);
  CHECK_THROWS_WITH_AS(r.dev.launch(KernelId::FILM_RESOLVE, 1), "CpuDevice: no entry point registered for kernel 7",
                       std::runtime_error);
  CHECK_THROWS_WITH_AS(r.dev.launch(static_cast<ar::Stage>(200), 1), "CpuDevice: stage 200 does not exist",
                       std::invalid_argument);
  CHECK_THROWS_WITH_AS(r.dev.launch(static_cast<KernelId>(99), 1), "CpuDevice: kernel 99 does not exist",
                       std::invalid_argument);
  CHECK(r.sink.queue_sum.load() == 0);  // no refused launch ran an item

  CHECK_THROWS_WITH_AS(r.dev.register_stage(ar::Stage::SHADE_VOLUME, &cpu_stage_test_dummy),
                       "CpuDevice: stage 10 is already registered", std::logic_error);
  CHECK_THROWS_WITH_AS(r.dev.register_stage(ar::Stage::SHADE_SURFACE, nullptr),
                       "CpuDevice: null entry point for stage 5", std::invalid_argument);
  CHECK_THROWS_WITH_AS(r.dev.register_stage(ar::Stage::TERMINATED, &cpu_stage_test_dummy),
                       "CpuDevice: stage 13 is not launchable", std::invalid_argument);
  CHECK_THROWS_WITH_AS(r.dev.register_stage(ar::Stage::SHADOW_TERMINATED, &cpu_stage_test_dummy),
                       "CpuDevice: stage 14 is not launchable", std::invalid_argument);
  CHECK_THROWS_WITH_AS(r.dev.register_stage(static_cast<ar::Stage>(200), &cpu_stage_test_dummy),
                       "CpuDevice: stage 200 is not launchable", std::invalid_argument);
  r.dev.register_kernel(KernelId::FILM_RESOLVE, &film_resolve_test);
  CHECK_THROWS_WITH_AS(r.dev.register_kernel(KernelId::FILM_RESOLVE, &film_resolve_test),
                       "CpuDevice: kernel 7 is already registered", std::logic_error);
  CHECK_THROWS_WITH_AS(r.dev.register_kernel(KernelId::FILM_PRESENT, nullptr), "CpuDevice: null entry point for kernel 9",
                       std::invalid_argument);
  CHECK_THROWS_WITH_AS(r.dev.register_kernel(static_cast<KernelId>(99), &film_resolve_test),
                       "CpuDevice: kernel 99 does not exist", std::invalid_argument);

  // A bound KernelGlobals without a step table: stages and the step-table read refuse.
  ar::KernelGlobals bare{};
  DeviceBuffer bare_buf = r.dev.allocate(LedgerCategory::TABLES, sizeof bare, {1, 1});
  r.dev.upload(bare_buf, 0, &bare, sizeof bare);
  r.dev.set_kernel_globals(bare_buf);
  CHECK_THROWS_WITH_AS(r.dev.launch(ar::Stage::SHADE_VOLUME, 1), "CpuDevice: the bound KernelGlobals has no step table",
                       std::logic_error);
  CHECK_THROWS_WITH_AS(r.dev.read_step_table(out), "CpuDevice: the bound KernelGlobals has no step table",
                       std::logic_error);
  r.dev.free(bare_buf);
}

TEST_CASE("cpu_device/read_step_table returns the device's table") {
  Rig r;
  for (uint32_t i = 0; i < ar::kStageCount; ++i) r.table.stage[i] = {i * 10, i * 10 + 1, i * 10 + 2};
  r.table.refill_count = 77;
  r.dev.set_kernel_globals(r.kg_buf);
  ar::StepTable out{};
  r.dev.read_step_table(out);
  for (uint32_t i = 0; i < ar::kStageCount; ++i) {
    INFO("stage " << i);
    CHECK(out.stage[i].start == i * 10);
    CHECK(out.stage[i].count == i * 10 + 1);
    CHECK(out.stage[i].shadow_base == i * 10 + 2);
  }
  CHECK(out.refill_count == 77);
  CHECK(r.dev.kind() == AR_DEVICE_CPU);
}

TEST_CASE("cpu_device/allocation goes through the ledger") {
  ContextState ctx;
  Ledger ledger;
  ThreadPool pool(ctx, 1);
  CpuDevice dev(ledger, pool);

  DeviceBuffer a = dev.allocate(LedgerCategory::GEOMETRY, 1024, {1, 0});
  DeviceBuffer b = dev.allocate(LedgerCategory::FILM, 4096, {1, 2});
  CHECK(a.bytes == 1024);
  CHECK(a.category == LedgerCategory::GEOMETRY);
  CHECK(a.ledger_id != 0);
  CHECK(reinterpret_cast<uintptr_t>(a.ptr) % 256 == 0);
  CHECK(reinterpret_cast<uintptr_t>(b.ptr) % 256 == 0);
  CHECK(ledger.total() == 5120);
  CHECK(ledger.category_total(LedgerCategory::GEOMETRY) == 1024);
  CHECK(ledger.category_total(LedgerCategory::FILM) == 4096);
  CHECK(ledger.scene_total(1) == 5120);
  CHECK(ledger.session_total(2) == 4096);
  CHECK(ledger.session_total(0) == 1024);

  // upload / memset / download inside the buffer.
  dev.memset(a, 0, 0xAB, 1024);
  uint8_t pattern[256];
  for (int i = 0; i < 256; ++i) pattern[i] = static_cast<uint8_t>(i);
  dev.upload(a, 100, pattern, 256);
  std::vector<uint8_t> back(1024);
  dev.download(back.data(), a, 0, 1024);
  uint32_t wrong = 0;
  for (uint32_t i = 0; i < 1024; ++i) {
    const uint8_t want = (i >= 100 && i < 356) ? static_cast<uint8_t>(i - 100) : 0xAB;
    wrong += back[i] != want;
  }
  CHECK(wrong == 0);

  const char* range_msg = "device buffer range [1000, +25) is outside the 1024-byte buffer";
  CHECK_THROWS_WITH_AS(dev.upload(a, 1000, pattern, 25), range_msg, std::out_of_range);
  CHECK_THROWS_WITH_AS(dev.download(back.data(), a, 1000, 25), range_msg, std::out_of_range);
  CHECK_THROWS_WITH_AS(dev.memset(a, 1000, 0, 25), range_msg, std::out_of_range);
  CHECK_THROWS_WITH_AS(dev.memset(a, 2000, 0, 0), "device buffer range [2000, +0) is outside the 1024-byte buffer",
                       std::out_of_range);
  CHECK_THROWS_AS(dev.memset(a, 8, 0, UINT64_MAX), std::out_of_range);

  DeviceBuffer stale = a;
  dev.free(a);
  CHECK(a.ptr == nullptr);
  CHECK(a.bytes == 0);
  CHECK(ledger.total() == 4096);
  CHECK(ledger.category_total(LedgerCategory::GEOMETRY) == 0);
  const std::string dbl = "ledger: free of allocation " + num(stale.ledger_id) + " that is not live";
  CHECK_THROWS_WITH_AS(dev.free(stale), dbl.c_str(), std::invalid_argument);
  dev.free(a);  // an empty buffer is ignored
  CHECK(ledger.total() == 4096);
  dev.free(b);
  CHECK(ledger.total() == 0);
  CHECK(ledger.records().empty());
}

TEST_CASE("cpu_device/allocation failures leave no ledger record") {
  ContextState ctx;
  Ledger ledger;
  ThreadPool pool(ctx, 1);
  CpuDevice dev(ledger, pool);
  CHECK_THROWS_WITH_AS(dev.allocate(LedgerCategory::FILM, 0, {}), "device allocation of zero bytes", std::invalid_argument);
  CHECK_THROWS_AS(dev.allocate(LedgerCategory::FILM, 1ull << 62, {}), std::bad_alloc);
  CHECK_THROWS_AS(dev.allocate(static_cast<LedgerCategory>(16), 64, {}), std::invalid_argument);
  CHECK(ledger.total() == 0);
  CHECK(ledger.records().empty());

  // A device that refuses: the typed error carries the message the report entry shows.
  RefusingDevice small(ledger, pool);
  DeviceBuffer ok = small.allocate(LedgerCategory::ACCELERATION, 1ull << 20, {1, 1});
  CHECK(ledger.total() == (1ull << 20));
  try {
    small.allocate(LedgerCategory::ACCELERATION, 2 * kGiB, {1, 1});
    FAIL("the allocation should have been refused");
  } catch (const AllocationRefused& e) {
    CHECK(std::string(e.what()) == "needs 2.00 GiB for acceleration, 1.50 GiB available");
    CHECK(e.status() == AR_ERR_OUT_OF_MEMORY);
    CHECK(e.category() == LedgerCategory::ACCELERATION);
  }
  CHECK(ledger.total() == (1ull << 20));
  small.free(ok);
  CHECK(ledger.total() == 0);
}

}  // TEST_SUITE
