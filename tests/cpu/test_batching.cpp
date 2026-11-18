// SPDX-License-Identifier: Apache-2.0
// Interaction and progressive batching (0007 d49, d50; 0009 B-34, B-37): the rules on a synthetic
// clock, then a CPU session whose presented-frame record shows them at work.
// Ref: docs/decisions/0007-blender-integration.md d49, d50; 0009 B-34, B-37; Cycles
// intern/cycles/integrator/render_scheduler.cpp for the schedule values and the rounding.
#include <doctest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "device/cpu/cpu_device.h"
#include "host/scene/scene.h"
#include "host/session/batching.h"
#include "host/session/session.h"
#include "host/thread_pool.h"
#include "kernel/kernel_globals.h"
#include "kernel/launch_config.h"
#include "vm/ops.h"

using namespace astroray;
using namespace astroray::host;

namespace {
StoredView stored_view(float scale, uint32_t w = 64, uint32_t h = 48) {
  ar_view_desc v{};
  v.region_width = w;
  v.region_height = h;
  v.render_scale = scale;
  return StoredView::copy_of(v);
}

// An epoch in which `n` passes of 1/256 s ran (a power of two, so the sums are exact).
constexpr double kPassS = 1.0 / 256.0;
void run_passes(Batching& b, int n) {
  b.begin_epoch();
  for (int i = 0; i < n; ++i) b.note_pass(kPassS);
}

// ---- session rig: every pass ends its paths at once; the launch can take time or be held.
struct LaunchControl {
  std::mutex m;
  std::condition_variable cv;
  uint32_t launches = 0;
  uint32_t hold_at = 0;  // 0: never hold
  uint32_t sleep_ms = 0;
  void reset() {
    std::lock_guard<std::mutex> lock(m);
    launches = hold_at = sleep_ms = 0;
  }
  void on_launch() {
    std::unique_lock<std::mutex> lock(m);
    const uint32_t n = ++launches;
    cv.notify_all();
    const uint32_t ms = sleep_ms;
    lock.unlock();
    // Spin: sleep_for on Windows rounds up to the scheduler tick, far above a 2 ms pass.
    for (const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms); std::chrono::steady_clock::now() < until;) {}
    lock.lock();
    cv.wait(lock, [&] { return hold_at != n; });
  }
  void hold(uint32_t n) {
    std::lock_guard<std::mutex> lock(m);
    hold_at = n;
    cv.notify_all();
  }
  uint32_t count() {
    std::lock_guard<std::mutex> lock(m);
    return launches;
  }
};
LaunchControl g_launch;

void gen_terminate(const ar::KernelGlobals& kg, uint32_t first, uint32_t last) {
  if (first == 0) g_launch.on_launch();
  const ar::StepEntry& e = kg.step_table->stage[static_cast<uint32_t>(ar::Stage::TERMINATED)];
  for (uint32_t i = first; i < last; ++i)
    ar::ps_next_stage(kg, kg.main_queue[e.start + i]) = static_cast<uint8_t>(ar::Stage::TERMINATED);
}

struct Rig {
  Ledger ledger;
  ThreadPool pool;
  CpuDevice dev;
  std::unique_ptr<Session> s;  // last: destroyed first
  Rig(ContextState& ctx, Scene& scene) : pool(ctx, 2), dev(ledger, pool) {
    dev.register_production_stages();
    dev.register_kernel(KernelId::GENERATE_CAMERA, &gen_terminate);
    SessionOptions o;
    o.max_width = o.max_height = 8;
    o.test_camera_samples_per_pass = 1024;
    o.test_record_presents = true;
    s = std::make_unique<Session>(scene, dev, o);
  }
};

ar_view_desc view_of(uint32_t w, uint32_t h) {
  ar_view_desc v{};
  v.region_width = w;
  v.region_height = h;
  v.render_scale = 1.0f;
  v.camera.cam_to_world.m[0] = v.camera.cam_to_world.m[5] = v.camera.cam_to_world.m[10] = 1.0f;
  const float p[16] = {0.5f, 0, 0, 0, 0, 0.375f, 0, 0, 0, 0, 0, -1.0f, 0, 0, 0, 1.0f};
  for (int i = 0; i < 16; ++i) v.camera.projection_inverse[i] = p[i];
  v.camera.type = AR_CAMERA_PERSP;
  return v;
}

bool until(const std::function<bool()>& f, int timeout_ms = 20000) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!f()) {
    if (std::chrono::steady_clock::now() > end) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

constexpr double kEps = 1e-4;  // the clock the record uses is the one the waits use
}  // namespace

