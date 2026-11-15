// SPDX-License-Identifier: Apache-2.0
// A session: one render of one scene. Creation returns at once in INITIALIZING; the session's
// render thread allocates the film, the display buffers and the kernel globals, then runs passes
// once started: per pass the scheduler drains, film_resolve folds the per-sample buffer into the
// film, film_present (Beauty) or film_heatmap (a heat-map display mode) writes a frame, and the
// frame is published to the display ring with the latched generation. At every pass boundary
// the latch folds pending views and newly active commits into one latched state (latch.h).
// Cancellation is checked between launches. The session never touches process-global state;
// everything it owns dies with it.
// Ref: docs/decisions/0007-blender-integration.md d5 (session functions), d23 (one render thread
// per session), d25 (latch, epochs, interrupt rule), d27 (cancel at the next launch boundary);
// 0004 D25 (epoch resets); 0008 d3 (resolve), d12 (heat-map default ranges); RULINGS 52, 69.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include "../../../include/astroray/astroray.h"
#include "host/device.h"
#include "host/display/display_ring.h"
#include "host/film/film_layout.h"
#include "host/film/film_stats.h"
#include "host/film/response_table.h"
#include "host/report.h"
#include "host/scheduler.h"
#include "host/scene/scene.h"
#include "host/session/latch.h"
#include "host/spectral/wavelength_table.h"
#include "kernel/film/present.h"

namespace astroray::host {

struct SessionOptions {
  uint32_t mode = AR_SESSION_MODE_VIEWPORT;
  uint32_t max_width = 0;
  uint32_t max_height = 0;
  uint32_t denoiser = AR_DENOISER_NONE;
  Owner owner{};
  // Test hook, zero in production: the camera samples every pass hands the scheduler. Until the
  // active-pixel list exists a production pass has no work, so tests that need launches to stop
  // at (cancel, interrupt) set this with their own generate kernel. Never read from anywhere but
  // the session that was given it.
  uint64_t test_camera_samples_per_pass = 0;
};

// Input the session refuses because it does not support it yet (AR_ERR_UNSUPPORTED at the ABI).
class SessionUnsupported : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct SessionProgress {
  uint32_t state = AR_STATE_INITIALIZING;
  uint64_t active_generation = 0;
  uint64_t pending_generation = 0;
  uint64_t failed_generation = 0;
  uint64_t epoch = 0;
};

struct SessionStats {
  uint64_t passes_resolved = 0;
  uint64_t passes_interrupted = 0;  // stopped for a new epoch, never resolved
  uint64_t passes_cancelled = 0;
  uint64_t stale_dropped = 0;       // resolves of a pass whose epoch was no longer current
  uint64_t frames_published = 0;
  uint64_t samples_done = 0;        // per pixel, in the current epoch
};

class Session {
 public:
  // Starts the render thread through start_engine_thread and returns. Throws
  // std::invalid_argument for a zero size, an unknown mode or denoiser, and SessionUnsupported
  // for a denoiser this build cannot run. `scene` and `device` outlive the session.
  Session(Scene& scene, Device& device, const SessionOptions& options);
  ~Session();  // close() and join()
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  void start();
  void pause();   // stops before the next pass
  void resume();
  void cancel();  // stops at the next launch boundary; the interrupted pass is not resolved
  // Non-blocking: cancels; the render thread frees the session's device memory and the state
  // becomes CLOSED.
  void close();
  // Blocks until the state is CONVERGED, CLOSED or FAILED or `timeout_ms` passes; returns
  // whether one of those states was reached.
  bool wait(uint32_t timeout_ms);
  // Blocks until the render thread has exited (after close()).
  void join();

  // Record pending state for the next pass boundary. set_view throws std::invalid_argument for a
  // region outside the session's maximum size or a non-finite or non-positive render scale, and
  // SessionUnsupported for a render scale other than 1 or an enabled border. set_display_mode
  // throws SessionUnsupported for a mode other than Beauty, Sample count and Bounce count.
  void set_view(const ar_view_desc& view);
  void set_display_mode(uint32_t mode);
  // A scene generation became active: wakes an idle session and raises the interrupt when due.
  void scene_generation_activated();

  SessionProgress progress() const;
  SessionStats stats() const;
  std::string failure() const;
  Report& report() { return report_; }
  const Latch& latch() const { return latch_; }
  // Tests: the sample count n of render-rectangle pixel j, read under the render lock.
  uint32_t film_samples(uint32_t j) const;

 private:
  void run();
  void allocate();
  void release();
  void begin_epoch();
  void apply_view();
  bool converged_locked() const;
  uint32_t sample_limit() const;
  void bind_globals();
  void resolve(uint64_t pass_epoch, uint64_t generation);
  void present(uint64_t generation);
  void wake();
  void raise_interrupt_if_due();
  void fail(const std::string& why);
  void set_state_locked(uint32_t state);

  Scene& scene_;
  Device& device_;
  SessionOptions options_;
  Report report_;
  Latch latch_;

  // Render-thread objects. scheduler_ is created with the session (it allocates nothing until
  // its first pass with work) and freed by the render thread at exit; the API thread touches
  // only its two flags, under mutex_.
  std::unique_ptr<Scheduler> scheduler_;
  ResponseTable response_;
  WavelengthTable wavelengths_;
  std::unique_ptr<Film> film_;
  std::unique_ptr<DisplayRing> ring_;
  InvalidSampleReporter invalid_;
  DeviceBuffer globals_buf_;  // KernelGlobals, then FilmData, then DisplayTarget
  DeviceBuffer present_buf_;
  uint64_t film_epoch_ = 0;
  uint64_t passes_in_epoch_ = 0;
  uint32_t pass_index_ = 0;

  mutable std::mutex render_mutex_;  // held by the render thread for a pass and its present
  mutable std::mutex mutex_;         // state below
  std::condition_variable wake_cv_;   // the render thread waits here
  std::condition_variable state_cv_;  // wait() waits here
  uint32_t state_ = AR_STATE_INITIALIZING;
  bool started_ = false;
  bool paused_ = false;
  bool idle_ = false;
  bool closing_ = false;
  bool exited_ = false;
  bool failed_ = false;
  std::string failure_;
  SessionStats stats_;
  std::thread thread_;  // last: started after every member exists
};

}  // namespace astroray::host

namespace astroray {
// The context's device, from its fault state (defined in capi/context.cpp beside the context
// struct, like ar_rec_of_state): a session reaches its context only through its scene.
void context_device_of_state(ContextState& state, uint32_t* kind, uint32_t* index);
}  // namespace astroray

struct ar_scene_s;

// The ABI handle. The device and what it needs (ledger, thread pool) are owned here, behind a
// type-erased pointer so this header names no device implementation; `session` is declared last
// so it is destroyed first (it joins the render thread, which still uses the device).
struct ar_session_s {
  std::string last_error;
  ar_scene_s* scene = nullptr;
  bool closed = false;  // after ar_session_close only wait, progress and destroy are accepted
  std::shared_ptr<void> resources;
  std::unique_ptr<astroray::host::Session> session;
};
