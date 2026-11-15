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
#include "host/scene/edit.h"
#include "host/session/session.h"
#include "recorder_hooks.h"

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
ar_status session_call(ar_session h, const char* function, F&& f) {
  if (!h || !h->session) return AR_ERR_INVALID_ARGUMENT;
  try {
    if (h->closed) return fail(h, AR_ERR_INVALID_ARGUMENT, std::string(function) + ": the session is closed");
    f(*h->session);
    return AR_OK;
  } catch (const SessionUnsupported& e) {
    return fail(h, AR_ERR_UNSUPPORTED, e.what());
  } catch (const std::invalid_argument& e) {
    return fail(h, AR_ERR_INVALID_ARGUMENT, std::string("invalid argument: ") + e.what());
  } catch (const std::bad_alloc&) {
    return fail(h, AR_ERR_OUT_OF_MEMORY, std::string(function) + ": out of host memory");
  } catch (const std::exception& e) {
    return fail(h, AR_ERR_INTERNAL, e.what());
  } catch (...) {
    return fail(h, AR_ERR_INTERNAL, "unknown exception");
  }
}

}  // namespace

ar_recorder* ar_rec_of(ar_session h) { return h ? h->recorder : nullptr; }

extern "C" {

AR_API ar_status ar_session_create(ar_scene scene, const ar_session_desc* desc, ar_session* out) {
  AR_RECORD(ar_session_create, ar_rec_of(scene), scene, desc, out);
  AR_RECORD_BODY({
  if (!scene || !scene->scene) return AR_ERR_INVALID_ARGUMENT;
  try {
    astroray::ContextState& ctx = scene->scene->ctx();
    if (ctx.poisoned.load(std::memory_order_acquire))
      return fail(scene, AR_ERR_INTERNAL, "session create: the context is poisoned by an engine thread fault");
    if (!out) return fail(scene, AR_ERR_INVALID_ARGUMENT, "invalid argument: ar_session_create output is null");
    *out = nullptr;
    if (!desc) return fail(scene, AR_ERR_INVALID_ARGUMENT, "invalid argument: ar_session_create desc is null");
    uint32_t kind = 0, index = 0;
    astroray::context_device_of_state(ctx, &kind, &index);
    if (kind != AR_DEVICE_CPU)
      return fail(scene, AR_ERR_UNSUPPORTED, "ar_session_create: CUDA sessions are not supported yet");
    auto h = std::make_unique<ar_session_s>();
    h->recorder = ar_rec_of(scene);
    const unsigned cores = std::thread::hardware_concurrency();
    std::shared_ptr<astroray::host::Device> device = astroray::host::make_cpu_session_device(ctx, cores ? cores : 1u);
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

// State, generations and epoch only; the remaining fields stay zero until session readback.
AR_API ar_status ar_session_get_progress(ar_session h, ar_progress* progress) {
  if (!h || !h->session) return AR_ERR_INVALID_ARGUMENT;
  if (!progress) return fail(h, AR_ERR_INVALID_ARGUMENT, "invalid argument: ar_session_get_progress output is null");
  try {
    const astroray::host::SessionProgress p = h->session->progress();
    *progress = ar_progress{};
    progress->state = p.state;
    progress->active_generation = p.active_generation;
    progress->pending_generation = p.pending_generation;
    progress->failed_generation = p.failed_generation;
    progress->epoch = p.epoch;
    return AR_OK;
  } catch (const std::exception& e) {
    return fail(h, AR_ERR_INTERNAL, e.what());
  } catch (...) {
    return fail(h, AR_ERR_INTERNAL, "unknown exception");
  }
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
