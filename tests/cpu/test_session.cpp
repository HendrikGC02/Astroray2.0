// SPDX-License-Identifier: Apache-2.0
// Sessions on the CPU device (0007 d23, d25, d27; 0004 D25; RULINGS 52, 69): the latch coalesces
// pending views into one latched state per pass boundary, an epoch resets the film, cancel stops
// at the next launch, two sessions of one scene render at the same time, and an epoch change
// interrupts a pass only after the latched state has presented.
// Production passes have no work until the active-pixel list exists, so the tests that need
// launches give the session work through its test hook and register a generate kernel that ends
// every path at once and reports each launch to a gate the test controls.
#include <doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "device/cpu/cpu_device.h"
#include "host/scene/edit.h"
#include "host/scene/scene.h"
#include "host/session/session.h"
#include "host/thread_pool.h"
#include "kernel/kernel_globals.h"

using namespace astroray;
using namespace astroray::host;

namespace {

// One per generate kernel instance: counts launches, can cancel a session at launch n and can
// hold launch n until released.
struct Gate {
  std::mutex m;
  std::condition_variable cv;
  uint32_t launches = 0;
  uint32_t hold_at = 0;    // 0: never hold
  uint32_t cancel_at = 0;  // 0: never cancel
  Session* target = nullptr;
  void reset() {
    std::lock_guard<std::mutex> lock(m);
    launches = hold_at = cancel_at = 0;
    target = nullptr;
  }
  void on_launch() {
    std::unique_lock<std::mutex> lock(m);
    const uint32_t n = ++launches;
    cv.notify_all();
    if (n == cancel_at && target) target->cancel();
    cv.wait(lock, [&] { return hold_at != n; });
  }
  void release() {
    std::lock_guard<std::mutex> lock(m);
    hold_at = 0;
    cv.notify_all();
  }
  uint32_t count() {
    std::lock_guard<std::mutex> lock(m);
    return launches;
  }
};

Gate g_gates[2];

// Item i takes slot main_queue[start + i] (scheduler.h refill contract) and ends there.
template <int I>
void gen_terminate(const ar::KernelGlobals& kg, uint32_t first, uint32_t last) {
  if (first == 0) g_gates[I].on_launch();
  const ar::StepEntry& e = kg.step_table->stage[static_cast<uint32_t>(ar::Stage::TERMINATED)];
  for (uint32_t i = first; i < last; ++i)
    ar::ps_next_stage(kg, kg.main_queue[e.start + i]) = static_cast<uint8_t>(ar::Stage::TERMINATED);
}

struct Rig {
  Ledger ledger;
  ThreadPool pool;
  CpuDevice dev;
  std::unique_ptr<Session> s;  // last: destroyed first
  Rig(ContextState& ctx, Scene& scene, uint32_t size, uint64_t work, CpuKernelFn gen) : pool(ctx, 2), dev(ledger, pool) {
    dev.register_production_stages();
    if (gen) dev.register_kernel(KernelId::GENERATE_CAMERA, gen);
    SessionOptions o;
    o.max_width = o.max_height = size;
    o.test_camera_samples_per_pass = work;
    s = std::make_unique<Session>(scene, dev, o);
  }
};

bool until(const std::function<bool()>& f, int timeout_ms = 20000) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!f()) {
    if (std::chrono::steady_clock::now() > end) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

ar_view_desc view_of(uint32_t w, uint32_t h) {
  ar_view_desc v{};
  v.region_width = w;
  v.region_height = h;
  v.render_scale = 1.0f;
  v.camera.cam_to_world.m[0] = v.camera.cam_to_world.m[5] = v.camera.cam_to_world.m[10] = 1.0f;
  return v;
}

uint32_t state(const Session& s) { return s.progress().state; }

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("session/views coalesce per pass") {
  // Three views arrive while the first pass of the first epoch runs. That state has not
  // presented, so the pass is not interrupted (0007 d25): it is resolved and presented, and the
  // next boundary folds all three views into one latch and one new epoch.
  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  g_gates[0].reset();
  g_gates[0].hold_at = 1;
  Rig r(ctx, scene, 8, 1024, &gen_terminate<0>);
  r.s->start();
  REQUIRE(until([&] { return g_gates[0].count() >= 1; }));
  CHECK(r.s->latch().epoch() == 1);
  r.s->set_view(view_of(4, 4));
  r.s->set_view(view_of(5, 5));
  r.s->set_view(view_of(6, 6));
  g_gates[0].release();
  REQUIRE(until([&] { return r.s->latch().epoch() >= 2 && r.s->stats().passes_resolved >= 3; }));
  const SessionStats st = r.s->stats();
  CHECK(r.s->latch().epoch() == 2);
  CHECK(r.s->latch().latches() == 2);  // the first latch, then one for the three views
  CHECK(r.s->latch().view().v.region_width == 6);
  CHECK(st.passes_interrupted == 0);
  CHECK(r.s->progress().active_generation == 0);  // RULINGS 69: views allocate no number
  r.s->close();
  REQUIRE(r.s->wait(20000));
  CHECK(state(*r.s) == AR_STATE_CLOSED);
}

TEST_CASE("session/epoch change resets film") {
  // Production passes (no work): every latch renders one pass, presents it and converges. The
  // resolve still counts one sample per pixel, so n tells whether the film was reset.
  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  Rig r(ctx, scene, 4, 0, nullptr);
  r.s->start();
  REQUIRE(r.s->wait(20000));
  REQUIRE(state(*r.s) == AR_STATE_CONVERGED);
  CHECK(r.s->latch().epoch() == 1);
  CHECK(r.s->film_samples(0) == 1);
  CHECK(r.s->stats().frames_published == 1);
  const double value[2] = {0, 0};
  REQUIRE(r.s->report().raise(AR_REPORT_INVALID_SAMPLE, 0, "", 1, value, 0) == AR_OK);

  r.s->set_view(view_of(4, 4));  // the first view differs from none: a new epoch
  REQUIRE(r.s->wait(20000));
  REQUIRE(state(*r.s) == AR_STATE_CONVERGED);
  CHECK(r.s->latch().epoch() == 2);
  CHECK(r.s->film_samples(0) == 1);  // 2 if the film had kept the first epoch's pass
  CHECK(r.s->stats().samples_done == 1);
  r.s->report().snapshot();
  ar_report_info info{};
  REQUIRE(r.s->report().info(&info) == AR_OK);
  CHECK(info.entry_count == 0);  // FILM entries go with the film

  r.s->set_view(view_of(4, 4));  // the same view: nothing changes
  REQUIRE(r.s->wait(20000));
  CHECK(r.s->latch().epoch() == 2);
  CHECK(r.s->film_samples(0) == 1);
  CHECK(r.s->stats().frames_published == 2);
}

TEST_CASE("session/commit wakes a converged session") {
  // The scene's activation listener (0007 d25): an idle session latches the commit at once, with no
  // other call to wake it, and the commit starts an epoch.
  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  Rig r(ctx, scene, 4, 0, nullptr);
  r.s->start();
  REQUIRE(r.s->wait(20000));
  REQUIRE(state(*r.s) == AR_STATE_CONVERGED);
  CHECK(r.s->latch().epoch() == 1);
  uint64_t g = 0;
  {
    auto e = scene.begin_edit();
    REQUIRE(e);
    REQUIRE(e->commit(&g) == AR_OK);
  }
  REQUIRE(r.s->wait(20000));
  CHECK(state(*r.s) == AR_STATE_CONVERGED);
  CHECK(r.s->progress().active_generation == g);
  CHECK(r.s->latch().epoch() == 2);
  CHECK(r.s->stats().frames_published == 2);
}

TEST_CASE("session/cancel stops at the next launch") {
  // Three generate launches per pass (work = 3 pools of 2^16); the first one cancels.
  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  g_gates[0].reset();
  Rig r(ctx, scene, 4, 3 * kCpuPoolCapacity, &gen_terminate<0>);
  g_gates[0].cancel_at = 1;
  g_gates[0].target = r.s.get();
  r.s->start();
  REQUIRE(until([&] { return state(*r.s) == AR_STATE_PAUSED; }));
  const SessionStats st = r.s->stats();
  CHECK(g_gates[0].count() == 1);
  CHECK(st.passes_cancelled == 1);
  CHECK(st.passes_resolved == 0);  // the cancelled pass is never resolved
  CHECK(st.frames_published == 0);
}

}  // TEST_SUITE

