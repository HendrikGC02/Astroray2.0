// SPDX-License-Identifier: Apache-2.0
// Ref: docs/decisions/0007-blender-integration.md d5, d23, d25, d27, d29; 0004 D25; 0008 d3, d12;
// RULINGS 52 (cancel between launches), 69 (generation numbers of latched views), 70 (uploads at
// the latch, pinned table sets).
#include "host/session/session.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <thread>
#include <utility>

#include "host/data_resource.h"
#include "host/light/light_builder.h"
#include "host/scene/camera.h"
#include "host/thread.h"
#include "kernel/sample/sobol.h"
#include "host/scene/accel.h"

namespace astroray::host {
namespace {


constexpr uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }
constexpr uint64_t kFilmDataAt = align_up(sizeof(ar::KernelGlobals), 256);
constexpr uint64_t kDisplayAt = kFilmDataAt + align_up(sizeof(ar::FilmData), 256);
// FilmData::invalid_total: one u32, cleared before each resolve and read after it.
constexpr uint64_t kInvalidTotalAt = align_up(kDisplayAt + sizeof(ar::DisplayTarget), 16);
constexpr uint64_t kGlobalsBytes = kInvalidTotalAt + sizeof(uint32_t);
// One pass is one sample per active pixel (0004 D25); the adaptive tail is not built.
constexpr uint32_t kSamplesPerPass = 1;
// The ABI's sampler mode is the kernel's value (ruling 60): bind_globals passes the latched settings'
// mode through and the FITS provenance names it.
static_assert(AR_SAMPLER_SOBOL == ar::SAMPLER_SOBOL && AR_SAMPLER_INDEPENDENT == ar::SAMPLER_INDEPENDENT);
// The slice wait() sleeps between checks of the poisoned flag, which no notification announces:
// the period Scene::wait_init uses.
constexpr auto kWaitSlice = std::chrono::milliseconds(50);

bool terminal(uint32_t state) {
  return state == AR_STATE_CONVERGED || state == AR_STATE_CLOSED || state == AR_STATE_FAILED;
}

// The film request of the settings `s` (null: none committed yet). Pass names are matched against
// what list_passes enumerates for a film with every channel this build can produce, so readback's
// table stays the one mapping from names to channels (0008 d10, d13a). A name outside that list is
// appended to `unknown`, so a statistics pass requested with the statistics off is one too. Combined
// is always on; the statistics follow ar_settings.statistics, on unless switched off (0008 d12).
FilmRequest film_request(const SessionOptions& o, const StoredSettings* s, std::vector<std::string>* unknown) {
  FilmRequest req;
  req.max_width = o.max_width;
  req.max_height = o.max_height;
  req.statistics = !s || s->s.statistics != AR_STATISTICS_OFF;  // 0008 d12: on by default
  if (!s || s->passes.empty()) return req;
  FilmRequest all = req;
  all.primary_aov = true;
  const std::vector<PassDesc> produced = list_passes(FilmLayout::build(all));
  for (const ar_name& n : s->passes) {
    const char* text = reinterpret_cast<const char*>(n.text);
    const std::string name(text, std::find(text, text + sizeof n.text, '\0'));
    const auto it = std::find_if(produced.begin(), produced.end(), [&](const PassDesc& p) { return p.name == name; });
    if (it == produced.end()) unknown->push_back(name);
    else if (it->source == PassSource::Aov) req.primary_aov = true;
  }
  return req;
}

bool same_channels(const FilmLayout& a, const FilmLayout& b) {
  return std::equal(a.channels().begin(), a.channels().end(), b.channels().begin(), b.channels().end(),
                    [](const FilmChannel& x, const FilmChannel& y) { return x.name == y.name; });
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
  // What the settings committed so far request; the render thread rebuilds it from the settings it
  // latches (sync_film), which also reports names it cannot produce.
  std::vector<std::string> unknown;
  layout_ = FilmLayout::build(film_request(options, scene.settings().get(), &unknown));
  render_size_ = uint64_t(options.max_width) << 32 | options.max_height;
  hooks_ = read_test_hooks();
  // A test hook changes what a render measures, so each active one is visible in the report (0001
  // D2.11 TEST_HOOK_ACTIVE); production builds have none.
  for (const std::string& name : hooks_.active) {
    const double value[2] = {0.0, 0.0};
    report_.raise(AR_REPORT_TEST_HOOK_ACTIVE, 0, name, 1, value, 0);
  }
  // Once per session (0002 D15): an architecture the runtime tests do not cover renders, with this warning.
  if (const std::string arch = device_.untested_architecture(); !arch.empty()) {
    const double value[2] = {0.0, 0.0};  // value[0] = PTX JIT seconds: no JIT path exists, so 0
    report_.raise(AR_REPORT_UNTESTED_DEVICE, 0, arch + ": not covered by runtime tests", 1, value, 0);
  }
  SchedulerConfig cfg;
  cfg.report = &report_;
  cfg.owner = options.owner;
  cfg.capacity_override = hooks_.pool_main;
  cfg.shadow_capacity_override = hooks_.pool_shadow;
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
  if (!failed_) return;
  const std::string text = std::string(what) + ": the session failed: " + failure_;
  // 0002 D15: a device failure stays AR_ERR_DEVICE for every later call.
  if (failure_device_) throw DeviceError(text, failure_sticky_);
  throw std::runtime_error(text);
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
      try {
        detached_scene_report_ = read_report(scene_.report());
      } catch (...) {
        // The rest of detach must still run, but the loss is loud: logged here, and every later
        // report_snapshot fails naming it instead of showing a table that lacks the entries.
        detached_scene_report_ = ReportTable{};
        scene_report_lost_ = true;
        try {
          ctx_.log.line("ERROR", "session: the scene's report entries could not be read at detach and are lost");
        } catch (...) {
          // The flag still makes report_snapshot fail.
        }
      }
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

std::vector<PresentRecord> Session::present_log() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return present_log_;
}

double Session::now_s() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
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
  batching_.note_view(StoredView::copy_of(view), now_s());
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
    batching_.note_commit(now_s());
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
  p.failed_generation = std::max(p.failed_generation, upload_failed_.load(std::memory_order_acquire));
  p.samples_per_pixel = uint32_t(stats_.samples_done);
  p.film_epoch = published_film_epoch_;
  p.last_pass_ms = last_pass_ms_;
  p.device_free_bytes = device_free_bytes_;
  p.device_total_bytes = device_total_bytes_;
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
  uint32_t n = 0;
  const uint64_t at = uint64_t(reinterpret_cast<const char*>(f.n + ar::film_active_pixel(f, j)) -
                               static_cast<const char*>(film_->buffer().ptr));
  device_.download(&n, film_->buffer(), at, sizeof n);  // the plane is device memory on CUDA
  return n;
}