TEST_SUITE("fast") {

TEST_CASE("batching/one pass during interaction") {
  Batching b;
  b.note_commit(10.0);
  run_passes(b, 1000);  // a long, fast epoch: still one pass per update while interacting
  for (double t : {10.0, 10.05, 10.1, 10.249}) {
    CHECK(b.interacting(t));
    CHECK(b.passes_per_update(t, 0) == 1);
    CHECK(b.passes_per_update(t, 500) == 1);
    CHECK(b.publish_gap_s(t) == doctest::Approx(1.0 / 60.0));
  }
}

TEST_CASE("batching/passes grow after interaction") {
  Batching b;
  b.note_commit(10.0);
  const double after = 11.0;  // interaction is over; the schedule runs on path-trace time only
  CHECK_FALSE(b.interacting(after));
  // Passes of 1/256 s: trace time n / 256 s. Cycles' intervals 0.1, 0.25, 0.5, 1.0 s, then 2.0 s once
  // 32 samples are done; interval * 256 = 25, 64, 128, 256, 512, rounded to the nearer power of two.
  struct Row { int passes; uint64_t samples; double interval; uint32_t want; };
  const Row rows[] = {{1, 4, 0.1, 32},     {255, 4, 0.1, 32},    {256, 4, 0.25, 64},   {512, 4, 0.5, 128},
                      {1024, 4, 1.0, 256}, {2047, 31, 1.0, 256}, {2048, 31, 1.0, 256}, {2048, 32, 2.0, 512}};
  uint32_t last = 0;
  for (const Row& r : rows) {
    run_passes(b, r.passes);
    CHECK(display_interval_s(b.trace_s(), r.samples) == doctest::Approx(r.interval));
    CHECK(b.passes_per_update(after, r.samples) == r.want);
    CHECK(r.want >= last);
    last = r.want;
  }
  // The idle time since the interaction does not enter: the same epoch asked later gives the same.
  CHECK(b.passes_per_update(after + 1000.0, 32) == 512);
  // A pass slower than the interval still makes progress: at least one pass per update.
  b.begin_epoch();
  b.note_pass(3.0);
  CHECK(b.passes_per_update(after, 100) == 1);
  // No timed pass yet: one (Cycles' fallback).
  b.begin_epoch();
  CHECK(b.passes_per_update(after, 100) == 1);
  // A tiny pass time cannot overflow the count.
  run_passes(b, 1);
  b.begin_epoch();
  b.note_pass(1e-300);
  CHECK(b.passes_per_update(after, 100) >= 1);
  CHECK(b.publish_gap_s(after) == doctest::Approx(0.1));
  // 0009 B-34: after interaction no more than 10 frames/s, whatever the pass time.
  CHECK(display_interval_s(0.0, 0) >= 0.1);
}

TEST_CASE("batching/power of two rounding follows Cycles") {
  CHECK(round_to_power_of_2(0) == 1);
  CHECK(round_to_power_of_2(1) == 1);
  CHECK(round_to_power_of_2(8) == 8);
  CHECK(round_to_power_of_2(5) == 4);
  CHECK(round_to_power_of_2(6) == 8);  // a tie goes up
  CHECK(round_to_power_of_2(7) == 8);
  CHECK(round_to_power_of_2(11) == 8);
  CHECK(round_to_power_of_2(12) == 16);
  CHECK(round_to_power_of_2(0x7fffffffu) == 0x80000000u);  // no overflow at the int limit
}

TEST_CASE("batching/interaction ends 250 ms after the last event") {
  Batching b;
  CHECK_FALSE(b.interacting(5.0));  // nothing happened yet: no interaction, and no clock to read
  CHECK_FALSE(b.interacting(1e12));
  Batching fresh;  // before any event the answer depends on no time at all
  fresh.begin_epoch();
  fresh.note_pass(0.004);
  CHECK(fresh.passes_per_update(0.0, 4) == fresh.passes_per_update(1e9, 4));
  b.note_commit(10.0);
  CHECK(b.interacting(10.2499));
  CHECK_FALSE(b.interacting(10.25));
  b.note_view(stored_view(1.0), 10.2);  // a later event extends the window
  CHECK(b.interacting(10.4499));
  CHECK_FALSE(b.interacting(10.45));
  b.note_commit(10.3);  // an older timestamp never shortens it
  CHECK(b.interacting(10.4499));
}

TEST_CASE("batching/scale changes are ignored") {
  Batching b;
  CHECK(b.note_view(stored_view(1.0), 1.0));  // the first view differs from none
  CHECK_FALSE(b.interacting(2.0));
  CHECK_FALSE(b.note_view(stored_view(0.5), 2.0));  // the controller moved the scale only
  CHECK_FALSE(b.interacting(2.0));
  CHECK_FALSE(b.note_view(stored_view(1.0), 2.1));
  CHECK_FALSE(b.interacting(2.1));
  CHECK(b.note_view(stored_view(1.0, 65, 48), 3.0));  // a region change is a hash change
  CHECK(b.interacting(3.1));
  CHECK_FALSE(b.note_view(stored_view(1.0, 65, 48), 3.2));  // the same view again: no new event
  CHECK_FALSE(b.interacting(3.26));
}

}  // TEST_SUITE

