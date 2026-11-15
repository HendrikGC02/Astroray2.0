// SPDX-License-Identifier: Apache-2.0
// Ref: docs/decisions/0007-blender-integration.md d5, d23, d25, d27, d29; 0004 D25; 0008 d3, d12;
// RULINGS 52 (cancel between launches), 69 (generation numbers of latched views).
#include "host/session/session.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <utility>

#include "host/data_resource.h"
#include "host/thread.h"
#include "kernel/sample/sobol.h"

namespace astroray::host {
namespace {

constexpr uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }
constexpr uint64_t kFilmDataAt = align_up(sizeof(ar::KernelGlobals), 256);
constexpr uint64_t kDisplayAt = kFilmDataAt + align_up(sizeof(ar::FilmData), 256);
constexpr uint64_t kGlobalsBytes = kDisplayAt + sizeof(ar::DisplayTarget);
// One pass is one sample per active pixel (0004 D25); the adaptive tail is not built.
constexpr uint32_t kSamplesPerPass = 1;
// The slice wait() sleeps between checks of the poisoned flag, which no notification announces:
// the period Scene::wait_init uses.
constexpr auto kWaitSlice = std::chrono::milliseconds(50);

bool terminal(uint32_t state) {
  return state == AR_STATE_CONVERGED || state == AR_STATE_CLOSED || state == AR_STATE_FAILED;
}

}  // namespace

Session::Session(Scene& scene, Device& device, const SessionOptions& options)
    : scene_(scene), ctx_(scene.ctx()), device_(device), options_(options) {
  if (options.max_width == 0 || options.max_height == 0)
    throw std::invalid_argument("session: max_width and max_height must be non-zero");
  if (options.mode > AR_SESSION_MODE_PREVIEW) throw std::invalid_argument("session: unknown mode");
  if (options.denoiser > AR_DENOISER_DLSS_RR) throw std::invalid_argument("session: unknown denoiser");
  if (options.denoiser != AR_DENOISER_NONE)
    throw SessionUnsupported("session: denoisers are not supported in this build yet");
  SchedulerConfig cfg;
  cfg.report = &report_;
  cfg.owner = options.owner;
  scheduler_ = std::make_unique<Scheduler>(device_, cfg);
  // Registered before the thread starts, so a commit activated meanwhile is never missed.
  listener_ = scene_.generations().add_listener([this] { scene_generation_activated(); });
  try {
    // The inner guard catches a structured exception in the render loop so the exit path (FAILED
    // or CLOSED, listener removal) still runs; the outer one is the rule that every engine thread
    // starts through start_engine_thread. The context is poisoned either way (0001 D2.5).
    thread_ = start_engine_thread(ctx_, [this] {
      if (!run_engine_thread(ctx_, [this] { run(); })) on_thread_exit();
    });
  } catch (...) {
    scene_.generations().remove_listener(listener_.exchange(0));
    throw;
  }
}

Session::~Session() {
  // First, so no activation callback can reach a session being destroyed. After CLOSED the
  // listener is already gone and the scene is not touched again (it may be destroyed by now).
  if (const uint64_t id = listener_.exchange(0)) scene_.generations().remove_listener(id);
  close();
  join();
}

void Session::join() {
  if (thread_.joinable()) thread_.join();
}

void Session::set_state_locked(uint32_t state) {
  state_ = state;
  state_cv_.notify_all();
}

void Session::throw_if_failed_locked(const char* what) const {
  if (failed_) throw std::runtime_error(std::string(what) + ": the session failed: " + failure_);
}

void Session::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (closing_) return;
  throw_if_failed_locked("start");
  started_ = true;
  paused_ = false;
  idle_ = false;
  if (scheduler_) scheduler_->cancel = false;
  wake_cv_.notify_all();
}

void Session::pause() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (closing_) return;
  paused_ = true;
  wake_cv_.notify_all();
}

void Session::resume() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (closing_) return;
  throw_if_failed_locked("resume");
  paused_ = false;
  idle_ = false;
  if (scheduler_) scheduler_->cancel = false;
  wake_cv_.notify_all();
}

void Session::cancel() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (closing_) return;
  paused_ = true;
  if (scheduler_) scheduler_->cancel = true;
  wake_cv_.notify_all();
}