void Session::render_size(uint32_t* width, uint32_t* height) const {
  const uint64_t v = render_size_.load(std::memory_order_acquire);
  *width = uint32_t(v >> 32);
  *height = uint32_t(v);
}

FitsProvenance Session::fits_provenance_locked() const {
  // Called with render_mutex_ held, so the film (and with it observer_hash_) exists and the scene
  // is still attached: release() runs before detach_scene().
  FitsProvenance pv;
  uint32_t filter = AR_FILTER_BOX;
  uint32_t sampler = AR_SAMPLER_SOBOL;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pv.spp = uint32_t(stats_.samples_done);
    if (const auto s = scene_.settings()) {
      pv.seed = s->s.seed;
      filter = s->s.pixel_filter;
      sampler = s->s.sampler_mode;
    }
  }
  pv.sampler_mode = sampler == AR_SAMPLER_INDEPENDENT ? "independent" : "sobol";
  pv.observer_hash = observer_hash_;
  // No working-space name and no wavelength-pdf name is defined yet, so the file says so instead of
  // naming something that is not one.
  pv.working_space = "UNSPECIFIED";
  pv.filter = filter == AR_FILTER_GAUSSIAN ? "gaussian" : filter == AR_FILTER_BLACKMAN_HARRIS ? "blackman-harris" : "box";
  pv.wavelength_pdf = "UNSPECIFIED";
  pv.device = device_.kind() == AR_DEVICE_CUDA ? "cuda" : "cpu";
  return pv;
}

