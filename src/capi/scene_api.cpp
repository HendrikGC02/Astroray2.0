// SPDX-License-Identifier: Apache-2.0
// Scenes and scene edits (0007 decisions 4-6, 9, 23-25; 0001 D2.5, D2.8).
// Ref: 0007 decision 5 (function list), 0001 D2.5 (guarded entry points).
#include <string.h>

#include <exception>
#include <memory>
#include <string>

#include "astroray/astroray.h"
#include "host/scene/edit.h"
#include "host/device.h"

using astroray::host::Edit;
using astroray::host::Scene;

namespace {

#ifdef ASTRORAY_GPU
// Everything the CUDA init thread creates, kept alive by the scene.
struct CudaInit {
  astroray::host::Ledger ledger;
  std::unique_ptr<astroray::host::ModuleDevice> device;
  explicit CudaInit(uint32_t index) : device(astroray::host::make_cuda_device(ledger, index)) {}
};
#endif

ar_status fail_ctx(ar_context ctx, ar_status st, const std::string& why) {
  try {
    astroray::context_set_error(ctx, why);
    astroray::context_state(ctx).log.line(st == AR_ERR_INTERNAL ? "ERROR" : "WARNING", why);
  } catch (...) {
    // Out of memory while reporting: the status is all that is left.
  }
  return st;
}

template <class H>
ar_status fail(H* h, ar_status st, const std::string& why) {
  try {
    h->last_error = why;
  } catch (...) {
  }
  return st;
}

void copy_text(const std::string& s, uint32_t capacity, uint8_t* text) {
  if (capacity == 0) return;
  size_t n = s.size() < capacity - 1 ? s.size() : capacity - 1;
  memcpy(text, s.data(), n);
  text[n] = 0;
}

// Runs an edit call; a non-OK status copies the edit's reason to the handle, a throw
// becomes AR_ERR_INTERNAL.
template <class F>
ar_status edit_call(ar_edit h, F&& f) {
  if (!h || !h->edit) return AR_ERR_INVALID_ARGUMENT;
  try {
    ar_status st = f(*h->edit);
    if (st != AR_OK) h->last_error = h->edit->last_error();
    return st;
  } catch (const std::exception& e) {
    return fail(h, AR_ERR_INTERNAL, e.what());
  } catch (...) {
    return fail(h, AR_ERR_INTERNAL, "unknown exception");
  }
}

}  // namespace

