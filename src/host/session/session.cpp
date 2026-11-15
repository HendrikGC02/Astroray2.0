// SPDX-License-Identifier: Apache-2.0
// Ref: docs/decisions/0007-blender-integration.md d5, d23, d25, d27, d29; 0004 D25; 0008 d3, d12;
// RULINGS 52 (cancel between launches), 69 (generation numbers of latched views).
#include "host/session/session.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <utility>

#include "host/data_resource.h"
#include "host/thread.h"
#include "kernel/film/resolve.h"
#include "kernel/sample/sobol.h"

namespace astroray::host {
namespace {

constexpr uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }
constexpr uint64_t kFilmDataAt = align_up(sizeof(ar::KernelGlobals), 256);
constexpr uint64_t kDisplayAt = kFilmDataAt + align_up(sizeof(ar::FilmData), 256);
constexpr uint64_t kGlobalsBytes = kDisplayAt + sizeof(ar::DisplayTarget);
// One pass is one sample per active pixel (0004 D25); the adaptive tail is not built.
constexpr uint32_t kSamplesPerPass = 1;

bool terminal(uint32_t state) {
  return state == AR_STATE_CONVERGED || state == AR_STATE_CLOSED || state == AR_STATE_FAILED;
}

}  // namespace

Session::Session(Scene& scene, Device& device, const SessionOptions& options)
    : scene_(scene), device_(device), options_(options) {
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
  thread_ = start_engine_thread(scene_.ctx(), [this] { run(); });
}

Session::~Session() {
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

void Session::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (closing_) return;
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
  std::lock_guard<std::mutex> lock(mutex_);
  if (closing_) return;
  closing_ = true;
  if (scheduler_) scheduler_->cancel = true;
  if (exited_) set_state_locked(AR_STATE_CLOSED);
  wake_cv_.notify_all();
}

bool Session::wait(uint32_t timeout_ms) {
  std::unique_lock<std::mutex> lock(mutex_);
  const auto pred = [this] { return terminal(state_) || scene_.ctx().poisoned.load(std::memory_order_acquire); };
  return state_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), pred);
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
  if (latch_.set_view(view)) wake();
  raise_interrupt_if_due();
}

void Session::set_display_mode(uint32_t mode) {
  if (mode != AR_DISPLAY_BEAUTY && mode != AR_DISPLAY_SAMPLE_COUNT && mode != AR_DISPLAY_BOUNCE_COUNT)
    throw SessionUnsupported("set_display_mode: display mode " + std::to_string(mode) + " is not supported yet");
  // Display-only: no epoch, so never an interrupt; applied at the next present.
  if (latch_.set_display_mode(mode)) wake();
}

void Session::scene_generation_activated() {
  if (latch_.pending(scene_.generations().active())) wake();
  raise_interrupt_if_due();
}

void Session::raise_interrupt_if_due() {
  if (!latch_.interrupt_due(scene_.generations().active())) return;
  std::lock_guard<std::mutex> lock(mutex_);
  if (scheduler_) scheduler_->epoch_pending = true;
}

void Session::wake() {
  std::lock_guard<std::mutex> lock(mutex_);
  idle_ = false;
  // The caller's next wait() must not see the CONVERGED of the state it just changed.
  if (state_ == AR_STATE_CONVERGED && started_ && !paused_ && !closing_) set_state_locked(AR_STATE_RENDERING);
  wake_cv_.notify_all();
}

SessionProgress Session::progress() const {
  SessionProgress p;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    p.state = state_;
  }
  if (p.state != AR_STATE_CLOSED && scene_.ctx().poisoned.load(std::memory_order_acquire)) p.state = AR_STATE_FAILED;
  p.active_generation = latch_.generation();
  p.pending_generation = scene_.generations().pending();
  p.failed_generation = scene_.generations().failed();
  p.epoch = latch_.epoch();
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
  try {
    scene_.ctx().log.line("ERROR", "session failed: " + why);
    const double value[2] = {0.0, 0.0};
    report_.raise(AR_REPORT_SESSION_FAILED, 0, why, 1, value, latch_.generation());
  } catch (...) {
    // Out of memory while reporting: the state and the stored text below still say it.
  }
  std::lock_guard<std::mutex> lock(mutex_);
  failed_ = true;
  failure_ = why;
  set_state_locked(AR_STATE_FAILED);
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
  if (ring_->publish(device_, present_buf_, generation, f.rect_width, f.rect_height, format) != 0) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.frames_published;
  }
  latch_.mark_presented();
}

void Session::run() {
  try {
    allocate();
    for (;;) {
      {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
          if (closing_) break;
          if (!started_) {
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
        if (closing_) break;
        if (state_ != AR_STATE_RENDERING) set_state_locked(AR_STATE_RENDERING);
        // Pending state is folded in below, so an interrupt raised before this point is spent.
        scheduler_->epoch_pending = false;
      }
      std::unique_lock<std::mutex> render(render_mutex_);
      const LatchResult lr = latch_.latch(scene_.generations().active());
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
          if (closing_) break;
          scheduler_->cancel = false;
          continue;  // paused_ is set by cancel(): the loop top parks in PAUSED
        }
        if (st == PassStatus::EPOCH_PENDING) {
          // 0007 d25: the interrupted pass is not resolved; the next latch resets the film.
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
  std::lock_guard<std::mutex> lock(mutex_);
  exited_ = true;
  if (closing_) set_state_locked(AR_STATE_CLOSED);
}

}  // namespace astroray::host
