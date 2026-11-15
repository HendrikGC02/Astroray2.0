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
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../../../include/astroray/astroray.h"
#include "host/device.h"
#include "host/display/display_ring.h"
#include "host/counters.h"
#include "host/film/active_pixels.h"
#include "host/film/filter_table.h"
#include "host/film/fits_writer.h"
#include "host/film/film_layout.h"
#include "host/film/film_stats.h"
#include "host/film/response_table.h"
#include "host/report.h"
#include "host/session/readback.h"
#include "host/scheduler.h"
#include "host/scene/scene.h"
#include "host/session/kg_builder.h"
#include "host/session/latch.h"
#include "host/session/test_hooks.h"
#include "host/spectral/wavelength_table.h"
#include "kernel/film/present.h"

namespace astroray::host {

struct LightTable;  // host/light/light_builder.h

struct SessionOptions {
  uint32_t mode = AR_SESSION_MODE_VIEWPORT;
  uint32_t max_width = 0;
  uint32_t max_height = 0;
  uint32_t denoiser = AR_DENOISER_NONE;
  Owner owner{};
  // Test override, zero in production: the camera samples every pass hands the scheduler, in
  // place of the active-pixel count times k. Tests that need launches to stop at (cancel,
  // interrupt) set this with their own generate kernel, and it works with no view latched. Never
  // read from anywhere but the session that was given it.
  uint64_t test_camera_samples_per_pass = 0;
  // Test hook, zero in production: resolved pass number n (1-based, counted over the session) is
  // handed to the resolve tagged with the previous epoch, so the stale-write drop can be seen.
  uint64_t test_stale_resolve_at_pass = 0;
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
  // Session-owned, so they need no scene and stay valid after CLOSED.
  uint32_t samples_per_pixel = 0;  // in the current epoch
  float last_pass_ms = 0.0f;       // the last resolved pass: scheduler run plus resolve
};

// What a film read runs against, under the render lock: the device and film of the session
// and the provenance of a FITS dump of this very state.
struct FilmReadContext {
  Device& device;
  const Film& film;
  FitsProvenance provenance;
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

  // start and resume throw std::runtime_error naming the failure on a FAILED session.
  void start();
  void pause();   // stops before the next pass
  void resume();
  void cancel();  // stops at the next launch boundary; the interrupted pass is not resolved
  // Non-blocking: cancels; the render thread frees the session's device memory and the state
  // becomes CLOSED.
  void close();
  // Blocks until the state is CONVERGED, CLOSED or FAILED, the context is poisoned, or
  // `timeout_ms` passes; returns whether one of those was reached.
  bool wait(uint32_t timeout_ms);
  // Blocks until the render thread has exited (after close()).
  void join();

  // Record pending state for the next pass boundary. Both throw std::runtime_error naming the
  // failure on a FAILED session. set_view throws std::invalid_argument for a
  // region outside the session's maximum size or a non-finite or non-positive render scale, and
  // SessionUnsupported for a render scale other than 1 or an enabled border. set_display_mode
  // throws SessionUnsupported for a mode other than Beauty, Sample count and Bounce count.
  void set_view(const ar_view_desc& view);
  void set_display_mode(uint32_t mode);
  // A scene generation became active: wakes an idle session and raises the interrupt when due.
  // Registered as the scene's activation listener until CLOSED is published; after CLOSED the
  // session never touches the scene again, so the scene may be destroyed first (0007 d5).
  void scene_generation_activated();

  SessionProgress progress() const;
  // The pass list's layout: built at construction, so it is valid in INITIALIZING.
  const FilmLayout& layout() const { return layout_; }
  // The render sub-rectangle as of the last pass boundary (the maximum size before any).
  void render_size(uint32_t* width, uint32_t* height) const;
  // Runs `fn` with the film under the render lock, so it sees a state between two passes. It
  // may wait for the pass in flight to end (up to one pass): for the F12 job thread, never for
  // Blender's main thread (0007 d23). Throws std::invalid_argument when the session has no film
  // (still INITIALIZING, or CLOSED).
  void read_film(const std::function<void(const FilmReadContext&)>& fn) const;
  // The ring's newest frame after `after_frame`, or nullopt. Never waits for a pass: it takes
  // only the short ring pointer lock and the ring's own. Throws std::invalid_argument when the
  // session has no ring. The frame data stays valid until the session is destroyed, so a frame
  // acquired before close may still be read and released after CLOSED.
  std::optional<DisplayFrame> display_acquire(uint64_t after_frame);
  // Throws std::invalid_argument for a frame that is not acquired or a session without a ring.
  void display_release(uint64_t frame_id);
  // The counters of the last resolved pass (all zero before the first), copied at the pass end
  // because the scheduler's own are the render thread's.
  PassCounters pass_counters() const;
  // Snapshot of the session Report (FILM and SESSION entries) merged with the scene's report
  // (SCENE entries and GENERATION_REJECTED) in 0001 D2.11's order. Before the scene detaches it
  // is read live under mutex_; after, from the copy taken at detach, so it works after CLOSED.
  // report_entry reads the latest snapshot. Throws std::invalid_argument past the end.
  ar_report_info report_snapshot();
  void report_entry(uint32_t index, ar_report_entry* out) const;
  SessionStats stats() const;
  std::string failure() const;
  Report& report() { return report_; }
  const Latch& latch() const { return latch_; }
  // Tests: the sample count n of render-rectangle pixel j, read under the render lock.
  uint32_t film_samples(uint32_t j) const;
  // The ASTRORAY_TEST_* hooks this session read at create (empty outside gate builds).
  const std::vector<std::string>& active_test_hooks() const { return hooks_.active; }

