// SPDX-License-Identifier: Apache-2.0
// The host scheduler on the CPU device with stub stages (stub_stages.h): every path terminates
// at its first hit, so the expected outcome of every camera sample and light rank is a table
// typed here from the stub routing rule, never read from the scheduler. Covers light-path
// routing (0002 D8), a pool far below the work, AR_DEBUG_PERMUTE invariance (D6-D7), the sort
// host contract, cancel, the invalid-items failure and the short-launch check of CpuDevice.
// Ref: docs/decisions/0002-kernel-architecture.md D5-D8; stub_stages.h.
#include <doctest.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "device/cpu/cpu_device.h"
#include "host/scheduler.h"
#include "host/thread_pool.h"
#include "kernel/scene/scene_view.h"
#include "kernel/stages/queue_build.h"
#include "kernel/stages/route_hit.h"
#include "kernel/stages/sort.h"
#include "stub_stages.h"

using namespace astroray;
using namespace astroray::host;
using ar::Stage;

namespace {

struct Rig {
  ContextState ctx;
  Ledger ledger;
  ThreadPool pool;
  CpuDevice dev;
  stub::Results res;
  ar::SceneData scene{};
  std::unique_ptr<Scheduler> sched;

  // programs: the scene's distinct surface programs (the sort threshold); by default one per material.
  Rig(uint32_t cameras, uint32_t lights, uint64_t capacity, bool permute = false, uint32_t materials = 0,
      CpuStageFn surface = &stub::shade_surface, int64_t programs = -1)
      : pool(ctx, 8), dev(ledger, pool), res(cameras, lights) {
    dev.register_scheduler_kernels();
    dev.register_kernel(KernelId::GENERATE_CAMERA, &stub::generate_camera);
    dev.register_kernel(KernelId::GENERATE_LIGHT, &stub::generate_light);
    dev.register_stage(Stage::INTERSECT_CLOSEST, &stub::intersect_closest);
    dev.register_stage(Stage::INTERSECT_SHADOW, &stub::intersect_shadow);
    dev.register_stage(Stage::SHADE_BACKGROUND, &stub::shade_background);
    dev.register_stage(Stage::SHADE_LIGHT, &stub::shade_light);
    dev.register_stage(Stage::SHADE_SURFACE, surface);
    dev.register_stage(Stage::SHADE_LIGHT_PATH, &stub::shade_light_path);
    dev.register_stage(Stage::SHADE_SHADOW, &stub::shade_shadow);
    scene.material_header_count = materials;
    SchedulerConfig cfg;
    cfg.globals.film = reinterpret_cast<ar::FilmData*>(&res);
    cfg.globals.scene = &scene;
    cfg.material_header_count = materials;
    cfg.material_program_count = programs < 0 ? materials : uint32_t(programs);
    cfg.capacity_override = capacity;
    cfg.permute = permute;
    sched = std::make_unique<Scheduler>(dev, cfg);
  }
  PassStatus run() {
    return sched->run_pass(PassWork{res.camera_count, uint64_t(res.outcome.size()) - res.camera_count});
  }
};

// The stub routing rule, typed independently of stub_stages.h: indices are camera samples then
// light ranks; the rank used for the rule is the index within its own kind.
uint32_t want_outcome(uint32_t cameras, uint32_t index) {
  const bool light = index >= cameras;
  const uint32_t r = (light ? index - cameras : index) % 5;
  if (r == 0) return light ? 5 : 1;  // light miss terminates; camera miss is the background
  if (r == 1) return 2;              // a lamp, either kind
  return light ? 4 : 3;              // light-path hit or surface hit
}
bool want_shadow(uint32_t cameras, uint32_t index) {
  const uint32_t o = want_outcome(cameras, index);
  return (o == 3 || o == 4) && index % 3 != 0;
}

void check_all(const Rig& r) {
  const uint32_t n = uint32_t(r.res.outcome.size());
  uint32_t bad_outcome = 0, bad_shadow = 0;
  for (uint32_t i = 0; i < n; ++i) {
    bad_outcome += r.res.outcome[i] != want_outcome(r.res.camera_count, i);
    bad_shadow += r.res.shadow_delivered[i] != (want_shadow(r.res.camera_count, i) ? i + 1 : 0u);
  }
  CHECK(bad_outcome == 0);
  CHECK(bad_shadow == 0);
}

void print(const char* what, const Rig& r) {
  std::printf("[scheduler] %s: capacity %llu\n%s", what, (unsigned long long)r.sched->capacity(),
              format_counters(r.sched->counters()).c_str());
}

void stage_invalid(const ar::KernelGlobals& kg, const ar::StepEntry& step, uint32_t first, uint32_t last) {
  for (uint32_t i = first; i < last; ++i) ar::ps_next_stage(kg, kg.main_queue[step.start + i]) = 99;
}

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("scheduler/stub light paths terminate") {
  // 0002 D8: a stub generate_light whose paths end at their first hit; every light path ends,
  // takes its routed stage, and the pass ends (run_pass returns, the livelock guard is silent).
  Rig r(3000, 2000, 0);
  REQUIRE(r.run() == PassStatus::COMPLETE);
  print("stub run, 3000 camera + 2000 light", r);
  check_all(r);
  const PassCounters& c = r.sched->counters();
  CHECK(c.camera_paths == 3000);
  CHECK(c.light_paths == 2000);
  CHECK(c.items[uint32_t(Stage::SHADE_LIGHT_PATH)] > 0);
  CHECK(c.items[uint32_t(Stage::SHADE_SURFACE)] > 0);
  // Every launched closest ray has a path; every shadow ray a shadow item.
  CHECK(c.rays_closest == 5000);
  CHECK(c.rays_shadow == c.items[uint32_t(Stage::SHADE_SURFACE)] + c.items[uint32_t(Stage::SHADE_LIGHT_PATH)]);
  CHECK(c.steps >= 3);
}

TEST_CASE("scheduler/pool of 2^10 drains all work") {
  Rig r(5000, 3000, 1u << 10);
  REQUIRE(r.run() == PassStatus::COMPLETE);
  print("pool 2^10, 5000 camera + 3000 light", r);
  CHECK(r.sched->capacity() == 1024);
  CHECK(r.sched->counters().refills > 1);
  CHECK(r.sched->counters().peak_active_paths <= 1024);
  check_all(r);
}

TEST_CASE("scheduler/permute invariance") {
  // Permuting item indices inside every segment before each launch changes slot and shadow-slot
  // assignment but not what any sample computes; shadow items arrive as the same multiset.
  Rig a(5000, 3000, 1u << 10, false);
  Rig b(5000, 3000, 1u << 10, true);
  REQUIRE(a.run() == PassStatus::COMPLETE);
  REQUIRE(b.run() == PassStatus::COMPLETE);
  CHECK(a.res.outcome == b.res.outcome);
  CHECK(a.res.shadow_delivered == b.res.shadow_delivered);
  CHECK(a.sched->counters().permuted_launches == 0);
  CHECK(b.sched->counters().permuted_launches > 0);
  // The schedule itself does not depend on the order inside segments.
  CHECK(a.sched->counters().steps == b.sched->counters().steps);
  for (uint32_t s = 0; s < ar::kStageCount; ++s) CHECK(a.sched->counters().items[s] == b.sched->counters().items[s]);
  // The permutation really moved items: the slot arrays of the two runs differ.
  const uint32_t* ia = a.sched->globals().path->sample_id;
  const uint32_t* ib = b.sched->globals().path->sample_id;
  uint32_t moved = 0;
  for (uint32_t i = 0; i < 1024; ++i) moved += ia[i] != ib[i];
  CHECK(moved > 0);
  check_all(b);
}

TEST_CASE("scheduler/sort host contract") {
  // 300000 paths in a 2^18 pool with 2 materials of 2 programs (default-material hits): the
  // SHADE_SURFACE segment passes kSortMinItems, the sort runs, and the result equals the unsorted
  // run, whose 2 materials share one program (RULINGS 80: the threshold counts programs).
  Rig sorted(200000, 100000, 1u << 18, false, 2);
  Rig plain(200000, 100000, 1u << 18, false, 2, &stub::shade_surface, 1);
  REQUIRE(sorted.run() == PassStatus::COMPLETE);
  REQUIRE(plain.run() == PassStatus::COMPLETE);
  CHECK(sorted.sched->counters().sorts > 0);
  CHECK(plain.sched->counters().sorts == 0);
  CHECK(sorted.res.outcome == plain.res.outcome);
  CHECK(sorted.res.shadow_delivered == plain.res.shadow_delivered);
  check_all(sorted);
}

TEST_CASE("scheduler/cancel and epoch stop at the next launch") {
  {
    Rig r(3000, 0, 0);
    r.res.on_closest = &r.sched->cancel;
    CHECK(r.run() == PassStatus::CANCELLED);
    const PassCounters& c = r.sched->counters();
    CHECK(c.items[uint32_t(Stage::INTERSECT_CLOSEST)] == 3000);
    CHECK(c.items[uint32_t(Stage::SHADE_SURFACE)] == 0);  // the launch after the cancelling one never ran
    CHECK(c.items[uint32_t(Stage::SHADE_BACKGROUND)] == 0);
  }
  {
    Rig r(3000, 0, 0);
    r.res.on_closest = &r.sched->epoch_pending;
    CHECK(r.run() == PassStatus::EPOCH_PENDING);
  }
}

TEST_CASE("scheduler/invalid items fail loudly") {
  Rig r(3000, 0, 0, false, 0, &stage_invalid);
  try {
    r.run();
    FAIL("run_pass did not throw");
  } catch (const std::runtime_error& e) {
    const std::string what = e.what();
    CHECK(what.find("next_stage is no stage of their pool") != std::string::npos);
    CHECK(what.find("stages launched in the previous step: SHADE_BACKGROUND, SHADE_LIGHT, SHADE_SURFACE") != std::string::npos);
  }
}

TEST_CASE("scheduler/short CPU launches fail loudly") {
  Rig r(5000, 0, 0);
  REQUIRE(r.run() == PassStatus::COMPLETE);
  const ar::StepTable& t = *r.sched->globals().step_table;
  const uint32_t grid = ar::queue_build_grid(t.high_water[0], t.high_water[1]);
  REQUIRE(grid > 1024);
  try {
    r.dev.launch(KernelId::QUEUE_BUILD_COUNT, grid - 1);
    FAIL("a short queue_build_count launch did not throw");
  } catch (const std::runtime_error& e) {
    const std::string want = "QUEUE_BUILD_COUNT launched with count " + std::to_string(grid - 1) +
                             ", its tile grid is " + std::to_string(grid);
    CHECK(std::string(e.what()).find(want) != std::string::npos);
  }
  CHECK_THROWS_AS(r.dev.launch(KernelId::QUEUE_BUILD_SCATTER, 0), std::runtime_error);
  // Sort: a 3000-item SHADE_SURFACE segment needs 3 tiles of 1024.
  const_cast<ar::StepTable&>(t).stage[uint32_t(Stage::SHADE_SURFACE)].count = 3000;
  CHECK_THROWS_AS(r.dev.launch(KernelId::SORT_COUNT, 3071), std::runtime_error);
  CHECK_THROWS_AS(r.dev.launch(KernelId::SORT_SCATTER, 1), std::runtime_error);
  CHECK_THROWS_AS(r.dev.launch(KernelId::QUEUE_BUILD_SCAN, 0), std::runtime_error);
  CHECK_NOTHROW(r.dev.launch(KernelId::QUEUE_BUILD_SCAN, 1));
}

TEST_CASE("scheduler/production CPU stages are registered") {
  ContextState ctx;
  Ledger ledger;
  ThreadPool pool(ctx, 2);
  CpuDevice dev(ledger, pool);
  dev.register_production_stages();
  // Each id is bound to its own function, so a swap of closest and shadow fails here.
  CHECK(dev.stage_fn(Stage::INTERSECT_CLOSEST) == &cpu_stage_intersect_closest);
  CHECK(dev.stage_fn(Stage::INTERSECT_SHADOW) == &cpu_stage_intersect_shadow);
  CHECK(dev.kernel_fn(KernelId::QUEUE_BUILD_COUNT) == &cpu_queue_build_count);
  CHECK(dev.kernel_fn(KernelId::QUEUE_BUILD_SCAN) == &cpu_queue_build_scan);
  CHECK(dev.kernel_fn(KernelId::QUEUE_BUILD_SCATTER) == &cpu_queue_build_scatter);
  CHECK(dev.kernel_fn(KernelId::SORT_COUNT) == &cpu_sort_count);
  CHECK(dev.kernel_fn(KernelId::SORT_SCAN) == &cpu_sort_scan);
  CHECK(dev.kernel_fn(KernelId::SORT_SCATTER) == &cpu_sort_scatter);
  // And only those: nothing else is bound, and a second registration is refused.
  CHECK(dev.stage_fn(Stage::SHADE_SURFACE) == nullptr);
  CHECK(dev.kernel_fn(KernelId::GENERATE_CAMERA) == nullptr);
  CHECK_THROWS_AS(dev.register_stage(Stage::INTERSECT_CLOSEST, &stub::intersect_closest), std::logic_error);
}

}  // TEST_SUITE

