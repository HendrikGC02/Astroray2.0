// SPDX-License-Identifier: Apache-2.0
// Interaction and progressive batching (0007 d49, d50; 0009 B-34, B-37): the rules on a synthetic
// clock, then a CPU session whose presented-frame record shows them at work.
// Ref: docs/decisions/0007-blender-integration.md d49, d50; 0009 B-34, B-37; Cycles
// intern/cycles/integrator/render_scheduler.cpp for the schedule values and the rounding.
#include <doctest.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

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

TEST_CASE("batching/session passes run back to back during interaction") {
  // A continuous orbit (a view every few ms) over 20 ms passes, longer than the 1/60 s gap: the cap
  // never binds, so a pass starts as soon as the previous frame is out and frames come about one
  // pass apart. Waiting out the gap before each pass instead makes every interval at least
  // gap + pass. Each frame is one pass of a newer view: the views set during a pass are latched
  // at the next pass start, and the frame still follows its own pass (0009 B-32).
  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  g_launch.reset();
  constexpr uint32_t kSlowPassMs = 20;
  g_launch.sleep_ms = kSlowPassMs;
  Rig r(ctx, scene);
  r.s->set_view(view_of(4, 4));
  r.s->start();
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
  for (uint32_t i = 1; std::chrono::steady_clock::now() < end; ++i) {
    // The camera moves on every call, so no pass's views can add up to the view already latched.
    ar_view_desc v = view_of(4, 4);
    v.camera.cam_to_world.m[3] = float(i) * 1e-3f;
    r.s->set_view(v);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  const auto log = r.s->present_log();
  r.s->close();
  std::vector<double> gaps;
  for (size_t i = 1; i < log.size(); ++i) {
    if (!log[i].interacting) continue;
    gaps.push_back(log[i].t_s - log[i - 1].t_s);
    CHECK(log[i].passes_resolved - log[i - 1].passes_resolved == 1);
    CHECK(log[i].epoch > log[i - 1].epoch);
    CHECK(gaps.back() >= 1.0 / 60.0 - kEps);
  }
  REQUIRE(gaps.size() >= 6);
  std::sort(gaps.begin(), gaps.end());
  CHECK(gaps[gaps.size() / 2] < kSlowPassMs * 1e-3 + kInteractionPublishGapS);
}

namespace {
// The CPU device with its resolve gated (launch `hold_at` of FILM_RESOLVE waits until released) and
// its present slowed by g_present_ms, as a 1080p present and ring download are on the GPU.
LaunchControl g_resolve;
uint32_t g_present_ms = 0;
struct GatedDevice : CpuDevice {
  using CpuDevice::CpuDevice;
  using CpuDevice::launch;
  void launch(KernelId kernel, uint32_t count) override {
    if (kernel == KernelId::FILM_RESOLVE) g_resolve.on_launch();
    if (kernel == KernelId::FILM_PRESENT)
      for (const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(g_present_ms);
           std::chrono::steady_clock::now() < until;) {}
    CpuDevice::launch(kernel, count);
  }
};
struct GatedRig {
  Ledger ledger;
  ThreadPool pool;
  GatedDevice dev;
  std::unique_ptr<Session> s;  // last: destroyed first
  // `work` 0: no camera work, so every epoch converges after its first pass (done every pass).
  GatedRig(ContextState& ctx, Scene& scene, uint64_t work = 1024) : pool(ctx, 2), dev(ledger, pool) {
    dev.register_production_stages();
    dev.register_kernel(KernelId::GENERATE_CAMERA, &gen_terminate);
    SessionOptions o;
    o.max_width = o.max_height = 8;
    o.test_camera_samples_per_pass = work;
    o.test_record_presents = true;
    s = std::make_unique<Session>(scene, dev, o);
  }
};

#ifdef _WIN32
// Blender asks Windows for a 1 ms timer, which the session's publish wait inherits inside Blender;
// a test process gets the 15.6 ms default unless it asks too, and every capped wait would then
// round up to a tick (gap ~31 ms, a cancel seen at +15.6 ms). Windows 11 can also ignore the
// request of a process it deems hidden; a harness process without a window may be one (a
// hypothesis: never reproduced on a real process, the cure shown by emulating the state, which
// the cap case does), so the process opts out of that throttling and the constructor then checks
// the request took effect: a refused request fails here by name instead of as a misleading pacing
// failure. Scoped to the cases that time the cap (winmm, test side only).
// The throttling state is per process and not reference-counted: holders (MsTimer, IgnoredTimerResolution)
// must be properly nested, destroyed in reverse order of construction. Lifetimes that overlap without
// nesting are unsafe, since each destructor restores the state its own constructor found.
// Ref: Microsoft Learn, timeBeginPeriod: "Starting with Windows 10, version 2004, this function no
// longer affects global timer resolution. For processes which have not called this function, Windows
// does not guarantee a higher resolution than the default system resolution. [...] Starting with
// Windows 11, if a window-owning process becomes fully occluded, minimized, or otherwise invisible
// or inaudible to the end user, Windows does not guarantee a higher resolution than the default
// system resolution. See SetProcessInformation for more information on this behavior."
// Ref: processthreadsapi.h, SetProcessInformation / GetProcessInformation with
// ProcessPowerThrottling and PROCESS_POWER_THROTTLING_STATE: the bit in ControlMask takes the
// policy over from the system and the bit in StateMask turns it on; both set to
// PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, the process's timer requests are ignored; set in
// ControlMask with StateMask 0, they are honoured.
#pragma comment(lib, "winmm.lib")
inline PROCESS_POWER_THROTTLING_STATE get_throttling() {
  PROCESS_POWER_THROTTLING_STATE s{};
  s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  const bool ok = GetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof(s)) != 0;
  const DWORD err = GetLastError();
  REQUIRE_MESSAGE(ok, "GetProcessInformation(ProcessPowerThrottling) failed, error " << err);
  return s;
}
inline bool set_throttling(PROCESS_POWER_THROTTLING_STATE s) {
  return SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof(s)) != 0;
}
// Puts the process into the state that makes Windows ignore timer requests, to exercise MsTimer's
// opt-out; restores what it found.
struct IgnoredTimerResolution {
  PROCESS_POWER_THROTTLING_STATE prior = get_throttling();
  IgnoredTimerResolution() {
    PROCESS_POWER_THROTTLING_STATE s = prior;
    s.ControlMask |= PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    s.StateMask |= PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    const bool ok = set_throttling(s);
    const DWORD err = GetLastError();
    REQUIRE_MESSAGE(ok, "cannot emulate a throttled process, error " << err);
  }
  ~IgnoredTimerResolution() {
    if (!set_throttling(prior)) {
      const DWORD err = GetLastError();
      WARN_MESSAGE(false, "IgnoredTimerResolution could not restore the power-throttling state, error " << err);
    }
  }
  IgnoredTimerResolution(const IgnoredTimerResolution&) = delete;
  IgnoredTimerResolution& operator=(const IgnoredTimerResolution&) = delete;
};
struct MsTimer {
  PROCESS_POWER_THROTTLING_STATE prior = get_throttling();
  MsTimer() {
    const MMRESULT mm = timeBeginPeriod(1);
    if (mm != TIMERR_NOERROR) FAIL("timeBeginPeriod(1) refused, MMRESULT " << mm);
    PROCESS_POWER_THROTTLING_STATE s = prior;
    s.ControlMask |= PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    s.StateMask &= ~ULONG(PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION);
    if (!set_throttling(s)) {
      const DWORD err = GetLastError();
      timeEndPeriod(1);
      FAIL("SetProcessInformation(ProcessPowerThrottling) refused, error " << err);
    }
    std::array<double, 9> ms{};
    for (double& m : ms) {
      const auto a = std::chrono::steady_clock::now();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      m = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
    }
    std::sort(ms.begin(), ms.end());
    if (ms[4] > 4.0) {
      release();
      FAIL("1 ms sleeps take " << ms[4] << " ms (median of 9, limit 4): the host timer resolution was "
           "not granted, or the test thread is starved by load");
    }
  }
  ~MsTimer() { release(); }
  MsTimer(const MsTimer&) = delete;
  MsTimer& operator=(const MsTimer&) = delete;
 private:
  void release() {
    if (!set_throttling(prior)) {
      const DWORD err = GetLastError();
      WARN_MESSAGE(false, "MsTimer could not restore the process's power-throttling state, error " << err);
    }
    if (timeEndPeriod(1) != TIMERR_NOERROR) WARN_MESSAGE(false, "timeEndPeriod(1) refused");
  }
};
#else
struct IgnoredTimerResolution {};
struct MsTimer {};
#endif