void Session::close() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closing_) return;
    closing_ = true;
    if (scheduler_) scheduler_->cancel = true;
    wake_cv_.notify_all();
    // A live render thread publishes CLOSED itself on its way out (on_thread_exit).
    if (!exited_) return;
  }
  // The thread already exited (failed, or died in a structured exception): close here.
  detach_scene();
  std::lock_guard<std::mutex> lock(mutex_);
  set_state_locked(AR_STATE_CLOSED);
}

void Session::detach_scene() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!scene_detached_) {
      detached_progress_.active_generation = latch_.generation();
      detached_progress_.pending_generation = scene_.generations().pending();
      detached_progress_.failed_generation = scene_.generations().failed();
      detached_progress_.epoch = latch_.epoch();
      scene_detached_ = true;
    }
  }
  // Outside mutex_: a listener call in flight holds the tracker's listener lock and may be
  // waiting for mutex_; remove_listener waits for it to finish.
  if (const uint64_t id = listener_.exchange(0)) scene_.generations().remove_listener(id);
}

void Session::on_thread_exit() {
  bool closing;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    exited_ = true;
    closing = closing_;
    if (!closing && !failed_) {
      failed_ = true;
      failure_ = ctx_.poisoned.load(std::memory_order_acquire) ? "the render thread faulted: " + ctx_.poison_message
                                                                : "the render thread ended unexpectedly";
      set_state_locked(AR_STATE_FAILED);
    }
    state_cv_.notify_all();
  }
  // Not closing: close() sees exited_ and finishes the job.
  if (!closing) return;
  detach_scene();
  std::lock_guard<std::mutex> lock(mutex_);
  set_state_locked(AR_STATE_CLOSED);
}

bool Session::wait(uint32_t timeout_ms) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  std::unique_lock<std::mutex> lock(mutex_);
  const auto pred = [this] { return terminal(state_) || ctx_.poisoned.load(std::memory_order_acquire); };
  // In slices: a poisoned context is a flag, not a notification.
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= end) return pred();
    const auto slice = std::min<std::chrono::steady_clock::duration>(end - now, kWaitSlice);
    if (state_cv_.wait_for(lock, slice, pred)) return true;
  }
}

void Session::set_view(const ar_view_desc& view) {
  if (view.region_width == 0 || view.region_height == 0 || view.region_width > options_.max_width ||
      view.region_height > options_.max_height)
    throw std::invalid_argument("set_view: region " + std::to_string(view.region_width) + "x" +
                                std::to_string(view.region_height) + " is empty or larger than the session's " +
                                std::to_string(options_.max_width) + "x" + std::to_string(options_.max_height));
  if (!(view.render_scale > 0.0f) || !std::isfinite(view.render_scale))
    throw std::invalid_argument("set_view: render_scale must be finite and positive");
  if (view.render_scale != 1.0f)
    throw SessionUnsupported("set_view: render scales other than 1 are not supported yet");
  if (view.border_enabled) throw SessionUnsupported("set_view: viewport borders are not supported yet");
  if (view.camera.motion_steps && !view.camera.motion_xf)
    throw std::invalid_argument("set_view: motion_steps is non-zero but motion_xf is null");
  {
    std::lock_guard<std::mutex> lock(mutex_);
    throw_if_failed_locked("set_view");
  }
  if (latch_.set_view(view)) wake();
  raise_interrupt_if_due();
}

void Session::set_display_mode(uint32_t mode) {
  if (mode != AR_DISPLAY_BEAUTY && mode != AR_DISPLAY_SAMPLE_COUNT && mode != AR_DISPLAY_BOUNCE_COUNT)
    throw SessionUnsupported("set_display_mode: display mode " + std::to_string(mode) + " is not supported yet");
  {
    std::lock_guard<std::mutex> lock(mutex_);
    throw_if_failed_locked("set_display_mode");
  }
  // Display-only: no epoch, so never an interrupt; applied at the next present.
  if (latch_.set_display_mode(mode)) wake();
}

void Session::scene_generation_activated() {
  // Runs on the thread that activated the generation: nothing may escape into it.
  try {
    if (latch_.pending(scene_.generations().active())) wake();
    raise_interrupt_if_due();
  } catch (const std::exception& e) {
    fail(std::string("handling a scene generation: ") + e.what());
  } catch (...) {
    fail("handling a scene generation: unknown exception");
  }
}

