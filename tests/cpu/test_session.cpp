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
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "device/cpu/cpu_device.h"
#include "host/material/material_store.h"
#include "host/scene/edit.h"
#include "host/scene/instances.h"
#include "host/scene/mesh.h"
#include "host/scene/scene.h"
#include "host/session/session.h"
#include "host/thread_pool.h"
#include "kernel/kernel_globals.h"
#include "kernel/launch_config.h"
#include "vm/ops.h"

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
  Rig(ContextState& ctx, Scene& scene, uint32_t size, uint64_t work, CpuKernelFn gen,
      uint32_t mode = AR_SESSION_MODE_VIEWPORT)
      : pool(ctx, 2), dev(ledger, pool) {
    dev.register_production_stages();
    if (gen) dev.register_kernel(KernelId::GENERATE_CAMERA, gen);
    SessionOptions o;
    o.mode = mode;
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
  // A valid pinhole: an invalid descriptor raises a report entry (camera_wiring tests that).
  const float p[16] = {0.5f, 0, 0, 0, 0, 0.375f, 0, 0, 0, 0, 0, -1.0f, 0, 0, 0, 1.0f};
  for (int i = 0; i < 16; ++i) v.camera.projection_inverse[i] = p[i];
  v.camera.type = AR_CAMERA_PERSP;
  return v;
}

uint32_t state(const Session& s) { return s.progress().state; }

// A scene worker job that waits until the test opens it, so its generation stays pending.
struct JobGate {
  std::mutex m;
  std::condition_variable cv;
  bool open = false;
  void pass() {
    std::unique_lock<std::mutex> lock(m);
    cv.wait(lock, [&] { return open; });
  }
  void release() {
    {
      std::lock_guard<std::mutex> lock(m);
      open = true;
    }
    cv.notify_all();
  }
};

// Commits generation 1 with one item whose job waits at `gate` and then fails if `fail`; then starts
// a FINAL session with production passes (no work, as F12 on a scene whose tables are not active yet)
// and returns the generation. A session that latched at once would converge on generation 0.
uint64_t commit_held_then_start(Scene& scene, Session& s, const std::shared_ptr<JobGate>& gate, bool fail) {
  auto item = std::make_shared<EditItem>();
  item->key = 99;
  item->needs_device = false;
  item->job = [gate, fail] {
    gate->pass();
    if (fail) throw std::runtime_error("held item fails");
  };
  uint64_t g = 0;
  {
    auto e = scene.begin_edit();
    REQUIRE(e);
    REQUIRE(e->stage(item) == AR_OK);
    REQUIRE(e->item_commit(99) == AR_OK);
    REQUIRE(e->commit(&g) == AR_OK);
  }
  REQUIRE(scene.generations().active() < g);
  s.start();
  return g;
}

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

TEST_CASE("session/final session waits for the generation committed before start" * doctest::test_suite("gate")) {
  // 0007 d25: F12 commits and starts at once; the final session renders that commit, never the
  // generation before it, however late the commit's jobs finish.
  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  auto gate = std::make_shared<JobGate>();
  Rig r(ctx, scene, 4, 0, nullptr, AR_SESSION_MODE_FINAL);
  struct Opener {
    std::shared_ptr<JobGate> g;
    ~Opener() { g->release(); }  // before the rig and the scene: the worker job ends on any exit
  } opener{gate};
  const uint64_t g = commit_held_then_start(scene, *r.s, gate, false);
  // A session that latched generation 0 would converge within one empty pass (well under 1 ms).
  CHECK_FALSE(r.s->wait(100));
  CHECK(state(*r.s) != AR_STATE_CONVERGED);
  CHECK(r.s->stats().passes_resolved == 0);
  gate->release();
  REQUIRE(r.s->wait(20000));
  CHECK(state(*r.s) == AR_STATE_CONVERGED);
  CHECK(r.s->progress().active_generation >= g);
  CHECK(r.s->latch().epoch() == 1);
}