// The clock Session::now_s and the present record use.
double steady_s() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// A view that differs from every earlier one: the camera moves by `i`.
ar_view_desc moved_view(uint32_t i) {
  ar_view_desc v = view_of(4, 4);
  v.camera.cam_to_world.m[3] = float(i) * 1e-3f;
  return v;
}
}  // namespace

TEST_CASE("batching/session the publish cap holds every due frame during interaction") {
  // The process starts in the state Windows ignores timer requests in; MsTimer must opt out of it.
  const IgnoredTimerResolution throttled;
  const MsTimer timer;
  // An orbit (a view about every millisecond) where every epoch converges after its first, empty
  // pass, so every frame is due as converged: the cap still holds each one (0009 B-34), so no two
  // frames are closer than 1/60 s. The present takes 6 ms; the gap runs from present start to
  // present start, so the mean interval stays under gap + present (what a gap counted from the end
  // of the present gives at least). The rest above the gap is the timer's wake-up.
  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  g_launch.reset();
  g_resolve.reset();
  constexpr uint32_t kPresentMs = 6;
  g_present_ms = kPresentMs;
  GatedRig r(ctx, scene, 0);
  r.s->set_view(moved_view(0));
  r.s->start();
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
  for (uint32_t i = 1; std::chrono::steady_clock::now() < end; ++i) {
    r.s->set_view(moved_view(i));
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const auto log = r.s->present_log();
  r.s->close();
  g_present_ms = 0;
  size_t n = 0;
  double first_t = 0.0, last_t = 0.0;
  for (size_t i = 1; i < log.size(); ++i) {
    if (!log[i].interacting) continue;
    CHECK(log[i].t_s - log[i - 1].t_s >= 1.0 / 60.0 - kEps);
    if (n++ == 0) first_t = log[i - 1].t_s;
    last_t = log[i].t_s;
  }
  REQUIRE(n >= 10);
  CHECK((last_t - first_t) / double(n) < kInteractionPublishGapS + kPresentMs * 1e-3);
}

TEST_CASE("batching/session a cancel during the publish wait parks at once") {
  const MsTimer timer;
  // 2 ms passes under a 1/60 s cap: the render thread spends most of each cycle holding a finished
  // frame. A cancel issued then ends the hold instead of waiting out the gap (0007 d27).
  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  g_launch.reset();
  g_resolve.reset();
  g_launch.sleep_ms = 2;
  GatedRig r(ctx, scene);
  r.s->set_view(moved_view(0));
  r.s->start();
  for (uint32_t i = 1; i < 6; ++i) r.s->set_view(moved_view(i));  // keep the session interacting
  REQUIRE(until([&] { return r.s->present_log().size() >= 3; }));
  // Just after a present: the next 2 ms pass runs, then its frame holds for the rest of the gap.
  const size_t seen = r.s->present_log().size();
  REQUIRE(until([&] { return r.s->present_log().size() > seen; }));
  const double presented = r.s->present_log().back().t_s;
  while (steady_s() < presented + 0.006) {}
  const double cancelled = steady_s();
  r.s->cancel();
  CHECK(r.s->wait(1000));
  const double parked = steady_s();
  const uint32_t st = r.s->progress().state;
  r.s->close();
  CHECK(st == AR_STATE_PAUSED);
  INFO("cancel at +" << (cancelled - presented) * 1e3 << " ms, parked at +" << (parked - presented) * 1e3 << " ms");
  // The hold had until presented + 1/60 s to run; ended by the cancel, it is over well before.
  CHECK(parked < presented + 1.0 / 60.0 - 0.004);
  CHECK(parked - cancelled < 0.004);
}

TEST_CASE("batching/session a view change during the resolve presents no frame of the old view") {
  // On the GPU the resolve waits out most of the pass after its last launch boundary. A view set
  // then, with the latched view already presented, interrupts there as at any boundary (0007 d25):
  // pass 2 is held in its resolve while the view changes, so the next frame is the new view's,
  // never pass 2's frame of the old one.
  ContextState ctx;
  Scene scene(ctx, SceneConfig{1, {}});
  g_launch.reset();
  g_resolve.reset();
  g_resolve.hold_at = 2;
  GatedRig r(ctx, scene);
  r.s->set_view(view_of(4, 4));
  r.s->start();
  REQUIRE(until([&] { return g_resolve.count() >= 2; }));
  REQUIRE(r.s->present_log().size() == 1);  // pass 1's frame: the latched view has presented
  r.s->set_view(view_of(5, 5));
  g_resolve.hold(0);
  REQUIRE(until([&] { return r.s->present_log().size() >= 2; }));
  const auto log = r.s->present_log();
  const SessionStats st = r.s->stats();
  r.s->close();
  CHECK(log[0].epoch == 1);
  CHECK(log[1].epoch == 2);
  CHECK(log[1].passes_resolved == 2);  // pass 1 and the new view's first pass; pass 2 was interrupted
  CHECK(st.passes_interrupted >= 1);
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