 private:
  void run();
  void render_loop();
  void on_thread_exit();
  void detach_scene();
  void throw_if_failed_locked(const char* what) const;
  void allocate();
  void release();
  void begin_epoch();
  void apply_view();
  bool converged_locked() const;
  uint32_t sample_limit() const;
  void bind_globals();
  void latch_tables(uint64_t generation);
  ar::FilmData film_host_view(std::vector<uint32_t>* invalid) const;
  void update_camera_and_filter(uint64_t generation);
  void build_active();
  uint64_t pass_camera_samples() const;
  void resolve(uint64_t pass_epoch, uint64_t generation);
  void present(uint64_t generation);
  void wake();
  void raise_interrupt_if_due();
  void fail(const std::string& why);
  void set_state_locked(uint32_t state);
  FitsProvenance fits_provenance_locked() const;

  Scene& scene_;  // not touched once scene_detached_ is set
  ContextState& ctx_;  // the context outlives every scene and session on it
  Device& device_;
  SessionOptions options_;
  Report report_;
  Latch latch_;
  FilmLayout layout_;  // built once at construction from the film request

  // Render-thread objects. scheduler_ is created with the session (it allocates nothing until
  // its first pass with work) and freed by the render thread at exit; the API thread touches
  // only its two flags, under mutex_.
  std::unique_ptr<Scheduler> scheduler_;
  ResponseTable response_;
  WavelengthTable wavelengths_;
  std::unique_ptr<Film> film_;
  // The light table the latch asked for at this epoch (null for a scene without material and mesh
  // stores), and the scene table set the session renders, pinned until the next set (RULINGS 70).
  // The set holds the light table it was built with, and inv_radiance_scale = 1 / s follows the
  // set: after a failed upload the session keeps its previous set and that set's s.
  std::shared_ptr<const LightTable> epoch_lights_;
  std::shared_ptr<const SceneTableSet> tables_;
  SessionTables session_tables_;  // response and wavelength tables on the session's device
  bool tables_retry_ = false;     // the last upload failed: the next latch retries
  std::atomic<uint64_t> upload_failed_{0};  // generation whose upload failed (ar_progress)
  std::unique_ptr<DisplayRing> ring_;
  mutable std::mutex ring_mutex_;  // guards only the ring_ pointer (created, reset), never a pass
  std::string observer_hash_;      // hex, written with film_ under render_mutex_
  std::atomic<uint64_t> render_size_{0};  // width << 32 | height, written at apply_view
  InvalidSampleReporter invalid_;
  DeviceBuffer globals_buf_;  // KernelGlobals, then FilmData, then DisplayTarget
  DeviceBuffer present_buf_;
  // Per session, in the ledger: the active-pixel list, the CameraView of the latched view and the
  // pixel-filter table of the scene's settings. Camera and filter exist once a view and the
  // settings are latched; until both do a pass has no camera work.
  std::unique_ptr<ActivePixels> active_;
  DeviceBuffer camera_buf_, filter_buf_;
  bool camera_ready_ = false;
  bool filter_ready_ = false;
  FilterSettings filter_uploaded_{};
  TestHooks hooks_;  // read once in the constructor
  uint64_t film_epoch_ = 0;
  uint64_t passes_in_epoch_ = 0;
  uint32_t pass_index_ = 0;
  uint64_t resolves_ = 0;           // resolve calls over the session (the stale-resolve test hook)
  bool expect_epoch_ = false;       // the last pass was interrupted: the next latch forces an epoch

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
  bool scene_report_lost_ = false;   // reading the scene's report at detach failed
  bool scene_detached_ = false;      // set under mutex_ just before the listener is removed
  ReportTable detached_scene_report_;  // the scene's report as of detach
  ReportTable report_snap_;            // the latest merged snapshot
  float last_pass_ms_ = 0.0f;
  PassCounters last_counters_;
  SessionProgress detached_progress_;  // what progress() reports once the scene is detached
  std::atomic<uint64_t> listener_{0};  // the scene activation listener id, 0 once removed
  std::thread thread_;  // last: started after every member exists
};

// A CPU device with the production entry points registered, owning its ledger and a pool of
// that many engine threads (defined in src/device/cpu/cpu_device.cpp, the only place that may name
// the CPU device).
std::shared_ptr<Device> make_cpu_session_device(ContextState& ctx, unsigned workers);

}  // namespace astroray::host

namespace astroray {
// The context's device, from its fault state (defined in capi/context.cpp beside the context
// struct, like ar_rec_of_state): a session reaches its context only through its scene.
void context_device_of_state(ContextState& state, uint32_t* kind, uint32_t* index);
}  // namespace astroray

// The ABI handle. It owns the device (with its ledger and thread pool); `session` is declared last
// so it is destroyed first (it joins the render thread, which still uses the device).
struct ar_recorder;
struct ar_session_s {
  std::string last_error;
  ar_recorder* recorder = nullptr;  // cached at create: the scene may be destroyed after CLOSED
  bool closed = false;  // after ar_session_close only wait, progress and destroy are accepted
  std::shared_ptr<astroray::host::Device> device;
  std::unique_ptr<astroray::host::Session> session;
};