void Session::read_film(const std::function<void(const FilmReadContext&)>& fn) const {
  std::lock_guard<std::mutex> render(render_mutex_);
  if (!film_) throw std::invalid_argument("the session has no film (still initializing, or closed)");
  uint64_t revision;
  {
    // film_ and the revision are published together under both locks (sync_film).
    std::lock_guard<std::mutex> lock(mutex_);
    revision = pass_revision_;
  }
  fn(FilmReadContext{device_, *film_, fits_provenance_locked(), revision});
}

std::optional<DisplayFrame> Session::display_acquire(uint64_t after_frame) {
  std::lock_guard<std::mutex> lock(ring_mutex_);
  if (!ring_) throw std::invalid_argument("the session has no display ring (still initializing, or closed)");
  return ring_->acquire(after_frame);
}

void Session::display_release(uint64_t frame_id) {
  std::lock_guard<std::mutex> lock(ring_mutex_);
  if (!ring_) throw std::invalid_argument("the session has no display ring (still initializing, or closed)");
  ring_->release(frame_id);
}

PassCounters Session::pass_counters() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_counters_;
}

ar_report_info Session::report_snapshot() {
  // Scene reads happen under mutex_, like progress(): detach_scene copies the scene's table under
  // the same lock before the scene may go.
  std::lock_guard<std::mutex> lock(mutex_);
  if (scene_report_lost_)
    throw std::runtime_error("the scene's report entries were lost when the session detached from the scene");
  report_snap_ = merge_reports(scene_detached_ ? detached_scene_report_ : read_report(scene_.report()),
                               read_report(report_));
  return report_snap_.info;
}

void Session::report_entry(uint32_t index, ar_report_entry* out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (index >= report_snap_.entries.size())
    throw std::invalid_argument("report entry " + std::to_string(index) + " is past the snapshot's " +
                                std::to_string(report_snap_.entries.size()) + " entries");
  *out = report_snap_.entries[index];
}