void Session::raise_interrupt_if_due() {
  // Check and set under one mutex_ hold, the one the render thread re-evaluates the flag under
  // right after each latch: a stale "due" can then never outlive the latch that consumed it and
  // interrupt the next epoch's first pass. Lock order: mutex_, then the tracker, then the latch.
  std::lock_guard<std::mutex> lock(mutex_);
  if (!scheduler_ || scene_detached_) return;
  if (latch_.interrupt_due(scene_.generations().active())) scheduler_->epoch_pending = true;
}

void Session::wake() {
  std::lock_guard<std::mutex> lock(mutex_);
  idle_ = false;
  // The caller's next wait() must not see the CONVERGED of the state it just changed.
  if (state_ == AR_STATE_CONVERGED && started_ && !paused_ && !closing_ && !failed_)
    set_state_locked(AR_STATE_RENDERING);
  wake_cv_.notify_all();
}

SessionProgress Session::progress() const {
  // Scene reads happen under mutex_, so they finish before detach_scene lets the scene go.
  std::lock_guard<std::mutex> lock(mutex_);
  SessionProgress p;
  if (scene_detached_) {
    p = detached_progress_;
  } else {
    p.active_generation = latch_.generation();
    p.pending_generation = scene_.generations().pending();
    p.failed_generation = scene_.generations().failed();
    p.epoch = latch_.epoch();
  }
  p.state = state_;
  if (p.state != AR_STATE_CLOSED && ctx_.poisoned.load(std::memory_order_acquire)) p.state = AR_STATE_FAILED;
  return p;
}

SessionStats Session::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

std::string Session::failure() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return failure_;
}

uint32_t Session::film_samples(uint32_t j) const {
  std::lock_guard<std::mutex> render(render_mutex_);
  if (!film_) throw std::logic_error("film_samples: the session has no film");
  const ar::FilmData& f = film_->data();
  if (j >= f.rect_width * f.rect_height) throw std::out_of_range("film_samples: pixel outside the render rectangle");
  return f.n[ar::film_active_pixel(f, j)];
}

void Session::fail(const std::string& why) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (failed_) return;  // the first failure is the one reported
    failed_ = true;
    failure_ = why;
    // Stop the pass in flight; the render loop parks until close.
    if (scheduler_) scheduler_->cancel = true;
    set_state_locked(AR_STATE_FAILED);
    wake_cv_.notify_all();
  }
  try {
    ctx_.log.line("ERROR", "session failed: " + why);
    const double value[2] = {0.0, 0.0};
    report_.raise(AR_REPORT_SESSION_FAILED, 0, why, 1, value, latch_.generation());
  } catch (...) {
    // Out of memory while reporting: the state and the stored text still say it.
  }
}

void Session::allocate() {
  DataBlob blob;
  if (!find_data("spectral/observer.bin", &blob))
    throw std::runtime_error("session: spectral/observer.bin is not embedded in this module");
  ObserverTables obs;
  const std::string err = parse_observer(blob.data, blob.size, &obs);
  if (!err.empty()) throw std::runtime_error("session: " + err);
  response_ = make_response_table(obs);
  wavelengths_ = WavelengthTableBuilder().build();

  FilmRequest req;
  req.max_width = options_.max_width;
  req.max_height = options_.max_height;
  req.statistics = true;  // 0008 d12: on by default; the bounce heat map reads it
  film_ = std::make_unique<Film>(device_, FilmLayout::build(req), options_.owner);
  // The tables are host memory: right for the CPU device, the only one sessions run on yet.
  film_->data().response = response_view(response_);
  film_->data().wavelengths = {wavelengths_.edges.data(), wavelengths_.cdf.data(), wavelengths_.bins()};

  const uint64_t frame_bytes = uint64_t(options_.max_width) * options_.max_height * 8u;  // RGBA16F
  globals_buf_ = device_.allocate(LedgerCategory::TABLES, kGlobalsBytes, options_.owner);
  present_buf_ = device_.allocate(LedgerCategory::DISPLAY_FRAME, frame_bytes, options_.owner);
  ring_ = std::make_unique<DisplayRing>(frame_bytes);
}