// Wall-clock cases: the interaction window alone is 250 ms, over the fast tier's 0.5 s per test (B-12).
TEST_SUITE("gate") {

TEST_CASE("batching/session one pass per update during interaction") {
  // The view changes every 20 ms for 0.4 s while each pass takes 2 ms: every published frame carries
  // exactly one resolved pass, and frames are at least 1/60 s apart (the render thread waits out
  // the gap instead of running passes that would share a frame).
  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  g_launch.reset();
  g_launch.sleep_ms = 2;
  Rig r(ctx, scene);
  r.s->set_view(view_of(4, 4));
  r.s->start();
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
  for (uint32_t i = 0; std::chrono::steady_clock::now() < end; ++i) {
    r.s->set_view(view_of(4 + (i & 1), 4 + (i & 1)));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  const auto log = r.s->present_log();
  r.s->close();
  REQUIRE(log.size() >= 6);
  for (size_t i = 1; i < log.size(); ++i) {
    if (!log[i].interacting) continue;
    CHECK(log[i].target == 1);
    CHECK(log[i].passes_resolved - log[i - 1].passes_resolved == 1);
    CHECK(log[i].t_s - log[i - 1].t_s >= 1.0 / 60.0 - kEps);
  }
}

TEST_CASE("batching/session passes follow the schedule after interaction") {
  // One view, then 2 ms passes with nothing else happening. Interaction ends 250 ms after the view;
  // from then frames are at least 0.1 s apart and each carries at least the passes the schedule
  // asked for (more only when the gap held a frame back), the first of them more than one.
  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  g_launch.reset();
  g_launch.sleep_ms = 2;
  Rig r(ctx, scene);
  r.s->set_view(view_of(4, 4));
  r.s->start();
  std::this_thread::sleep_for(std::chrono::milliseconds(950));
  const auto log = r.s->present_log();
  r.s->close();
  size_t settled = 0;
  uint32_t first_target = 0, last_target = 0;
  for (size_t i = 1; i < log.size(); ++i) {
    if (log[i].interacting) continue;
    ++settled;
    CHECK(log[i].t_s - log[i - 1].t_s >= 0.1 - kEps);
    CHECK(log[i].passes_resolved - log[i - 1].passes_resolved >= log[i].target);
    if (!first_target) first_target = log[i].target;
    last_target = log[i].target;
  }
  CHECK(settled >= 4);
  CHECK(first_target > 1);
  CHECK(last_target >= first_target);  // the interval only grows as path-trace time accumulates
}

TEST_CASE("batching/session interrupt and first pass under rapid views") {
  // Pass 2 is held inside its launch; three views arrive within a millisecond (far faster than
  // 60/s). The first raises the interrupt (the state latched has presented), so pass 2 stops
  // unresolved, the three coalesce into one epoch, and that epoch's first pass presents once the
  // 1/60 s gap since the last frame allows.
  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  g_launch.reset();
  g_launch.hold_at = 2;
  Rig r(ctx, scene);
  r.s->set_view(view_of(4, 4));
  r.s->start();
  REQUIRE(until([&] { return g_launch.count() >= 2; }));
  CHECK(r.s->stats().passes_resolved == 1);
  r.s->set_view(view_of(5, 5));
  r.s->set_view(view_of(6, 6));
  r.s->set_view(view_of(7, 7));
  g_launch.hold(3);  // releases launch 2, holds launch 3: the first pass of the new epoch
  REQUIRE(until([&] { return g_launch.count() >= 3; }));
  CHECK(r.s->stats().passes_resolved == 1);
  CHECK(r.s->stats().passes_interrupted == 1);
  CHECK(r.s->latch().epoch() == 2);
  g_launch.hold(0);
  REQUIRE(until([&] { return r.s->present_log().size() >= 2; }));
  const auto log = r.s->present_log();
  r.s->close();
  CHECK(log[0].epoch == 1);
  CHECK(log[1].epoch == 2);
  CHECK(log[1].passes_resolved == 2);
  CHECK(log[1].t_s - log[0].t_s >= 1.0 / 60.0 - kEps);
}

}  // TEST_SUITE