void Session::fail(const std::string& why, const DeviceError* device_error) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (failed_) return;  // the first failure is the one reported
    failed_ = true;
    failure_ = why;
    failure_device_ = device_error != nullptr;
    failure_sticky_ = device_error != nullptr && device_error->sticky();
    // Stop the pass in flight; the render loop parks until close.
    if (scheduler_) scheduler_->cancel = true;
    // The entry goes in before the state changes, so a host that sees FAILED and then reads the report
    // finds it (lock order mutex_, then the report, as report_snapshot takes them).
    try {
      const double value[2] = {0.0, 0.0};
      report_.raise(AR_REPORT_SESSION_FAILED, 0, why, 1, value, latch_.generation());
    } catch (...) {
      // Out of memory while reporting: the state and the stored text still say it.
    }
    set_state_locked(AR_STATE_FAILED);
    wake_cv_.notify_all();
  }
  try {
    ctx_.log.line("ERROR", "session failed: " + why);
  } catch (...) {
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

#ifdef ASTRORAY_GPU
  if (device_.kind() == AR_DEVICE_CUDA) {
    // One pipeline per session on the scene's OptiX context, which exists once the scene's init has
    // run (accel_builder waits for it and throws its failure). release() detaches it before the
    // scene can go (ar_scene_destroy refuses while a session is not CLOSED).
    const auto t0 = std::chrono::steady_clock::now();
    const TraversalAttach t = attach_cuda_traversal(device_, scene_.accel_builder(), options_.owner);
    const std::chrono::duration<double, std::milli> took = std::chrono::steady_clock::now() - t0;
    char line[160];
    std::snprintf(line, sizeof line, "session: traversal pipeline created in %.1f ms (module %.1f ms, pipeline %.1f ms)",
                  took.count(), t.module_ms, t.pipeline_ms);
    ctx_.log.line("INFO", line);
  }
#endif
  {
    // Published under the render lock so a readback on another thread sees a whole film.
    std::lock_guard<std::mutex> render(render_mutex_);
    sync_film();
    static const char* hex = "0123456789abcdef";
    observer_hash_.clear();
    for (uint8_t b : obs.observer_hash) observer_hash_ += {hex[b >> 4], hex[b & 15]};
  }

  const uint64_t frame_bytes = uint64_t(options_.max_width) * options_.max_height * 8u;  // RGBA16F
  globals_buf_ = device_.allocate(LedgerCategory::TABLES, kGlobalsBytes, options_.owner);
  present_buf_ = device_.allocate(LedgerCategory::DISPLAY_FRAME, frame_bytes, options_.owner);
  active_ = std::make_unique<ActivePixels>(device_, options_.max_width, options_.max_height, options_.owner);
  camera_buf_ = device_.allocate(LedgerCategory::TABLES, sizeof(ar::CameraView), options_.owner);
  filter_buf_ = device_.allocate(LedgerCategory::TABLES, sizeof(ar::FilterTable), options_.owner);
  auto ring = std::make_unique<DisplayRing>(frame_bytes);
  std::lock_guard<std::mutex> lock(ring_mutex_);
  ring_ = std::move(ring);
}

void Session::sync_film() {
  // Called with render_mutex_ held, before the first pass and at every epoch start: the settings
  // only change with an activated commit, and every one starts an epoch (0007 d25).
  const auto s = scene_.settings();
  if (film_ && s == film_settings_) return;
  // Held, not just compared: a new settings object can then never reuse the old one's address.
  film_settings_ = s;
  stats_display_warned_ = false;
  std::vector<std::string> unknown;
  const FilmLayout layout = FilmLayout::build(film_request(options_, s.get(), &unknown));
  // Ruling 74: one entry per name and settings change, detail = the pass name; the render goes on
  // without the pass.
  for (const std::string& name : unknown) {
    const double value[2] = {0.0, 0.0};
    report_.raise(AR_REPORT_PASS_UNSUPPORTED, 0, name, 1, value, latch_.generation());
  }
  if (film_ && same_channels(layout, film_->layout())) return;
  auto film = std::make_unique<Film>(device_, layout, options_.owner);
  // The response and wavelength views live in the film's FilmData; bind_globals uploads the new
  // film's address before the next launch.
  SessionTables tables = upload_session_tables(device_, options_.owner, response_, wavelengths_, film->data());
  film_ = std::move(film);
  session_tables_ = std::move(tables);
  std::lock_guard<std::mutex> lock(mutex_);
  if (!same_channels(layout, layout_)) ++pass_revision_;
  layout_ = layout;
}

void Session::release() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    scheduler_.reset();  // frees the pools
  }
  // The pipeline goes while the scene's OptiX context lives. A detach error (a dead device) still
  // freed the pipeline; it is rethrown once everything else was released.
  std::exception_ptr detach_failed;
#ifdef ASTRORAY_GPU
  if (device_.kind() == AR_DEVICE_CUDA) {
    try {
      detach_cuda_traversal(device_);
    } catch (...) {
      detach_failed = std::current_exception();
    }
  }