void Session::release() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    scheduler_.reset();  // frees the pools
  }
  std::lock_guard<std::mutex> render(render_mutex_);
  ring_.reset();
  film_.reset();
  device_.free(present_buf_);
  device_.free(globals_buf_);
}

uint32_t Session::sample_limit() const {
  const auto s = scene_.settings();
  if (!s) return 0;
  return options_.mode == AR_SESSION_MODE_VIEWPORT ? s->s.viewport_samples : s->s.samples;
}

bool Session::converged_locked() const {
  // No work: one empty pass shows the latched state, more would only spin.
  if (options_.test_camera_samples_per_pass == 0 && passes_in_epoch_ > 0) return true;
  const uint32_t limit = sample_limit();
  return limit > 0 && stats_.samples_done >= limit;
}

void Session::begin_epoch() {
  // 0004 D25 and 0007 d25: film (per-sample buffer, splat and direct channels included), statistics
  // and sample indices restart; FILM report entries go with the film.
  film_->clear();
  invalid_.on_film_reset(report_);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.samples_done = 0;
  }
  pass_index_ = 0;
  passes_in_epoch_ = 0;
  film_epoch_ = latch_.epoch();
}

void Session::apply_view() {
  uint32_t w = options_.max_width, h = options_.max_height;
  if (latch_.has_view()) {
    const StoredView v = latch_.view();
    w = v.v.region_width;
    h = v.v.region_height;
  }
  film_->set_render_size(w, h);
}

void Session::bind_globals() {
  ar::KernelGlobals kg{};
  char* base = static_cast<char*>(globals_buf_.ptr);
  kg.film = reinterpret_cast<ar::FilmData*>(base + kFilmDataAt);
  kg.display = reinterpret_cast<ar::DisplayTarget*>(base + kDisplayAt);
  const ar::FilmData& f = film_->data();
  kg.session.width = f.rect_width;
  kg.session.height = f.rect_height;
  uint64_t samples_done;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    samples_done = stats_.samples_done;
  }
  kg.session.samples_done = uint32_t(samples_done);
  kg.session.samples_per_pass = kSamplesPerPass;
  kg.session.pass_index = pass_index_;
  kg.session.sampler_mode = ar::SAMPLER_SOBOL;
  if (const auto s = scene_.settings()) {
    kg.session.seed = s->s.seed;
    // PathStateSoA::bounces order: total, diffuse, glossy, transmission, volume, transparent.
    const uint32_t b[6] = {s->s.max_bounces,          s->s.diffuse_bounces, s->s.glossy_bounces,
                           s->s.transmission_bounces, s->s.volume_bounces,  s->s.transparent_bounces};
    std::memcpy(kg.session.max_bounces, b, sizeof b);
  }
  device_.upload(globals_buf_, 0, &kg, sizeof kg);
  device_.upload(globals_buf_, kFilmDataAt, &f, sizeof f);
  device_.set_kernel_globals(globals_buf_);
  scheduler_->config.globals = kg;
}

void Session::resolve(uint64_t pass_epoch, uint64_t generation) {
  ++resolves_;
  if (options_.test_stale_resolve_at_pass != 0 && resolves_ == options_.test_stale_resolve_at_pass) --pass_epoch;
  // Writes carry their epoch; one from an epoch that has been reset since is dropped (0004 D25).
  if (pass_epoch != film_epoch_) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.stale_dropped;
    return;
  }
  bind_globals();
  film_->check_resolve_fits(kSamplesPerPass);
  device_.launch(KernelId::FILM_RESOLVE, film_->data().rect_width * film_->data().rect_height);
  device_.synchronize();
  invalid_.report(film_->data(), report_, generation);
  ++pass_index_;
  ++passes_in_epoch_;
  std::lock_guard<std::mutex> lock(mutex_);
  stats_.samples_done += kSamplesPerPass;
  ++stats_.passes_resolved;
}

