// SPDX-License-Identifier: Apache-2.0
// Sessions: one render of one scene (0007 decisions 5, 23, 25, 27; 0001 D2.5, D2.6, D2.8).
// Every entry point returns promptly (0009 B-21) except ar_session_wait, the named wait, and
// ar_session_destroy before the session reports CLOSED.
// Ref: 0007 decision 5 (function list), 0001 D2.5 (guarded entry points), D2.6 (named waits).
#include <string.h>

#include <exception>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>

#include "astroray/astroray.h"
#include "host/film/fits_writer.h"
#include "host/scene/edit.h"
#include "host/session/readback.h"
#include "host/session/session.h"
#include "recorder_hooks.h"

using astroray::host::FilmReadContext;
using astroray::host::Session;
using astroray::host::SessionUnsupported;

namespace {

template <class H>
ar_status fail(H* h, ar_status st, const std::string& why) {
  try {
    h->last_error = why;
  } catch (...) {
    // Out of memory while storing the text: the status is all that is left.
  }
  return st;
}

void copy_text(const std::string& s, uint32_t capacity, uint8_t* text) {
  if (capacity == 0) return;
  size_t n = s.size() < capacity - 1 ? s.size() : capacity - 1;
  memcpy(text, s.data(), n);
  text[n] = 0;
}

// Runs a session call; the session's exceptions become statuses and last_error.
template <class F>
ar_status session_call(ar_session h, const char* function, F&& f, bool allow_closed = false) {
  if (!h || !h->session) return AR_ERR_INVALID_ARGUMENT;
  try {
    if (h->closed && !allow_closed) return fail(h, AR_ERR_INVALID_ARGUMENT, std::string(function) + ": the session is closed");
    f(*h->session);
    return AR_OK;
  } catch (const SessionUnsupported& e) {
    return fail(h, AR_ERR_UNSUPPORTED, e.what());
  } catch (const std::invalid_argument& e) {
    return fail(h, AR_ERR_INVALID_ARGUMENT, std::string("invalid argument: ") + e.what());
  } catch (const std::bad_alloc&) {
    return fail(h, AR_ERR_OUT_OF_MEMORY, std::string(function) + ": out of host memory");
  } catch (const astroray::host::DeviceError& e) {
    return fail(h, e.status(), e.what());
  } catch (const std::exception& e) {
    return fail(h, AR_ERR_INTERNAL, e.what());
  } catch (...) {
    return fail(h, AR_ERR_INTERNAL, "unknown exception");
  }
}

// Ruling 75: pass indices belong to the pass-list revision ar_session_pass_count last reported on
// the handle.
void check_pass_revision(ar_session h, uint64_t revision) {
  if (revision != h->pass_revision.load())
    throw std::invalid_argument("the pass list changed; call ar_session_pass_count again");
}
// The session's own device (RULINGS 70): CPU sessions get a CPU device with a worker pool, CUDA
// sessions their own CudaDevice (own stream and globals) on the context's device, each with its
// own ledger. Out of the entry point for the same reason as scene_api.cpp's cuda_init.
std::shared_ptr<astroray::host::Device> session_device(astroray::ContextState& ctx, uint32_t kind, uint32_t index) {
  if (kind == AR_DEVICE_CPU) {
    // ASTRORAY_TEST_THREADS (gate builds) fixes the worker count, for the threads=1 runs of 0004
    // D4/D5 and 0002 D12; the session reads the same hooks again for its report.
    const unsigned forced = astroray::host::read_test_hooks().threads;
    const unsigned cores = std::thread::hardware_concurrency();
    return astroray::host::make_cpu_session_device(ctx, forced ? forced : cores ? cores : 1u);
  }
#ifdef ASTRORAY_GPU
  struct Held {
    astroray::host::Ledger ledger;  // declared first: destroyed after the device
    std::unique_ptr<astroray::host::ModuleDevice> device;
  };
  auto held = std::make_shared<Held>();
  held->device = astroray::host::make_cuda_device(held->ledger, index);
  return std::shared_ptr<astroray::host::Device>(held, held->device.get());
#else
  (void)index;
  throw SessionUnsupported("ar_session_create: this build has no CUDA backend");
#endif
}

}  // namespace