#endif
  std::lock_guard<std::mutex> render(render_mutex_);
  // The ring stays until the session is destroyed: a frame the host acquired remains readable
  // after CLOSED and is released then (display_release), so no thread reads freed memory.
  film_.reset();
  film_settings_.reset();
  tables_.reset();  // the pin: the set's copies go when no session holds them (RULINGS 70)
  session_tables_ = SessionTables{};
  active_.reset();
  device_.free(camera_buf_);
  device_.free(filter_buf_);
  device_.free(present_buf_);
  device_.free(globals_buf_);
  if (detach_failed) std::rethrow_exception(detach_failed);
}

uint32_t Session::sample_limit() const {
  const auto s = scene_.settings();
  if (!s) return 0;
  return options_.mode == AR_SESSION_MODE_VIEWPORT ? s->s.viewport_samples : s->s.samples;
}

bool Session::converged_locked() const {
  // No work: one empty pass shows the latched state, more would only spin.
  if (pass_camera_samples() == 0 && passes_in_epoch_ > 0) return true;
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
    published_film_epoch_ = latch_.epoch();
  }
  pass_index_ = 0;
  passes_in_epoch_ = 0;
  unpresented_ = 0;
  batching_.begin_epoch();
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
  render_size_ = uint64_t(w) << 32 | h;
}

void Session::bind_globals() {
  KgSession in;
  char* base = static_cast<char*>(globals_buf_.ptr);
  in.film = reinterpret_cast<ar::FilmData*>(base + kFilmDataAt);
  in.display = reinterpret_cast<ar::DisplayTarget*>(base + kDisplayAt);
  const ar::FilmData& f = film_->data();
  ar::SessionUniforms& u = in.uniforms;
  u.width = f.rect_width;
  u.height = f.rect_height;
  uint64_t samples_done;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    samples_done = stats_.samples_done;
  }
  u.samples_done = uint32_t(samples_done);
  u.samples_per_pass = kSamplesPerPass;
  u.pass_index = pass_index_;
  u.sampler_mode = AR_SAMPLER_SOBOL;  // no settings yet: the ABI's zero default
  u.test_hooks = hooks_.bits;
  in.active_pixels = active_->list();
  in.camera = camera_ready_ ? static_cast<const ar::CameraView*>(camera_buf_.ptr) : nullptr;
  in.filter = filter_ready_ ? static_cast<const ar::FilterTable*>(filter_buf_.ptr) : nullptr;
  if (const auto s = scene_.settings()) {
    u.seed = s->s.seed;
    u.sampler_mode = s->s.sampler_mode;
    // PathStateSoA::bounces order: total, diffuse, glossy, transmission, volume, transparent.
    const uint32_t b[6] = {s->s.max_bounces,          s->s.diffuse_bounces, s->s.glossy_bounces,
                           s->s.transmission_bounces, s->s.volume_bounces,  s->s.transparent_bounces};
    std::memcpy(u.max_bounces, b, sizeof b);
    u.min_bounces = s->s.min_bounces;
  }
  const ar::KernelGlobals kg = build_kernel_globals(tables_.get(), in);
  if (tables_) {
    // Rule 5: a null pointer that is not optional is a builder bug, never a silent fallback.
    const std::vector<std::string> missing = missing_pointers(kg, *tables_, false);
    if (!missing.empty()) throw std::logic_error("kernel globals: " + missing.front() + " is null");
  }
  device_.upload(globals_buf_, 0, &kg, sizeof kg);
  ar::FilmData fd = f;
  fd.invalid_total = reinterpret_cast<uint32_t*>(base + kInvalidTotalAt);
  device_.upload(globals_buf_, kFilmDataAt, &fd, sizeof fd);
  device_.set_kernel_globals(globals_buf_);
#ifdef ASTRORAY_GPU
  // The pinned snapshot's TLAS: SceneData::accel holds its handle as an integer (scene_view.h).
  if (device_.kind() == AR_DEVICE_CUDA)
    set_cuda_traversable(device_, tables_ ? uint64_t(reinterpret_cast<uintptr_t>(tables_->scene_host.accel)) : 0u);
