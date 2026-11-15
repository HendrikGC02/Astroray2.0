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
  void release() { hold(0); }
  // Moves the hold to launch n (0: none), which releases a launch held at the old value.
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

TEST_CASE("session/latch spends the interrupt with the epoch") {
  // 0007 d25: an interrupt is due only for pending epoch-starting state after the latched state has
  // presented. The latch that starts the epoch consumes it: right after, nothing has presented, so
  // a view arriving then waits for the pass to complete instead of interrupting it.
  Latch l;
  CHECK(l.latch(0).new_epoch);
  CHECK_FALSE(l.interrupt_due(0));  // nothing pending
  l.mark_presented();
  l.set_view(view_of(4, 4));
  CHECK(l.interrupt_due(0));
  const LatchResult r = l.latch(0);
  CHECK(r.new_epoch);
  CHECK(r.epoch == 2);
  CHECK_FALSE(l.interrupt_due(0));
  l.set_view(view_of(5, 5));  // pending, epoch-starting, but epoch 2 has not presented
  CHECK(l.pending(0));
  CHECK_FALSE(l.interrupt_due(0));
  l.mark_presented();
  CHECK(l.interrupt_due(0));
  CHECK(l.interrupt_due(1));  // a newly active commit is due as well
}

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
  CHECK(until([&] { return state(*r.s) == AR_STATE_CLOSED; }));
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

TEST_CASE("session/stale epoch resolve is dropped") {
  // Writes carry their epoch (0004 D25): the test hook tags the first resolve with the previous
  // epoch, so it is dropped; the next pass of the same epoch resolves normally.
  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  Ledger ledger;
  ThreadPool pool(ctx, 2);
  CpuDevice dev(ledger, pool);
  dev.register_production_stages();
  SessionOptions o;
  o.max_width = o.max_height = 4;
  o.test_stale_resolve_at_pass = 1;
  Session s(scene, dev, o);
  s.start();
  REQUIRE(s.wait(20000));
  REQUIRE(state(s) == AR_STATE_CONVERGED);
  CHECK(s.stats().stale_dropped == 1);
  CHECK(s.stats().passes_resolved == 1);
  CHECK(s.film_samples(0) == 1);
}

TEST_CASE("session/failed session refuses start, resume and set_view") {
  // A heat map with no sample limit cannot be drawn: the render thread fails the session, and
  // FAILED is final until close (0007 d5; rule 5: no call on it reports success).
  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  Rig r(ctx, scene, 4, 0, nullptr);
  r.s->start();
  REQUIRE(r.s->wait(20000));
  r.s->set_display_mode(AR_DISPLAY_SAMPLE_COUNT);
  REQUIRE(until([&] { return state(*r.s) == AR_STATE_FAILED; }));
  CHECK(r.s->failure().find("range_max") != std::string::npos);
  CHECK_THROWS_WITH_AS(r.s->start(), doctest::Contains("range_max"), std::runtime_error);
  CHECK_THROWS_WITH_AS(r.s->resume(), doctest::Contains("range_max"), std::runtime_error);
  CHECK_THROWS_WITH_AS(r.s->set_view(view_of(4, 4)), doctest::Contains("range_max"), std::runtime_error);
  CHECK(state(*r.s) == AR_STATE_FAILED);
  r.s->close();
  CHECK(until([&] { return state(*r.s) == AR_STATE_CLOSED; }));
}

TEST_CASE("session/scene destroyed after CLOSED") {
  // 0007 d5 allows destroying the scene once its session reports CLOSED and the session after it:
  // by CLOSED the session has dropped its scene listener and never touches the scene again.
  ContextState ctx;
  auto scene = std::make_unique<Scene>(ctx, SceneConfig{1, {}});
  auto rig = std::make_unique<Rig>(ctx, *scene, 4, 0, nullptr);
  rig->s->start();
  REQUIRE(rig->s->wait(20000));
  CHECK(scene->generations().listener_count() == 1);
  rig->s->close();
  // CONVERGED is terminal too, so wait() alone may return before CLOSED.
  REQUIRE(until([&] { return state(*rig->s) == AR_STATE_CLOSED; }));
  // REQUIRE, so a failure here unwinds with the session destroyed before the scene.
  REQUIRE(scene->generations().listener_count() == 0);
  const SessionProgress before = rig->s->progress();
  scene.reset();
  const SessionProgress after = rig->s->progress();
  CHECK(after.state == AR_STATE_CLOSED);
  CHECK(after.epoch == before.epoch);
  rig.reset();
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
  CHECK(until([&] { return state(*a.s) == AR_STATE_CLOSED && state(*b.s) == AR_STATE_CLOSED; }));
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
  // Release launch 2 and hold launch 3, the first pass of epoch 2: while it is held, the
  // interrupted pass 2 must be counted as interrupted and never resolved.
  g_gates[0].hold(3);
  REQUIRE(until([&] { return g_gates[0].count() >= 3; }));
  const SessionStats st = r.s->stats();
  CHECK(st.passes_resolved == 1);
  CHECK(st.passes_interrupted == 1);
  CHECK(r.s->latch().epoch() == 2);
  g_gates[0].release();
  CHECK(until([&] { return r.s->stats().passes_resolved >= 2; }));
  CHECK(r.s->stats().passes_interrupted == 1);
}

TEST_CASE("session/view away and back after an interrupt forces an epoch") {
  // Orbit away and straight back (B, then the latched A) while a pass of a presented state runs:
  // B interrupts the pass, A makes the pending view equal the latched one, and the next latch must
  // still start an epoch, because the interrupted pass left partial writes only the reset clears.
  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  g_gates[0].reset();
  g_gates[0].hold_at = 2;
  Rig r(ctx, scene, 8, 1024, &gen_terminate<0>);
  r.s->set_view(view_of(6, 6));  // A, latched at the first boundary
  r.s->start();
  REQUIRE(until([&] { return g_gates[0].count() >= 2; }));
  CHECK(r.s->stats().passes_resolved == 1);
  CHECK(r.s->latch().epoch() == 1);
  r.s->set_view(view_of(4, 4));  // B: the presented state is interrupted
  r.s->set_view(view_of(6, 6));  // back to A before the boundary
  g_gates[0].hold(3);
  REQUIRE(until([&] { return g_gates[0].count() >= 3; }));
  const SessionStats st = r.s->stats();
  CHECK(state(*r.s) == AR_STATE_RENDERING);
  CHECK(st.passes_interrupted == 1);
  CHECK(st.passes_resolved == 1);
  CHECK(st.samples_done == 0);  // the epoch reset ran: the film restarts
  CHECK(r.s->latch().epoch() == 2);
  CHECK(r.s->latch().view().v.region_width == 6);
  g_gates[0].release();
  CHECK(until([&] { return r.s->stats().passes_resolved >= 2; }));
  CHECK(r.s->failure().empty());
}
}  // TEST_SUITE