extern "C" {

AR_API ar_status ar_scene_create(ar_context ctx, const ar_scene_desc* desc, ar_scene* out) {
  if (!ctx) return AR_ERR_INVALID_ARGUMENT;
  try {
    if (astroray::context_state(ctx).poisoned.load(std::memory_order_acquire))
      return fail_ctx(ctx, AR_ERR_INTERNAL, "scene create: the context is poisoned by an engine thread fault");
    if (!out) return fail_ctx(ctx, AR_ERR_INVALID_ARGUMENT, "invalid argument: ar_scene_create output is null");
    *out = nullptr;
    if (!desc) return fail_ctx(ctx, AR_ERR_INVALID_ARGUMENT, "invalid argument: ar_scene_create desc is null");
    // A sanity cap against a garbage value spawning thousands of threads, not a physical limit.
    if (desc->worker_threads > 256) return fail_ctx(ctx, AR_ERR_INVALID_ARGUMENT, "invalid argument: worker_threads above 256");
    astroray::host::SceneConfig cfg;
    cfg.workers = desc->worker_threads;
    uint32_t kind = 0, index = 0;
    astroray::context_device(ctx, &kind, &index);
    if (kind == AR_DEVICE_CUDA) {
#ifdef ASTRORAY_GPU
      cfg.init = [index]() -> std::shared_ptr<void> { return std::make_shared<CudaInit>(index); };
#else
      cfg.init = []() -> std::shared_ptr<void> { throw std::runtime_error("this build has no CUDA backend"); };
#endif
    }
    auto h = std::make_unique<ar_scene_s>();
    h->scene = std::make_unique<Scene>(astroray::context_state(ctx), std::move(cfg));
    *out = h.release();
    return AR_OK;
  } catch (const std::exception& e) {
    return fail_ctx(ctx, AR_ERR_INTERNAL, e.what());
  } catch (...) {
    return fail_ctx(ctx, AR_ERR_INTERNAL, "unknown exception");
  }
}

AR_API ar_status ar_scene_destroy(ar_scene h) {
  if (!h) return AR_ERR_INVALID_ARGUMENT;
  try {
    delete h;  // the open edit first (aborted), then the scene (joins its threads)
    return AR_OK;
  } catch (...) {
    return AR_ERR_INTERNAL;
  }
}

AR_API ar_status ar_scene_last_error(ar_scene h, uint32_t capacity, uint8_t* text) {
  if (!h) return AR_ERR_INVALID_ARGUMENT;
  if (capacity > 0 && !text) return fail(h, AR_ERR_INVALID_ARGUMENT, "invalid argument: last_error text buffer is null");
  copy_text(h->last_error, capacity, text);
  return AR_OK;
}

AR_API ar_status ar_scene_edit_begin(ar_scene h, ar_edit* out) {
  if (!h) return AR_ERR_INVALID_ARGUMENT;
  try {
    if (!out) return fail(h, AR_ERR_INVALID_ARGUMENT, "invalid argument: ar_scene_edit_begin output is null");
    *out = nullptr;
    std::unique_ptr<Edit> e = h->scene->begin_edit();
    if (!e) return fail(h, AR_ERR_INVALID_ARGUMENT, "invalid argument: the scene already has an open edit");
    h->edit = std::make_unique<ar_edit_s>();
    h->edit->owner = h;
    h->edit->edit = std::move(e);
    *out = h->edit.get();
    return AR_OK;
  } catch (const std::exception& e) {
    return fail(h, AR_ERR_INTERNAL, e.what());
  } catch (...) {
    return fail(h, AR_ERR_INTERNAL, "unknown exception");
  }
}

AR_API ar_status ar_edit_reset(ar_edit h) {
  return edit_call(h, [](Edit& e) { return e.reset(); });
}
AR_API ar_status ar_edit_item_commit(ar_edit h, ar_key key) {
  return edit_call(h, [&](Edit& e) { return e.item_commit(key); });
}
AR_API ar_status ar_edit_remove(ar_edit h, uint32_t kind, ar_key key) {
  return edit_call(h, [&](Edit& e) { return e.remove(kind, key); });
}
AR_API ar_status ar_edit_set_settings(ar_edit h, const ar_settings* s) {
  return edit_call(h, [&](Edit& e) { return e.set_settings(s); });
}
AR_API ar_status ar_edit_set_camera(ar_edit h, const ar_camera_desc* c) {
  return edit_call(h, [&](Edit& e) { return e.set_camera(c); });
}
AR_API ar_status ar_edit_set_astro(ar_edit h, ar_key key, const ar_astro_desc* a) {
  return edit_call(h, [&](Edit& e) { return e.set_astro(key, a); });
}

// A committed or aborted edit is destroyed, which frees the scene's one edit slot; its
// handle must not be used again.
AR_API ar_status ar_edit_commit(ar_edit h, uint64_t* generation) {
  ar_scene owner = h ? h->owner : nullptr;
  ar_status st = edit_call(h, [&](Edit& e) { return e.commit(generation); });
  if (st == AR_OK) owner->edit.reset();
  return st;
}
AR_API ar_status ar_edit_abort(ar_edit h) {
  ar_scene owner = h ? h->owner : nullptr;
  ar_status st = edit_call(h, [](Edit& e) { return e.abort(); });
  if (st == AR_OK) owner->edit.reset();
  return st;
}

AR_API ar_status ar_edit_last_error(ar_edit h, uint32_t capacity, uint8_t* text) {
  if (!h) return AR_ERR_INVALID_ARGUMENT;
  if (capacity > 0 && !text) return fail(h, AR_ERR_INVALID_ARGUMENT, "invalid argument: last_error text buffer is null");
  copy_text(h->last_error, capacity, text);
  return AR_OK;
}

}  // extern "C"