#endif
  scheduler_->config.globals = kg;
  // The material sort's threshold and scratch size follow the pinned table set (the scheduler
  // reallocates sort_counts when the count changes).
  scheduler_->config.material_header_count = tables_ ? tables_->scene_host.material_header_count : 0u;
  scheduler_->config.material_program_count = tables_ ? tables_->material_program_count : 0u;
}

void Session::update_camera_and_filter(uint64_t generation) {
  const ar::FilmData& f = film_->data();
  camera_ready_ = false;
  if (latch_.has_view()) {
    const StoredView v = latch_.view();
    ar::CameraView view{};
    // The host folds the scene origin into the camera before it sends the descriptor, so it is
    // origin-relative already and the origin passed here is zero (0003 d26; 0007).
    const double origin[3] = {0.0, 0.0, 0.0};
    if (make_camera_view(v.v.camera, f.rect_width, f.rect_height, origin, 0, report_, generation, &view) == AR_OK) {
      device_.upload(camera_buf_, 0, &view, sizeof view);
      camera_ready_ = true;
    } else {
      // A descriptor the host sent wrong is the host's bug to fix, not a reason to kill the
      // session: the pass has no camera work until the next view, and the report says so on the
      // closest existing code (the view is not rendered as asked).
      const double value[2] = {0.0, 0.0};
      report_.raise(AR_REPORT_CAMERA_UNSUPPORTED, 0, "invalid camera descriptor: no camera paths are launched", 1,
                    value, generation);
    }
  }
  // Pixel filter of the scene's settings: rebuilt only when it changes. An unknown type or width
  // makes build_filter_table throw, which fails the session.
  const auto s = scene_.settings();
  filter_ready_ = filter_ready_ && s != nullptr;
  if (s) {
    const FilterSettings want{static_cast<FilterType>(s->s.pixel_filter), s->s.pixel_filter_width};
    if (!filter_ready_ || want.type != filter_uploaded_.type || want.width != filter_uploaded_.width) {
      const auto table = std::make_unique<ar::FilterTable>(build_filter_table(want));
      device_.upload(filter_buf_, 0, table.get(), sizeof(ar::FilterTable));
      filter_uploaded_ = want;
      filter_ready_ = true;
    }
  }
}

// One thread per listed pixel writes the pass's list before anything reads it (0008 d3).
void Session::build_active() {
  // Before any slot is written: the pass's slots must fit the film's capacity.
  film_->check_resolve_fits(kSamplesPerPass);
  bind_globals();
  device_.launch(KernelId::FILM_BUILD_ACTIVE, active_->plan(film_->data()));
}

uint64_t Session::pass_camera_samples() const {
  if (options_.test_camera_samples_per_pass != 0) return options_.test_camera_samples_per_pass;
  return camera_ready_ && filter_ready_ && tables_ ? uint64_t(active_->count()) * kSamplesPerPass : 0;
}

bool Session::latch_tables() {
  // RULINGS 70: a failed upload keeps the previous set, records the snapshot's generation and
  // names the table in one entry per failed generation; the next latch retries. Failures that
  // cannot heal (a sticky device error, device init, host memory) are not caught: they fail the
  // session (0001 D2.5).
  const auto before = tables_;
  try {
    tables_ = acquire_scene_tables(scene_);
    tables_retry_ = false;
  } catch (const TableUploadFailed& e) {
    tables_retry_ = true;
    const uint64_t g = e.generation();
    upload_failed_.store(g, std::memory_order_release);
    if (g != upload_failed_reported_) {
      upload_failed_reported_ = g;
      const double value[2] = {double(g), 0.0};
      report_.raise(AR_REPORT_GENERATION_REJECTED, 0, e.what(), 1, value, g);
      ctx_.log.line("WARNING", std::string("session: ") + e.what() + "; rendering the previous scene tables");
    }
  }
  return tables_ != before;
}