TEST_SUITE("gate") {

// Two runs of one stub workload, one with the shadow pool partitioned up to its high-water mark
// (RULINGS 84) and one over its whole capacity as before; shadow_capacity 0 keeps the default 2x.
// The ranged mark is kept growing here (keep_high_water) so its end value is the pass's peak; the
// mark that falls to live_end is compared with it in check_live_range.
void check_shadow_range(uint32_t cameras, uint32_t lights, uint64_t capacity, uint64_t shadow_capacity) {
  Rig ranged(cameras, lights, capacity);
  Rig full(cameras, lights, capacity);
  ranged.sched->config.keep_high_water = true;
  ranged.sched->config.shadow_capacity_override = full.sched->config.shadow_capacity_override = shadow_capacity;
  full.sched->config.shadow_full_range = true;
  REQUIRE(ranged.run() == PassStatus::COMPLETE);
  REQUIRE(full.run() == PassStatus::COMPLETE);
  const uint64_t shadow_cap = shadow_capacity ? shadow_capacity : 2 * ranged.sched->capacity();
  // The full range starts at the capacity and never moves; the ranged mark grew from 0 and stayed
  // inside the pool.
  CHECK(full.sched->shadow_high_water() == shadow_cap);
  CHECK(ranged.sched->shadow_high_water() > 0);
  CHECK(ranged.sched->shadow_high_water() <= shadow_cap);
  // The free list is the same slots in the same order either way, so everything agrees, counter
  // for counter.
  CHECK(ranged.res.outcome == full.res.outcome);
  CHECK(ranged.res.shadow_delivered == full.res.shadow_delivered);
  const PassCounters& a = ranged.sched->counters();
  const PassCounters& b = full.sched->counters();
  CHECK(std::memcmp(&a, &b, sizeof a) == 0);
  check_all(ranged);
}

TEST_CASE("scheduler/shadow range matches the full shadow range") {
  // A pool far below the work, the default shadow pool (2 x 1024): the stub's shadow items live
  // one or two steps, so the ranged mark stops below the capacity.
  check_shadow_range(5000, 3000, 1u << 10, 0);
  Rig r(5000, 3000, 1u << 10);
  r.sched->config.keep_high_water = true;  // the peak, not the drained pass's lowered mark
  REQUIRE(r.run() == PassStatus::COMPLETE);
  CHECK(r.sched->shadow_high_water() < 2048);
}

TEST_CASE("scheduler/shadow range under a starved shadow pool") {
  // 64 shadow slots for a 1024-path pool: producers are held back (floor(free_left / k)) once the
  // fresh slots run out, and nothing is dropped.
  check_shadow_range(5000, 3000, 1u << 10, 64);
}

// Two runs of one stub workload: the marks falling to the step table's live_end (task 134) and the
// marks only growing (keep_high_water, RULINGS 84 as 133 built it). Same slots in the same order, so
// outcomes, deliveries and every counter agree; the lowered run partitions fewer items.
void check_live_range(uint32_t cameras, uint32_t lights, uint64_t capacity, uint64_t shadow_capacity) {
  Rig lowered(cameras, lights, capacity);
  Rig kept(cameras, lights, capacity);
  lowered.sched->config.shadow_capacity_override = kept.sched->config.shadow_capacity_override = shadow_capacity;
  kept.sched->config.keep_high_water = true;
  REQUIRE(lowered.run() == PassStatus::COMPLETE);
  REQUIRE(kept.run() == PassStatus::COMPLETE);
  CHECK(lowered.res.outcome == kept.res.outcome);
  CHECK(lowered.res.shadow_delivered == kept.res.shadow_delivered);
  const PassCounters& a = lowered.sched->counters();
  const PassCounters& b = kept.sched->counters();
  CHECK(std::memcmp(&a, &b, sizeof a) == 0);
  CHECK(lowered.sched->partitioned_items() < kept.sched->partitioned_items());
  // The pass drains, so the last partition finds nothing live: the lowered shadow mark ends at 0.
  CHECK(lowered.sched->shadow_high_water() == 0);
  CHECK(kept.sched->shadow_high_water() > 0);
  check_all(lowered);
}

TEST_CASE("scheduler/live range matches kept high-water marks") {
  // A 1024-path pool refilled many times from 8000 samples (the refill takes the free slots below
  // the lowered main mark, then fresh ones), with the default and a starved shadow pool.
  check_live_range(5000, 3000, 1u << 10, 0);
  check_live_range(5000, 3000, 1u << 10, 64);
  // One refill fills the pool, as at 1080p 1 spp on the GPU (capacity = the pass's work).
  check_live_range(3000, 0, 0, 0);
}

}  // TEST_SUITE