TEST_CASE("session/final session fails loudly when its start generation is dropped" * doctest::test_suite("gate")) {
  // The commit's job fails: the generation is dropped, the tracker's listener wakes the waiting session,
  // and it converges on the previous generation with GENERATION_REJECTED in its report (an F12 error).
  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  auto gate = std::make_shared<JobGate>();
  Rig r(ctx, scene, 4, 0, nullptr, AR_SESSION_MODE_FINAL);
  struct Opener {
    std::shared_ptr<JobGate> g;
    ~Opener() { g->release(); }
  } opener{gate};
  const uint64_t g = commit_held_then_start(scene, *r.s, gate, true);
  CHECK_FALSE(r.s->wait(100));
  gate->release();
  REQUIRE(r.s->wait(5000));
  CHECK(state(*r.s) == AR_STATE_CONVERGED);
  CHECK(r.s->progress().failed_generation == g);
  const ar_report_info info = r.s->report_snapshot();
  bool rejected = false;
  for (uint32_t i = 0; i < info.entry_count; ++i) {
    ar_report_entry e{};
    r.s->report_entry(i, &e);
    rejected = rejected || (e.code == AR_REPORT_GENERATION_REJECTED && e.level == AR_LEVEL_ERROR);
  }
  CHECK(rejected);
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

TEST_CASE("session/pass time is published with its sample count" * doctest::test_suite("gate")) {
  // A poll that sees a new sample count reads that pass's time, never the previous pass's (0 before
  // the first pass): both are published under one lock. The hook holds the render thread between the
  // resolve and the publish, where publishing the count early would show it without its time.
  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  Ledger ledger;
  ThreadPool pool(ctx, 2);
  CpuDevice dev(ledger, pool);
  dev.register_production_stages();
  SessionOptions o;
  o.max_width = o.max_height = 4;
  o.test_sleep_after_resolve_ms = 100;
  Session s(scene, dev, o);
  s.start();
  SessionProgress p;
  REQUIRE(until([&] {
    p = s.progress();
    return p.samples_per_pixel >= 1;
  }));
  CHECK(p.samples_per_pixel == 1);
  CHECK(p.last_pass_ms > 0.0f);
  REQUIRE(s.wait(20000));
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

TEST_CASE("session/wait wakes on pause") {
  // 0007 d27: F12 loops on wait; a cancel ends in PAUSED, which must end the wait (0009 B-37) rather
  // than its timeout. Launch 1 is held so the cancel lands while a pass is in flight.
  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  g_gates[0].reset();
  g_gates[0].hold_at = 1;
  Rig r(ctx, scene, 4, 16, &gen_terminate<0>);
  r.s->start();
  REQUIRE(until([&] { return g_gates[0].count() >= 1; }));
  r.s->cancel();
  g_gates[0].release();
  CHECK(r.s->wait(5000));
  CHECK(state(*r.s) == AR_STATE_PAUSED);
}

TEST_CASE("session/resume leaves PAUSED before wait can see it" * doctest::test_suite("gate")) {
  // wait() returns on PAUSED, so resume() itself ends PAUSED: a wait right after it must not return
  // for the pause just lifted (the render thread leaves PAUSED only at its loop top). Twenty rounds,
  // each read straight after resume().
  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  g_gates[0].reset();
  Rig r(ctx, scene, 4, 16, &gen_terminate<0>);
  r.s->start();
  for (int round = 0; round < 20; ++round) {
    r.s->pause();
    REQUIRE(until([&] { return state(*r.s) == AR_STATE_PAUSED; }));
    r.s->resume();
    const uint32_t after = state(*r.s);
    CHECK(after != AR_STATE_PAUSED);
  }
  CHECK_FALSE(r.s->wait(5));  // rendering with no sample limit: only the timeout ends this wait
  CHECK(state(*r.s) == AR_STATE_RENDERING);
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


namespace {

// Every camera path of the pass arrives at SHADE_SURFACE on triangle i % 2 of instance 0, and the
// stub stage ends it: the sort decision then depends only on the segment size and the scene's
// program count (kSortMinItems, kSortMinMaterials).
void gen_to_surface(const ar::KernelGlobals& kg, uint32_t first, uint32_t last) {
  const ar::StepEntry& e = kg.step_table->stage[static_cast<uint32_t>(ar::Stage::TERMINATED)];
  for (uint32_t i = first; i < last; ++i) {
    const uint32_t slot = kg.main_queue[e.start + i];
    ar::ps_flags(kg, slot) = 0u;
    ar::ps_hit_inst(kg, slot) = 0u;
    ar::ps_hit_prim(kg, slot) = i & 1u;
    ar::ps_next_stage(kg, slot) = static_cast<uint8_t>(ar::Stage::SHADE_SURFACE);
  }
}

void surface_terminate(const ar::KernelGlobals& kg, const ar::StepEntry& step, uint32_t first, uint32_t last) {
  for (uint32_t i = first; i < last; ++i)
    ar::ps_next_stage(kg, kg.main_queue[step.start + i]) = static_cast<uint8_t>(ar::Stage::TERMINATED);
}

// A quad instanced once with `materials` emission materials in its slot pool. Material 0 emits the
// shading position, a per-point input, so it compiles a program of its own; material 1's graph
// folds to constants (CONSTANT_SURFACE): `materials` materials run `materials` surface programs.
// The default material (constant, last in the header table) must not count: with one material it
// would make a second program.
void commit_quad(Scene& scene, uint32_t materials) {
  scene.meshes = std::make_shared<MeshStore>();
  scene.materials = std::make_shared<MaterialStore>(scene.meshes->shared_ledger());
  const auto commit = [&](auto&& body) {
    auto e = scene.begin_edit();
    REQUIRE(e);
    body(*e);
    uint64_t g = 0;
    REQUIRE(e->commit(&g) == AR_OK);
    scene.materials->end_edit();
    e.reset();
    scene.generations().wait_idle();
    REQUIRE(scene.generations().failed() == 0);
  };
  std::vector<ar_key> keys;
  commit([&](Edit& e) {
    for (uint32_t m = 0; m < materials; ++m) {
      std::vector<ar_graph_node> nodes(3);
      std::vector<ar_graph_link> links{{0, 0, 2, 0}, {1, 0, 2, 1}};
      std::vector<float> f{1.0f, 0.5f, 0.25f, 1.0f + float(m)};
      if (m == 0) nodes[0].op = uint32_t(ar::Op::LOAD_CTX_P);
      else nodes[0].op = uint32_t(ar::Op::CONST_F32X3), nodes[0].f32_count = 3;
      nodes[1].op = uint32_t(ar::Op::CONST_F32), nodes[1].f32_first = 3, nodes[1].f32_count = 1;
      nodes[2].op = uint32_t(ar::Op::EMIT_LE);
      ar_graph_desc d = {};
      d.node_count = 3, d.link_count = 2, d.f32_count = 4, d.nodes = nodes.data(), d.links = links.data(), d.params_f32 = f.data();
      REQUIRE(scene.materials->set_graph(e, 1000 + m, &d) == AR_OK);
      ar_material_desc md = {};
      md.surface_graph = 1000 + m;
      md.emission_sampling = AR_EMISSION_SAMPLING_AUTO;
      REQUIRE(scene.materials->set_material(e, 1 + m, &md) == AR_OK);
      keys.push_back(1 + m);
    }
    const std::vector<float> pos{0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    const std::vector<int32_t> cv{0, 1, 2, 3}, fs{0};
    ar_mesh_desc d = {};
    d.key = 100, d.vertex_count = 4, d.corner_count = 4, d.face_count = 1, d.motion_steps = 1;
    ar_mesh_buffers b = {};
    REQUIRE(mesh_begin(scene, e, &d, &b) == AR_OK);
    std::memcpy(*b.positions, pos.data(), pos.size() * 4);
    std::memcpy(*b.corner_vert, cv.data(), cv.size() * 4);
    std::memcpy(*b.face_start, fs.data(), fs.size() * 4);
    for (uint64_t i = 0; i < b.corner_normals_count; ++i) (*b.corner_normals)[i] = i % 3 == 2 ? 1.0f : 0.0f;
    REQUIRE(e.item_commit(100) == AR_OK);
  });
  commit([&](Edit& e) {
    std::vector<ar_key> geom{100}, object{1};
    std::vector<ar_xform> xf(1);
    xf[0].m[0] = xf[0].m[5] = xf[0].m[10] = 1.0f;
    std::vector<uint32_t> zeros(1, 0), vis(1, AR_VISIBILITY_CAMERA), moff{0}, mcount{uint32_t(keys.size())};
    std::vector<ar_rgba> colors(1, ar_rgba{{1, 1, 1, 1}});
    std::vector<uint8_t> lg(1, 0);
    ar_instances_desc d = {};
    d.instance_count = 1, d.motion_steps = 1, d.transform_count = 1, d.material_key_count = uint32_t(keys.size());
    d.geom_keys = geom.data(), d.xf = xf.data(), d.object_keys = object.data();
    d.random_ids = d.pass_indices = d.crypto_objects = d.crypto_assets = zeros.data();
    d.colors = colors.data(), d.visibility = vis.data(), d.flags = zeros.data(), d.light_groups = lg.data();
    d.material_offsets = moff.data(), d.material_counts = mcount.data(), d.material_keys = keys.data();
    REQUIRE(set_instances(e, &d) == AR_OK);
  });
}

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("session/shade_surface items are sorted once the scene has two material programs") {
  // The session hands the pinned table set's program count to the scheduler at each latch, so a
  // pass whose SHADE_SURFACE segment holds kSortMinItems paths sorts it with two materials of two
  // programs and not with one (0002 D7 step 3, RULINGS 80).
  for (uint32_t materials : {2u, 1u}) {
    ContextState ctx;
    Scene scene(ctx, SceneConfig{2, {}, {}});
    commit_quad(scene, materials);
    Rig r(ctx, scene, 8, ar::kSortMinItems, &gen_to_surface);
    r.dev.register_stage(ar::Stage::SHADE_SURFACE, &surface_terminate);
    r.s->start();
    REQUIRE(until([&] { return r.s->stats().passes_resolved >= 1; }));
    const PassCounters pc = r.s->pass_counters();
    std::printf("session sort: %u materials, %llu SHADE_SURFACE items, %llu sorts\n", materials,
                static_cast<unsigned long long>(pc.items[static_cast<uint32_t>(ar::Stage::SHADE_SURFACE)]),
                static_cast<unsigned long long>(pc.sorts));
    CHECK(pc.items[static_cast<uint32_t>(ar::Stage::SHADE_SURFACE)] >= ar::kSortMinItems);
    CHECK((pc.sorts > 0) == (materials >= ar::kSortMinMaterials));
    r.s->close();
    CHECK(until([&] { return state(*r.s) == AR_STATE_CLOSED; }));
    CHECK(r.s->failure().empty());
  }
}

}  // TEST_SUITE