bool Session::resolve(uint64_t pass_epoch, uint64_t generation) {
  ++resolves_;
  if (options_.test_stale_resolve_at_pass != 0 && resolves_ == options_.test_stale_resolve_at_pass) --pass_epoch;
  // Writes carry their epoch; one from an epoch that has been reset since is dropped (0004 D25).
  if (pass_epoch != film_epoch_) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.stale_dropped;
    return false;
  }
  bind_globals();
  // The resolve counts the pass's invalid samples into FilmData::invalid_total as well as the
  // plane, so the INVALID_SAMPLE entry needs 4 bytes, not the plane (8 MiB at 1080p, whose download
  // and host sum had left the GPU idle about 0.9 ms per pass). The clear and the read are ordered
  // with the launch on the device's stream; the read is the pass's one wait for its resolve.
  device_.memset(globals_buf_, kInvalidTotalAt, 0, sizeof(uint32_t));
  device_.launch(KernelId::FILM_RESOLVE, active_->count());
  uint32_t pass_invalid = 0;
  device_.download(&pass_invalid, globals_buf_, kInvalidTotalAt, sizeof pass_invalid);
  invalid_.report_pass(pass_invalid, uint64_t(active_->count()) * (kSamplesPerPass + 1), report_, generation);
  ++pass_index_;
  ++passes_in_epoch_;
  return true;
}