void Session::present(uint64_t generation) {
  bind_globals();
  const ar::FilmData& f = film_->data();
  const uint32_t mode = latch_.display_mode();
  ar::DisplayTarget target{present_buf_.ptr, 0u, 0.0f};
  KernelId kernel = KernelId::FILM_PRESENT;
  DisplayFormat format = DisplayFormat::RGBA16F;
  if (mode != AR_DISPLAY_BEAUTY) {
    // 0008 d12 default ranges: sample count [0, max spp], bounces [0, max depth].
    const bool spp = mode == AR_DISPLAY_SAMPLE_COUNT;
    target.mode = spp ? ar::HEATMAP_SAMPLE_COUNT : ar::HEATMAP_BOUNCE_COUNT;
    const auto s = scene_.settings();
    target.range_max = float(spp ? sample_limit() : (s ? s->s.max_bounces : 0u));
    // The kernel only asserts these in debug builds, so the host refuses them here.
    if (!(target.range_max > 0.0f) || !std::isfinite(target.range_max))
      throw std::invalid_argument(std::string("heat map: range_max must be finite and positive (") +
                                  (spp ? "the sample limit" : "max_bounces") + " is 0)");
    if (!ar::film_heatmap_supported(f, target.mode))
      throw std::invalid_argument("heat map: the film lacks the channels display mode " + std::to_string(mode) +
                                  " reads");
    kernel = KernelId::FILM_HEATMAP;
    format = DisplayFormat::RGBA8_SRGB;
  }
  device_.upload(globals_buf_, kDisplayAt, &target, sizeof target);
  const uint32_t count = f.rect_width * f.rect_height;
  device_.launch(kernel, count);
  device_.synchronize();
  // A frame dropped because every slot was being read was never shown, so it does not count as
  // the latched state having presented (the interrupt rule's condition).
  if (ring_->publish(device_, present_buf_, generation, f.rect_width, f.rect_height, format) == 0) return;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.frames_published;
  }
  latch_.mark_presented();
}

void Session::run() {
  try {
    allocate();
    render_loop();
  } catch (const std::exception& e) {
    fail(e.what());
  } catch (...) {
    fail("unknown exception on the render thread");
  }
  try {
    release();
  } catch (const std::exception& e) {
    fail(std::string("freeing session memory: ") + e.what());
  }
  on_thread_exit();
}

void Session::render_loop() {
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      for (;;) {
        if (closing_) return;
        if (failed_) {
          // FAILED stays FAILED (a failure raised on another thread included) until close.
        } else if (!started_) {
          // INITIALIZING until started.
        } else if (paused_) {
          if (state_ != AR_STATE_PAUSED) set_state_locked(AR_STATE_PAUSED);
        } else if (idle_) {
          if (state_ != AR_STATE_CONVERGED) set_state_locked(AR_STATE_CONVERGED);
        } else {
          break;
        }
        wake_cv_.wait(lock);
      }
      if (state_ != AR_STATE_RENDERING) set_state_locked(AR_STATE_RENDERING);
    }
    std::unique_lock<std::mutex> render(render_mutex_);
    const uint64_t active = scene_.generations().active();
    const LatchResult lr = latch_.latch(active);
    {
      // The interrupt flag follows the state just latched: an interrupt raised for pending state
      // this latch consumed is spent, one raised for state still pending stays (0007 d25).
      std::lock_guard<std::mutex> lock(mutex_);
      scheduler_->epoch_pending = latch_.interrupt_due(scene_.generations().active());
    }
    if (expect_epoch_ && !lr.new_epoch)
      throw std::logic_error("session: a pass was interrupted for a new epoch, but the next latch started none");
    expect_epoch_ = false;
    if (lr.new_epoch) begin_epoch();
    apply_view();
    bool need_pass;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      need_pass = !converged_locked();
    }
    if (need_pass) {
      bind_globals();
      const PassStatus st = scheduler_->run_pass(PassWork{options_.test_camera_samples_per_pass, 0});
      if (st == PassStatus::CANCELLED) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.passes_cancelled;
        if (closing_) return;
        scheduler_->cancel = false;
        continue;  // cancel() set paused_, fail() set failed_: the loop top parks
      }
      if (st == PassStatus::EPOCH_PENDING) {
        // 0007 d25: the interrupted pass is not resolved; the next latch resets the film.
        expect_epoch_ = true;
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.passes_interrupted;
        continue;
      }
      resolve(lr.epoch, lr.generation);
    }
    if (need_pass || lr.changed) present(lr.generation);
    render.unlock();
    std::lock_guard<std::mutex> lock(mutex_);
    if (converged_locked() && !latch_.pending(scene_.generations().active())) idle_ = true;
  }
}

}  // namespace astroray::host
