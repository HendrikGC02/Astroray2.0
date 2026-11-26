// SPDX-License-Identifier: Apache-2.0
// A session: one render of one scene. Creation returns at once in INITIALIZING; the session's
// render thread allocates the film, the display buffers and the kernel globals, then runs passes
// once started: per pass the scheduler drains, film_resolve folds the per-sample buffer into the
// film, film_present (Beauty) or film_heatmap (a heat-map display mode) writes a frame, and the
// frame is published to the display ring with the latched generation. At every pass boundary
// the latch folds pending views and newly active commits into one latched state (latch.h).
// Cancellation is checked between launches. With a denoiser, the state loads on a worker from
// creation, and a converged final render runs film_prepare_denoise and one FINAL denoise before it
// reports CONVERGED; a denoiser that fails leaves the noisy image and one report entry. The session
// never touches process-global state; everything it owns dies with it.
// Ref: docs/decisions/0007-blender-integration.md d5 (session functions), d23 (one render thread
// per session), d25 (latch, epochs, interrupt rule), d27 (cancel at the next launch boundary);
// 0004 D25 (epoch resets); 0008 d3 (resolve), d12 (heat-map default ranges), d16-d18 (the final
// denoise, its failure rule, FINAL settings), d13a (Combined denoised, Noisy Image the film's);
// RULINGS 52, 69, 87 (the denoise order written into 0008 d16).
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
#include "host/denoise/denoiser.h"
#include "host/film/display_image.h"
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
#include "host/session/batching.h"
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
  // Test hook, false in production: keep one PresentRecord per published frame (present_log).
  bool test_record_presents = false;
  // Test hook, zero in production: the render thread sleeps this long after each resolve and before
  // the pass is published, so a poll can look for a sample count that arrives without its pass time.
  uint32_t test_sleep_after_resolve_ms = 0;
  // Test hook, false in production: at convergence the render thread treats the denoiser state as
  // still loading, so a cancel can be issued during the final render's wait for it. It holds the
  // session's wait, not DenoiserState's own loading worker.
  bool test_hold_denoiser = false;
  // Test hook, empty in production: called on the render thread right after the final denoise's OIDN
  // call returned and before its result is kept, so a test can cancel "during" the execute step.
  std::function<void()> test_after_denoise;
  // Test hook, zero in production: the pixels the denoise target is taken to hold, in place of the
  // maximum size (the buffer itself is always allocated at the maximum size), so the size check fires.
  uint64_t test_denoise_capacity_px = 0;
};

// One published frame as the render thread saw it: when (the steady clock the batching uses), how
// many passes had been resolved over the session, the epoch, whether interaction was on, and the
// passes per update the schedule asked for at that moment.
struct PresentRecord {
  double t_s = 0.0;
  uint64_t passes_resolved = 0;
  uint64_t epoch = 0;
  bool interacting = false;
  uint32_t target = 0;
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
  // The epoch the film is accumulating, published with the reset of samples_per_pixel: `epoch` moves at
  // the latch, this one only when the film has been cleared, so (film_epoch == epoch, samples_per_pixel)
  // counts passes of the current epoch alone.
  uint64_t film_epoch = 0;
  // Session-owned, so they need no scene and stay valid after CLOSED.
  uint32_t samples_per_pixel = 0;  // in the current epoch
  float last_pass_ms = 0.0f;       // the last resolved pass: scheduler run plus resolve
  // Device::memory_info as the render thread read it when it published the last pass (0 before the
  // first pass and on the CPU device), so a progress poll never calls into the device.
  uint64_t device_free_bytes = 0;
  uint64_t device_total_bytes = 0;
};

// What a film read runs against, under the render lock: the device and film of the session
// and the provenance of a FITS dump of this very state.
struct FilmReadContext {
  Device& device;
  const Film& film;
  FitsProvenance provenance;
  uint64_t pass_revision = 0;  // of film's layout (ruling 75)
  bool denoiser = false;  // the session selected a denoiser: list_passes(film.layout(), denoiser)
  // The final denoise's DisplayImage when it ran and succeeded for this film state, else null.
  const DisplayImage* denoised = nullptr;
};

struct SessionStats {
  uint64_t passes_resolved = 0;
  uint64_t passes_interrupted = 0;  // stopped for a new epoch, never resolved
  uint64_t passes_cancelled = 0;
  uint64_t stale_dropped = 0;       // resolves of a pass whose epoch was no longer current
  uint64_t frames_published = 0;
  uint64_t samples_done = 0;        // per pixel, in the current epoch
  uint64_t denoises = 0;            // final denoises that succeeded
  // The last final denoise: film_prepare_denoise and the copy of its three planes to the host (the
  // OIDN backend takes host images), then the OIDN filter itself.
  double denoise_copy_ms = 0.0;
  double denoise_ms = 0.0;
};