void Session::present(uint64_t generation) {
  bind_globals();
  const ar::FilmData& f = film_->data();
  uint32_t mode = latch_.display_mode();
  if (mode == AR_DISPLAY_BOUNCE_COUNT && !film_->layout().find("bounces")) {
    // ar_settings.statistics is off, so the film has no bounce channel: the beauty is shown and
    // one PASS_UNSUPPORTED entry per settings change names the pass the heat map reads (ruling 74's
    // code and detail), never a silent "no data" map.
    if (!stats_display_warned_) {
      const double value[2] = {0.0, 0.0};
      report_.raise(AR_REPORT_PASS_UNSUPPORTED, 0, "AR Bounces", 1, value, generation);
      stats_display_warned_ = true;
    }
    mode = AR_DISPLAY_BEAUTY;
  }
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
  device_.launch(kernel, active_->count());
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
  } catch (const DeviceError& e) {
    fail(e.what(), &e);
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
          // 0007 d50: during interaction each displayed update is one pass at most 60 times a
          // second, so the next pass waits until the last frame is a publish gap old. A new view
          // wakes the wait and is latched when it ends.
          const double now = now_s();
          const double wait = last_present_s_ + kInteractionPublishGapS - now;
          if (wait > 0.0 && batching_.interacting(now)) {
            wake_cv_.wait_for(lock, std::chrono::duration<double>(wait));
            continue;
          }
          break;
        }
        wake_cv_.wait(lock);
      }
      if (state_ != AR_STATE_RENDERING) set_state_locked(AR_STATE_RENDERING);
    }
    std::unique_lock<std::mutex> render(render_mutex_);
    const uint64_t active = scene_.generations().active();
    // A retry that pins a new set changes the image, so it starts an epoch like any image change
    // (RULINGS 69, 70).
    const bool retried = tables_retry_ && latch_tables();
    // An interrupted pass forces the epoch (latch.h), even if the pending view went back.
    const LatchResult lr = latch_.latch(active, expect_epoch_ || retried);
    {
      // The interrupt flag follows the state just latched: an interrupt raised for pending state
      // this latch consumed is spent, one raised for state still pending stays (0007 d25).
      std::lock_guard<std::mutex> lock(mutex_);
      scheduler_->epoch_pending = latch_.interrupt_due(scene_.generations().active());
    }
    expect_epoch_ = false;
    if (lr.new_epoch) {
      // s moves only when emitters change, and every activated commit starts an epoch, so one
      // epoch's samples never mix two values (0003 d10). The set and its light table come from one
      // snapshot, so s and the lights it was folded into agree.
      if (!retried) latch_tables();
      sync_film();  // a changed pass request rebuilds the film here, before the reset
      begin_epoch();
      const LightTable* lt = tables_ ? tables_->lights.get() : nullptr;
      const double inv_s = lt ? 1.0 / lt->radiance_scale : 1.0;
      if (inv_s != film_->data().inv_radiance_scale) {
        // s is invisible in the film by design (0003 d10), so the log is where a test (0003 check 15)
        // sees the value the render used.
        int e = 0;
        std::frexp(inv_s, &e);  // inv_s = 2^-k exactly, so e = 1 - k
        ctx_.log.line("INFO", "session: radiance scale s = 2^" + std::to_string(1 - e));
      }
      film_->data().inv_radiance_scale = inv_s;
    }
    apply_view();
    if (lr.changed) update_camera_and_filter(lr.generation);
    build_active();
    bool need_pass;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      need_pass = !converged_locked();
    }
    if (need_pass) {
      // build_active bound this pass's globals and the scheduler's config just above; nothing
      // they read has changed since.
      const auto pass_start = std::chrono::steady_clock::now();
      const PassStatus st = scheduler_->run_pass(PassWork{pass_camera_samples(), 0});
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
      const bool resolved = resolve(lr.epoch, lr.generation);
      const std::chrono::duration<float, std::milli> took = std::chrono::steady_clock::now() - pass_start;
      batching_.note_pass(double(took.count()) * 1e-3);
      uint64_t free_bytes = 0, total_bytes = 0;
      device_.memory_info(&free_bytes, &total_bytes);  // the render thread is the device's only caller here
      if (options_.test_sleep_after_resolve_ms != 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(options_.test_sleep_after_resolve_ms));
      // One hold publishes the pass: a poll that sees the new sample count reads this pass's time
      // and counters, never the previous pass's.
      std::lock_guard<std::mutex> lock(mutex_);
      if (resolved) {
        stats_.samples_done += kSamplesPerPass;
        ++stats_.passes_resolved;
      }
      last_pass_ms_ = took.count();
      device_free_bytes_ = free_bytes;
      device_total_bytes_ = total_bytes;
      last_counters_ = scheduler_->counters();
    }
    // 0007 d50: during interaction every pass is a displayed update; afterwards several passes
    // run between updates (0009 B-34). The last pass before the session idles always presents, as
    // does a display-mode change and a latch with no pass to follow it.
    bool show = lr.changed && !need_pass;
    uint32_t target = 1;
    bool interacting = false;
    if (need_pass) {
      ++unpresented_;
      const double t = now_s();
      bool done;
      uint64_t samples;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        done = converged_locked();
        samples = stats_.samples_done;
      }
      const bool mode_change = lr.changed && !lr.new_epoch;
      // The first pass of an epoch shows the new state at once (and lets the interrupt rule see a
      // presented state); the publish gap still bounds the rate.
      const bool gap_ok = t - last_present_s_ >= batching_.publish_gap_s(t);
      const bool first = passes_in_epoch_ == 1;
      target = batching_.passes_per_update(t, samples);
      interacting = batching_.interacting(t);
      show = done || mode_change || (gap_ok && (first || unpresented_ >= target));
    }
    if (show) {
      present(lr.generation);
      unpresented_ = 0;
      last_present_s_ = now_s();
      if (options_.test_record_presents) {
        std::lock_guard<std::mutex> lock(mutex_);
        present_log_.push_back({last_present_s_, stats_.passes_resolved, lr.epoch, interacting, target});
      }
    }
    render.unlock();
    std::lock_guard<std::mutex> lock(mutex_);
    if (converged_locked() && !latch_.pending(scene_.generations().active())) idle_ = true;
  }
}

}  // namespace astroray::host