ar_recorder* ar_rec_of(ar_session h) { return h ? h->recorder : nullptr; }

extern "C" {

AR_API ar_status ar_session_create(ar_scene scene, const ar_session_desc* desc, ar_session* out) {
  AR_RECORD(ar_session_create, ar_rec_of(scene), scene, desc, out);
  AR_RECORD_BODY({
  if (!scene) return AR_ERR_INVALID_ARGUMENT;
  try {
    if (!scene->scene)  // destroyed: the context keeps the emptied handle (ar_scene_destroy)
      return fail(scene, AR_ERR_INVALID_ARGUMENT, "invalid argument: ar_session_create on a destroyed scene handle");
    astroray::ContextState& ctx = scene->scene->ctx();
    if (ctx.poisoned.load(std::memory_order_acquire))
      return fail(scene, AR_ERR_INTERNAL, "session create: the context is poisoned by an engine thread fault");
    if (!out) return fail(scene, AR_ERR_INVALID_ARGUMENT, "invalid argument: ar_session_create output is null");
    *out = nullptr;
    if (!desc) return fail(scene, AR_ERR_INVALID_ARGUMENT, "invalid argument: ar_session_create desc is null");
    uint32_t kind = 0, index = 0;
    astroray::context_device_of_state(ctx, &kind, &index);
    auto h = std::make_unique<ar_session_s>();
    h->recorder = ar_rec_of(scene);
    std::shared_ptr<astroray::host::Device> device = session_device(ctx, kind, index);
    astroray::host::SessionOptions o;
    o.mode = desc->mode;
    o.max_width = desc->max_width;
    o.max_height = desc->max_height;
    o.denoiser = desc->denoiser;
    o.owner = astroray::host::Owner{0, 1};  // the ledger is this session's own
    h->session = std::make_unique<Session>(*scene->scene, *device, o);
    h->device = std::move(device);
    *out = h.release();
    AR_RECORD_BIND(*out);
    return AR_OK;
  } catch (const SessionUnsupported& e) {
    return fail(scene, AR_ERR_UNSUPPORTED, e.what());
  } catch (const astroray::host::DeviceError& e) {
    return fail(scene, e.status(), e.what());
  } catch (const std::invalid_argument& e) {
    return fail(scene, AR_ERR_INVALID_ARGUMENT, std::string("invalid argument: ") + e.what());
  } catch (const std::bad_alloc&) {
    return fail(scene, AR_ERR_OUT_OF_MEMORY, "ar_session_create: out of host memory");
  } catch (const std::exception& e) {
    return fail(scene, AR_ERR_INTERNAL, e.what());
  } catch (...) {
    return fail(scene, AR_ERR_INTERNAL, "unknown exception");
  }
  });
}

AR_API ar_status ar_session_close(ar_session h) {
  AR_RECORD(ar_session_close, ar_rec_of(h), h);
  AR_RECORD_BODY({
  ar_status st = session_call(h, "ar_session_close", [](Session& s) { s.close(); });
  if (st == AR_OK) h->closed = true;
  return st;
  });
}

AR_API ar_status ar_session_destroy(ar_session h) {
  AR_RECORD(ar_session_destroy, ar_rec_of(h), h);
  AR_RECORD_BODY({
  if (!h) return AR_ERR_INVALID_ARGUMENT;
  try {
    delete h;  // the session first (close, join), then its device
    return AR_OK;
  } catch (...) {
    return AR_ERR_INTERNAL;
  }
  });
}

AR_API ar_status ar_session_start(ar_session h) {
  AR_RECORD(ar_session_start, ar_rec_of(h), h);
  AR_RECORD_BODY({ return session_call(h, "ar_session_start", [](Session& s) { s.start(); }); });
}
AR_API ar_status ar_session_pause(ar_session h) {
  AR_RECORD(ar_session_pause, ar_rec_of(h), h);
  AR_RECORD_BODY({ return session_call(h, "ar_session_pause", [](Session& s) { s.pause(); }); });
}
AR_API ar_status ar_session_resume(ar_session h) {
  AR_RECORD(ar_session_resume, ar_rec_of(h), h);
  AR_RECORD_BODY({ return session_call(h, "ar_session_resume", [](Session& s) { s.resume(); }); });
}
AR_API ar_status ar_session_cancel(ar_session h) {
  AR_RECORD(ar_session_cancel, ar_rec_of(h), h);
  AR_RECORD_BODY({ return session_call(h, "ar_session_cancel", [](Session& s) { s.cancel(); }); });
}

AR_API ar_status ar_session_set_view(ar_session h, const ar_view_desc* view) {
  AR_RECORD(ar_session_set_view, ar_rec_of(h), h, view);
  AR_RECORD_BODY({
  if (h && !view) return fail(h, AR_ERR_INVALID_ARGUMENT, "invalid argument: ar_session_set_view view is null");
  return session_call(h, "ar_session_set_view", [view](Session& s) { s.set_view(*view); });
  });
}

AR_API ar_status ar_session_set_display_mode(ar_session h, uint32_t mode) {
  AR_RECORD(ar_session_set_display_mode, ar_rec_of(h), h, mode);
  AR_RECORD_BODY({
  return session_call(h, "ar_session_set_display_mode", [mode](Session& s) { s.set_display_mode(mode); });
  });
}

// The named wait (0001 D2.6): returns AR_OK when the session reached CONVERGED, CLOSED or FAILED
// or the timeout passed; the caller reads the state with ar_session_get_progress (0007 d27).
AR_API ar_status ar_session_wait(ar_session h, uint32_t timeout_ms) {
  AR_RECORD(ar_session_wait, ar_rec_of(h), h, timeout_ms);
  AR_RECORD_BODY({
  if (!h || !h->session) return AR_ERR_INVALID_ARGUMENT;
  try {
    h->session->wait(timeout_ms);
    return AR_OK;
  } catch (const std::exception& e) {
    return fail(h, AR_ERR_INTERNAL, e.what());
  } catch (...) {
    return fail(h, AR_ERR_INTERNAL, "unknown exception");
  }
  });
}

// Progress is read after CLOSED too: everything below is the session's own or cached at detach.
// The ledger is the session's own (each CPU session owns its device and ledger). The CPU device
// has no device memory, so device_free_bytes and device_total_bytes stay 0 there; CUDA sessions
// fill them (102).
AR_API ar_status ar_session_get_progress(ar_session h, ar_progress* progress) {
  if (!h || !h->session) return AR_ERR_INVALID_ARGUMENT;
  if (!progress) return fail(h, AR_ERR_INVALID_ARGUMENT, "invalid argument: ar_session_get_progress output is null");
  try {
    const astroray::host::SessionProgress p = h->session->progress();
    *progress = ar_progress{};
    progress->state = p.state;
    progress->samples_per_pixel = p.samples_per_pixel;
    progress->active_generation = p.active_generation;
    progress->pending_generation = p.pending_generation;
    progress->failed_generation = p.failed_generation;
    progress->epoch = p.epoch;
    progress->last_pass_ms = p.last_pass_ms;
    static_assert(astroray::host::kLedgerCategoryCount <= 16, "ar_progress.ledger_bytes has 16 slots");
    if (h->device)
      for (uint32_t i = 0; i < astroray::host::kLedgerCategoryCount; ++i)
        progress->ledger_bytes[i] = h->device->ledger().category_total(static_cast<astroray::host::LedgerCategory>(i));
    return AR_OK;
  } catch (const std::exception& e) {
    return fail(h, AR_ERR_INTERNAL, e.what());
  } catch (...) {
    return fail(h, AR_ERR_INTERNAL, "unknown exception");
  }
}

// Report polling (0001 D2.11): the session's table merged with the scene's, kept readable after
// CLOSED because the final table is what a host prints.
AR_API ar_status ar_session_report_info(ar_session h, ar_report_info* info) {
  if (h && !info) return fail(h, AR_ERR_INVALID_ARGUMENT, "invalid argument: ar_session_report_info output is null");
  return session_call(h, "ar_session_report_info", [info](Session& s) { *info = s.report_snapshot(); }, true);
}

AR_API ar_status ar_session_report_entry(ar_session h, uint32_t index, ar_report_entry* entry) {
  if (h && !entry) return fail(h, AR_ERR_INVALID_ARGUMENT, "invalid argument: ar_session_report_entry output is null");
  return session_call(h, "ar_session_report_entry", [index, entry](Session& s) { s.report_entry(index, entry); }, true);
}

// Display frames (0007 d29). Never waits for a pass. frame_id 0 in the output means no frame newer
// than after_frame. A frame stays readable after close, until the session is destroyed.
AR_API ar_status ar_display_acquire(ar_session h, uint64_t after_frame, ar_display_frame* out) {
  if (h && !out) return fail(h, AR_ERR_INVALID_ARGUMENT, "invalid argument: ar_display_acquire output is null");
  return session_call(h, "ar_display_acquire", [after_frame, out](Session& s) {
    *out = ar_display_frame{};
    const auto f = s.display_acquire(after_frame);
    if (!f) return;
    out->frame_id = f->frame_id;
    out->generation = f->generation;
    out->width = f->width;
    out->height = f->height;
    out->format = f->format == astroray::host::DisplayFormat::RGBA16F ? AR_DISPLAY_FORMAT_RGBA16F
                                                                      : AR_DISPLAY_FORMAT_DIAG_RGBA8;
    out->row_bytes = f->row_bytes;
    out->data = static_cast<uint8_t*>(const_cast<void*>(f->data));
  });
}

AR_API ar_status ar_display_release(ar_session h, uint64_t frame_id) {
  // Allowed after close: the ring lives until destroy, so a frame acquired earlier can be released.
  return session_call(h, "ar_display_release", [frame_id](Session& s) { s.display_release(frame_id); }, true);
}

// Passes (0008 d13a). pass_count and pass_info read only the layout and the latched size. Indices
// belong to the pass-list revision pass_count last reported on this handle: a film rebuilt since
// can shift every index after Combined, so pass_info and read_pass refuse another (ruling 75).

AR_API ar_status ar_session_pass_count(ar_session h, uint32_t* count) {
  if (h && !count) return fail(h, AR_ERR_INVALID_ARGUMENT, "invalid argument: ar_session_pass_count output is null");
  return session_call(h, "ar_session_pass_count", [h, count](Session& s) {
    uint64_t revision = 0;
    const auto passes = astroray::host::list_passes(s.layout(&revision));
    *count = uint32_t(passes.size());
    h->pass_revision = revision;
  });
}

AR_API ar_status ar_session_pass_info(ar_session h, uint32_t index, ar_pass_info* info) {
  if (h && !info) return fail(h, AR_ERR_INVALID_ARGUMENT, "invalid argument: ar_session_pass_info output is null");
  return session_call(h, "ar_session_pass_info", [h, index, info](Session& s) {
    uint64_t revision = 0;
    const auto passes = astroray::host::list_passes(s.layout(&revision));
    check_pass_revision(h, revision);
    if (index >= passes.size())
      throw std::invalid_argument("pass index " + std::to_string(index) + " is past the " +
                                  std::to_string(passes.size()) + " passes");
    const astroray::host::PassDesc& p = passes[index];
    if (p.name.size() >= sizeof(info->name))
      throw std::invalid_argument("pass name '" + p.name + "' does not fit ar_pass_info.name");
    *info = ar_pass_info{};
    memcpy(info->name, p.name.data(), p.name.size());  // names are shorter than the 64-byte field
    info->component_count = p.components;
    info->unit = p.unit;
    s.render_size(&info->width, &info->height);
    info->flags = 0;  // AR_PASS_DENOISED only once a denoiser exists
  });
}

// Waits up to one pass (the render lock): for the F12 job thread, never Blender's main thread
// (0007 d23). `n` must be exactly the floats the quantity needs (see readback.h).
AR_API ar_status ar_session_read_pass(ar_session h, uint32_t index, uint32_t quantity, uint32_t n, float* dst) {
  if (h && !dst) return fail(h, AR_ERR_INVALID_ARGUMENT, "invalid argument: ar_session_read_pass dst is null");
  return session_call(h, "ar_session_read_pass", [=](Session& s) {
    // The list entry and the film come from one render-locked state: the film can be rebuilt for a
    // changed pass request between two calls (ruling 74).
    std::vector<astroray::host::PassDesc> passes;
    std::unique_ptr<astroray::host::FilmView> copy;
    s.read_film([&](const FilmReadContext& c) {
      check_pass_revision(h, c.pass_revision);
      passes = astroray::host::list_passes(c.film.layout());
      if (index < passes.size()) copy = std::make_unique<astroray::host::FilmView>(c.device, c.film);
    });
    if (index >= passes.size())
      throw std::invalid_argument("pass index " + std::to_string(index) + " is past the " +
                                  std::to_string(passes.size()) + " passes");
    astroray::host::read_pass(*copy, passes[index], quantity, n, dst);
  });
}

// The counters of the last resolved pass (0004 D29). `n` must be at least the number of words
// (the error text names it); words past it are zeroed.
AR_API ar_status ar_session_read_counters(ar_session h, uint32_t n, uint64_t* dst) {
  if (h && !dst) return fail(h, AR_ERR_INVALID_ARGUMENT, "invalid argument: ar_session_read_counters dst is null");
  return session_call(h, "ar_session_read_counters", [n, dst](Session& s) {
    const std::vector<uint64_t> w = astroray::host::counter_words(s.pass_counters());
    if (n < w.size())
      throw std::invalid_argument("read_counters: the buffer holds " + std::to_string(n) + " words, the counters need " +
                                  std::to_string(w.size()));
    for (uint32_t i = 0; i < n; ++i) dst[i] = i < w.size() ? w[i] : 0;
  });
}

// Waits up to one pass (the render lock), like ar_session_read_pass: the F12 job thread only.
// AR_FITS_DUMP writes every channel the film has (the layout holds only requested ones);
// AR_FITS_SCIENCE is not designed yet and is refused.
AR_API ar_status ar_session_write_fits(ar_session h, const char* path, uint32_t what) {
  if (h && !path) return fail(h, AR_ERR_INVALID_ARGUMENT, "invalid argument: ar_session_write_fits path is null");
  return session_call(h, "ar_session_write_fits", [path, what](Session& s) {
    if (what == AR_FITS_SCIENCE) throw SessionUnsupported("ar_session_write_fits: the science file is not supported yet");
    if (what != AR_FITS_DUMP) throw std::invalid_argument("ar_session_write_fits: unknown file kind " + std::to_string(what));
    std::unique_ptr<astroray::host::FilmView> view;
    astroray::host::FitsProvenance pv;
    s.read_film([&](const FilmReadContext& c) {
      view = std::make_unique<astroray::host::FilmView>(c.device, c.film);
      pv = c.provenance;
    });
    astroray::host::write_fits_film_dump(path, *view, pv);
  });
}

AR_API ar_status ar_session_last_error(ar_session h, uint32_t capacity, uint8_t* text) {
  if (!h) return AR_ERR_INVALID_ARGUMENT;
  if (capacity > 0 && !text) return fail(h, AR_ERR_INVALID_ARGUMENT, "invalid argument: last_error text buffer is null");
  try {
    std::string why = h->last_error;
    // A render-thread failure is the session's error too.
    if (why.empty() && h->session) why = h->session->failure();
    copy_text(why, capacity, text);
    return AR_OK;
  } catch (...) {
    return AR_ERR_OUT_OF_MEMORY;
  }
}

}  // extern "C"