class Session {
 public:
  // Starts the render thread through start_engine_thread and, with AR_DENOISER_OIDN, the denoiser
  // state's loading worker (0008 d16), and returns. Throws std::invalid_argument for a zero size, an
  // unknown mode or denoiser, and SessionUnsupported for DLSS_RR and for OIDN in a VIEWPORT session.
  // `scene` and `device` outlive the session.
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
  // Blocks until the state is PAUSED, CONVERGED, CLOSED or FAILED, the context is poisoned, or
  // `timeout_ms` passes; returns whether one of those was reached. A session with a denoiser stays
  // RENDERING until its final denoise ended (0008 d16, ruling 87).
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
  // A scene generation became active or was dropped: wakes an idle session (or a final session
  // waiting for its start generation) and raises the interrupt when due.
  // Registered as the scene's activation listener until CLOSED is published; after CLOSED the
  // session never touches the scene again, so the scene may be destroyed first (0007 d5).
  void scene_generation_activated();

  SessionProgress progress() const;
  // The pass list's layout, a copy: built at construction from the settings committed by then, so
  // it is valid in INITIALIZING, and replaced when the render thread rebuilds the film for a
  // changed pass request (0008 d13a: readback enumerates what the layout allocated). `revision`
  // receives the pass-list revision of that layout (ruling 75).
  FilmLayout layout(uint64_t* revision = nullptr) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (revision) *revision = pass_revision_;
    return layout_;
  }
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
  // Tests: the frames published so far (empty unless SessionOptions::test_record_presents).
  std::vector<PresentRecord> present_log() const;
  std::string failure() const;
  Report& report() { return report_; }
  const Latch& latch() const { return latch_; }
  // Tests: the sample count n of render-rectangle pixel j, read under the render lock.
  uint32_t film_samples(uint32_t j) const;
  bool denoiser_selected() const { return options_.denoiser != AR_DENOISER_NONE; }
  // A final denoise succeeded for the current film state (AR_PASS_DENOISED on Combined).
  bool denoised() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return denoised_;
  }
  // Tests: the denoiser's error text once it failed (what its DENOISER_UNAVAILABLE entry holds,
  // before the report's 127-byte cut), else empty.
  std::string denoiser_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return denoiser_error_;
  }
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
  // Builds the film from the scene's settings when they changed since the film was built and the
  // pass request differs, and reports requested passes this build cannot produce.
  void sync_film();
  void begin_epoch();
  void apply_view();
  bool converged_locked() const;
  // A final session whose start generation is neither active nor dropped yet (0007 d25).
  bool awaiting_start_generation_locked();
  uint32_t sample_limit() const;
  void bind_globals();
  // Pins the set of the scene's current snapshot; true when the pinned set changed.
  bool latch_tables();
  void update_camera_and_filter(uint64_t generation);
  void build_active();
  uint64_t pass_camera_samples() const;
  // False when the pass was dropped as stale; the caller publishes a resolved pass.
  bool resolve(uint64_t pass_epoch, uint64_t generation);
  void present(uint64_t generation);
  // The final denoise after convergence (0008 d16): waits for the state in slices, prepares the
  // three images, denoises with FINAL quality. True when it ended (denoised, or failed with its
  // report entry); false when a cancel, close, failure or newly pending state stopped it, which
  // leaves the noisy image.
  bool final_denoise(uint64_t generation);
  void denoiser_unavailable(const std::string& why, uint64_t generation);
  static double now_s();  // seconds on steady_clock, the one clock batching_ sees
  void wake();
  void raise_interrupt_if_due();
  void fail(const std::string& why, const DeviceError* device_error = nullptr);
  void set_state_locked(uint32_t state);
  void leave_paused_locked();  // start()/resume(): PAUSED becomes RENDERING at once
  FitsProvenance fits_provenance_locked() const;

  Scene& scene_;  // not touched once scene_detached_ is set
  ContextState& ctx_;  // the context outlives every scene and session on it
  Device& device_;
  SessionOptions options_;
  Report report_;
  Latch latch_;
  FilmLayout layout_;  // film_'s layout (from the settings at construction until the first film); under mutex_
  // Ruling 75: counts layouts whose channel list differs from the last published one; under mutex_.
  uint64_t pass_revision_ = 0;

  // Render-thread objects. scheduler_ is created with the session (it allocates nothing until
  // its first pass with work) and freed by the render thread at exit; the API thread touches
  // only its two flags, under mutex_.
  std::unique_ptr<Scheduler> scheduler_;
  ResponseTable response_;
  WavelengthTable wavelengths_;
  std::unique_ptr<Film> film_;
  std::shared_ptr<const StoredSettings> film_settings_;  // the settings film_ was last checked against
  bool stats_display_warned_ = false;  // the bounce display met a film without statistics (render thread)
  // The scene table set the session renders, pinned until the next set (RULINGS 70). It holds the
  // light table of its snapshot (null for a scene without material and mesh stores), and
  // inv_radiance_scale = 1 / s follows it: after a failed upload the session keeps its previous
  // set and that set's s.
  std::shared_ptr<const SceneTableSet> tables_;
  SessionTables session_tables_;  // response and wavelength tables on the session's device
  bool tables_retry_ = false;     // the last upload failed: the next latch retries
  std::atomic<uint64_t> upload_failed_{0};  // generation whose upload failed (ar_progress)
  uint64_t upload_failed_reported_ = 0;     // the last one with an entry and a log line
  std::unique_ptr<DisplayRing> ring_;
  mutable std::mutex ring_mutex_;  // guards only the ring_ pointer (created, reset), never a pass
  std::string observer_hash_;      // hex, written with film_ under render_mutex_
  std::atomic<uint64_t> render_size_{0};  // width << 32 | height, written at apply_view
  InvalidSampleReporter invalid_;
  DeviceBuffer globals_buf_;  // KernelGlobals, then FilmData, then DisplayTarget, then the invalid total
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
  // The denoiser (null without one): created at construction, used and destroyed by the render
  // thread (release(), before the device buffers; 176's rule that it goes before the CUDA device).
  std::unique_ptr<DenoiserState> denoiser_;
  // film_prepare_denoise's target: colour, albedo and normal planes at the maximum size, then the
  // DenoiseTarget the kernel reads (KernelGlobals::denoise). Allocated at the first final denoise.
  DeviceBuffer denoise_buf_;
  bool denoise_bind_ = false;   // bind_globals points KernelGlobals::denoise at denoise_buf_
  uint64_t denoise_capacity_px_ = 0;  // pixels each plane of denoise_buf_ holds
  bool denoise_done_ = false;   // the final denoise of this epoch ended (render thread)
  bool denoiser_reported_ = false;  // its DENOISER_UNAVAILABLE entry exists (once per session)
  std::vector<float> denoise_host_[3];  // the planes on the host, the backend's input
  DisplayImage display_;  // the final denoise's output; under render_mutex_
  uint64_t film_epoch_ = 0;
  uint64_t published_film_epoch_ = 0;  // guarded by mutex_
  uint64_t passes_in_epoch_ = 0;
  uint32_t pass_index_ = 0;
  uint64_t resolves_ = 0;           // resolve calls over the session (the stale-resolve test hook)
  Batching batching_;               // interaction and passes per displayed update (0007 d49, d50)
  uint32_t unpresented_ = 0;        // resolved passes since the last published frame
  double last_present_s_ = -1e9;    // now_s() of the last present
  std::vector<PresentRecord> present_log_;  // under mutex_
  bool expect_epoch_ = false;       // the last pass was interrupted: the next latch forces an epoch

  mutable std::mutex render_mutex_;  // held by the render thread for a pass and its present
  mutable std::mutex mutex_;         // state below
  std::condition_variable wake_cv_;   // the render thread waits here
  std::condition_variable state_cv_;  // wait() waits here
  uint32_t state_ = AR_STATE_INITIALIZING;
  bool started_ = false;
  bool paused_ = false;
  bool idle_ = false;
  // FINAL: the scene's last committed generation when start() ran, 0 once it resolved. The session
  // latches nothing before it resolves, so it never converges on an older scene (0007 d25).
  uint64_t start_generation_ = 0;
  bool closing_ = false;
  bool exited_ = false;
  bool failed_ = false;
  bool failure_device_ = false;  // the failure was a DeviceError: calls then throw one (AR_ERR_DEVICE)
  bool failure_sticky_ = false;
  std::string failure_;
  SessionStats stats_;
  bool scene_report_lost_ = false;   // reading the scene's report at detach failed
  bool scene_detached_ = false;      // set under mutex_ just before the listener is removed
  ReportTable detached_scene_report_;  // the scene's report as of detach
  ReportTable report_snap_;            // the latest merged snapshot
  float last_pass_ms_ = 0.0f;
  bool denoised_ = false;       // display_ holds a denoised image for the current film state
  std::string denoiser_error_;  // the denoiser's failure text
  uint64_t device_free_bytes_ = 0, device_total_bytes_ = 0;  // published with last_pass_ms_
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
  // The pass-list revision ar_session_pass_count last reported; pass_info and read_pass refuse
  // another (ruling 75).
  std::atomic<uint64_t> pass_revision{0};
  std::shared_ptr<astroray::host::Device> device;
  // The scene's device ledger (null on the CPU device), held so progress still reads it after the
  // scene is destroyed (its records are gone then).
  std::shared_ptr<astroray::host::Ledger> scene_ledger;
  std::unique_ptr<astroray::host::Session> session;
};