TEST_SUITE("gate") {

TEST_CASE("session/two sessions on one scene run concurrently") {
  // Session A is held inside its first launch; B must keep resolving passes meanwhile.
  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  g_gates[0].reset();
  g_gates[1].reset();
  g_gates[0].hold_at = 1;
  Rig a(ctx, scene, 8, 1024, &gen_terminate<0>);
  Rig b(ctx, scene, 8, 1024, &gen_terminate<1>);
  a.s->start();
  b.s->start();
  REQUIRE(until([&] { return g_gates[0].count() >= 1; }));
  CHECK(until([&] { return b.s->stats().passes_resolved >= 5; }));
  CHECK(a.s->stats().passes_resolved == 0);
  g_gates[0].release();
  CHECK(until([&] { return a.s->stats().passes_resolved >= 5; }));
  a.s->close();
  b.s->close();
  CHECK(a.s->wait(20000));
  CHECK(b.s->wait(20000));
}

TEST_CASE("session/epoch change interrupts a presented pass") {
  // Pass 1 resolves and presents; pass 2 is held inside its launch while a new view arrives.
  // The latched state has presented, so pass 2 stops at its next launch and is not resolved.
  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  g_gates[0].reset();
  g_gates[0].hold_at = 2;
  Rig r(ctx, scene, 8, 1024, &gen_terminate<0>);
  r.s->start();
  REQUIRE(until([&] { return g_gates[0].count() >= 2; }));
  CHECK(r.s->stats().passes_resolved == 1);
  r.s->set_view(view_of(4, 4));
  g_gates[0].release();
  REQUIRE(until([&] { return r.s->latch().epoch() >= 2 && r.s->stats().passes_resolved >= 2; }));
  CHECK(r.s->stats().passes_interrupted == 1);
  CHECK(r.s->latch().epoch() == 2);
}

}  // TEST_SUITE
