// SPDX-License-Identifier: Apache-2.0
// Scenes and scene edits (0007 decisions 4-6, 9, 23-25; 0001 D2.5, D2.8).
// Ref: 0007 decision 5 (function list), 0001 D2.5 (guarded entry points).
#include <string.h>

#include <exception>
#include <functional>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>

#include "astroray/astroray.h"
#include "host/light/lamps.h"
#include "host/material/material_store.h"
#include "host/scene/edit.h"
#include "host/scene/mesh.h"
#include "host/device.h"
#include "host/scene/accel.h"
#include "recorder_hooks.h"

using astroray::host::Edit;
using astroray::host::Scene;

namespace {

#ifdef ASTRORAY_GPU
// What the CUDA init thread creates, kept alive by the scene: the device charges the scene's
// ledger, the one its host arrays count in (RULINGS 70).
struct CudaInit {
  std::shared_ptr<astroray::host::Ledger> ledger;  // outlives the device
  std::unique_ptr<astroray::host::ModuleDevice> device;
  std::unique_ptr<astroray::host::AccelBuilder> accel;  // after the device it uses
};
#endif

// The scene's device init for a CUDA context. Kept out of the entry point because the
// recording macro wraps that body and a preprocessor conditional cannot sit inside it.
// `cache_dir` is ar_scene_desc.cache_dir (0007 d45), empty for none: the OptiX disk cache is then off.
void cuda_init(astroray::host::SceneConfig& cfg, uint32_t index, std::shared_ptr<astroray::host::Ledger> ledger,
               const std::string& cache_dir) {
#ifdef ASTRORAY_GPU
  cfg.init = [index, ledger, cache_dir]() -> std::shared_ptr<void> {
    auto held = std::make_shared<CudaInit>();
    held->ledger = ledger;
    held->device = astroray::host::make_cuda_device(*ledger, index);
    held->accel = astroray::host::make_optix_accel_builder(*held->device, index, cache_dir);
    return held;
  };
  cfg.device_of = [](void* p) -> astroray::host::Device* { return static_cast<CudaInit*>(p)->device.get(); };
  cfg.accel_of = [](void* p) -> astroray::host::AccelBuilder* { return static_cast<CudaInit*>(p)->accel.get(); };
#else
  (void)index, (void)ledger, (void)cache_dir;
  cfg.init = []() -> std::shared_ptr<void> { throw std::runtime_error("this build has no CUDA backend"); };
#endif
}

// The last error of a destroyed scene handle, which the context keeps until it is destroyed.
const char* const kDestroyed = "invalid argument: the scene handle was destroyed (ar_scene_destroy)";

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

// A staging call that throws std::invalid_argument for bad input: that is the caller's
// error, reported like Edit's own checks (status, last_error, a warning line).
template <class F>
ar_status staging_call(ar_edit h, F&& f) {
  if (!h || !h->edit) return AR_ERR_INVALID_ARGUMENT;
  try {
    ar_status st = f(*h->edit);
    if (st != AR_OK) h->last_error = h->edit->last_error();
    return st;
  } catch (const std::invalid_argument& e) {
    std::string why = std::string("invalid argument: ") + e.what();
    h->owner->scene->ctx().log.line("WARNING", why);
    return fail(h, AR_ERR_INVALID_ARGUMENT, why);
  } catch (const std::bad_alloc&) {
    return fail(h, AR_ERR_OUT_OF_MEMORY, "out of host memory staging geometry");
  } catch (const std::exception& e) {
    return fail(h, AR_ERR_INTERNAL, e.what());
  } catch (...) {
    return fail(h, AR_ERR_INTERNAL, "unknown exception");
  }
}

}  // namespace

ar_recorder* ar_rec_of(ar_scene h) { return h && h->scene ? ar_rec_of_state(h->scene->ctx()) : nullptr; }
ar_recorder* ar_rec_of(ar_edit h) { return h && h->owner ? ar_rec_of(h->owner) : nullptr; }

extern "C" {

AR_API ar_status ar_scene_create(ar_context ctx, const ar_scene_desc* desc, ar_scene* out) {
  AR_RECORD(ar_scene_create, ar_rec_of(ctx), ctx, desc, out);
  AR_RECORD_BODY({
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
    // The mesh store (and with it the scene's ledger) exists before the init thread charges it.
    auto meshes = std::make_shared<astroray::host::MeshStore>();
    if (kind == AR_DEVICE_CUDA) {
      const std::string cache_dir = desc->cache_dir ? desc->cache_dir : "";
      if (cache_dir.empty())
        astroray::context_state(ctx).log.line(
            "INFO", "scene create: ar_scene_desc.cache_dir is empty, so the OptiX disk cache is disabled");
      cuda_init(cfg, index, meshes->shared_ledger(), cache_dir);
    }
    auto h = std::make_unique<ar_scene_s>();
    h->scene = std::make_unique<Scene>(astroray::context_state(ctx), std::move(cfg));
    h->scene->meshes = std::move(meshes);
    // Material tables count in the scene's host ledger, beside its geometry.
    h->scene->materials = std::make_shared<astroray::host::MaterialStore>(h->scene->meshes->shared_ledger());
    *out = h.release();
    AR_RECORD_BIND(*out);
    return AR_OK;
  } catch (const std::exception& e) {
    return fail_ctx(ctx, AR_ERR_INTERNAL, e.what());
  } catch (...) {
    return fail_ctx(ctx, AR_ERR_INTERNAL, "unknown exception");
  }
  });
}

AR_API ar_status ar_scene_destroy(ar_scene h) {
  AR_RECORD(ar_scene_destroy, ar_rec_of(h), h);
  AR_RECORD_BODY({
  if (!h) return AR_ERR_INVALID_ARGUMENT;
  try {
    if (!h->scene) return fail(h, AR_ERR_INVALID_ARGUMENT, kDestroyed);
    // A session that is not CLOSED still holds the scene (its activation listener); destroying the
    // scene under it is a host error (0007 d5, d23: close, wait for CLOSED, then destroy).
    if (h->scene->generations().listener_count() != 0)
      return fail(h, AR_ERR_INVALID_ARGUMENT, "ar_scene_destroy: a session of this scene is not CLOSED; close it and wait for CLOSED first");
    // Freeing joins the scene's threads and frees device and host memory (~110 ms on B-5M), so the
    // context's reaper does it and this returns within 0009 B-21 (0007 d23). The handle is dead now.
    astroray::context_retire_scene(h->scene->ctx(), h);
    return AR_OK;
  } catch (const std::exception& e) {
    return fail(h, AR_ERR_INTERNAL, e.what());
  } catch (...) {
    return AR_ERR_INTERNAL;
  }
  });
}

AR_API ar_status ar_scene_last_error(ar_scene h, uint32_t capacity, uint8_t* text) {
  if (!h) return AR_ERR_INVALID_ARGUMENT;
  if (capacity > 0 && !text) return fail(h, AR_ERR_INVALID_ARGUMENT, "invalid argument: last_error text buffer is null");
  copy_text(h->last_error, capacity, text);
  return AR_OK;
}

AR_API ar_status ar_scene_edit_begin(ar_scene h, ar_edit* out) {
  AR_RECORD(ar_scene_edit_begin, ar_rec_of(h), h, out);
  AR_RECORD_BODY({
  if (!h) return AR_ERR_INVALID_ARGUMENT;
  try {
    if (!out) return fail(h, AR_ERR_INVALID_ARGUMENT, "invalid argument: ar_scene_edit_begin output is null");
    *out = nullptr;
    if (!h->scene) return fail(h, AR_ERR_INVALID_ARGUMENT, kDestroyed);
    std::unique_ptr<Edit> e = h->scene->begin_edit();
    if (!e) return fail(h, AR_ERR_INVALID_ARGUMENT, "invalid argument: the scene already has an open edit");
    h->edit = std::make_unique<ar_edit_s>();
    h->edit->owner = h;
    h->edit->edit = std::move(e);
    *out = h->edit.get();
    AR_RECORD_BIND(*out);
    return AR_OK;
  } catch (const std::exception& e) {
    return fail(h, AR_ERR_INTERNAL, e.what());
  } catch (...) {
    return fail(h, AR_ERR_INTERNAL, "unknown exception");
  }
  });
}

AR_API ar_status ar_edit_reset(ar_edit h) {
  AR_RECORD(ar_edit_reset, ar_rec_of(h), h);
  AR_RECORD_BODY({
  return edit_call(h, [h](Edit& e) {
    ar_status st = e.reset();
    return st == AR_OK ? h->owner->scene->materials->note_reset(e) : st;
  });
  });
}
AR_API ar_status ar_edit_mesh_begin(ar_edit h, const ar_mesh_desc* desc, ar_mesh_buffers* out) {
  AR_RECORD(ar_edit_mesh_begin, ar_rec_of(h), h, desc, out);
  AR_RECORD_BODY({
  ar_status st = staging_call(h, [&](Edit& e) {
    return astroray::host::mesh_begin(*h->owner->scene, e, desc, out);
  });
  if (st == AR_OK) AR_RECORD_POST(ar_edit_mesh_begin, h, desc, out);
  return st;
  });
}
AR_API ar_status ar_edit_positions_begin(ar_edit h, ar_key key, ar_positions_buffers* out) {
  AR_RECORD(ar_edit_positions_begin, ar_rec_of(h), h, key, out);
  AR_RECORD_BODY({
  ar_status st = staging_call(h, [&](Edit& e) {
    return astroray::host::positions_begin(*h->owner->scene, e, key, out);
  });
  if (st == AR_OK) AR_RECORD_POST(ar_edit_positions_begin, h, key, out);
  return st;
  });
}
AR_API ar_status ar_edit_item_commit(ar_edit h, ar_key key) {
  AR_RECORD(ar_edit_item_commit, ar_rec_of(h), h, key);
  AR_RECORD_BODY({
  return edit_call(h, [&](Edit& e) { return e.item_commit(key); });
  });
}
AR_API ar_status ar_edit_remove(ar_edit h, uint32_t kind, ar_key key) {
  AR_RECORD(ar_edit_remove, ar_rec_of(h), h, kind, key);
  AR_RECORD_BODY({
  return edit_call(h, [&](Edit& e) {
    ar_status st = e.remove(kind, key);
    return st == AR_OK ? h->owner->scene->materials->note_remove(e, kind, key) : st;
  });
  });
}
AR_API ar_status ar_edit_set_settings(ar_edit h, const ar_settings* s) {
  AR_RECORD(ar_edit_set_settings, ar_rec_of(h), h, s);
  AR_RECORD_BODY({
  return edit_call(h, [&](Edit& e) { return e.set_settings(s); });
  });
}
AR_API ar_status ar_edit_set_camera(ar_edit h, const ar_camera_desc* c) {
  AR_RECORD(ar_edit_set_camera, ar_rec_of(h), h, c);
  AR_RECORD_BODY({
  return edit_call(h, [&](Edit& e) { return e.set_camera(c); });
  });
}
AR_API ar_status ar_edit_set_astro(ar_edit h, ar_key key, const ar_astro_desc* a) {
  AR_RECORD(ar_edit_set_astro, ar_rec_of(h), h, key, a);
  AR_RECORD_BODY({
  return edit_call(h, [&](Edit& e) { return e.set_astro(key, a); });
  });
}
// Graphs cross as flat arrays and compile on a worker at commit (0007 d8); only SURFACE graphs
// and WORLD graphs compile in M1a (the world's in the material pass), the other targets refuse loudly. Each call stages and commits its own item.
AR_API ar_status ar_edit_set_graph(ar_edit h, ar_key key, uint32_t target, const ar_graph_desc* desc) {
  AR_RECORD(ar_edit_set_graph, ar_rec_of(h), h, key, target, desc);
  AR_RECORD_BODY({
  if (h && h->edit && target != AR_TARGET_SURFACE && target != AR_TARGET_WORLD) {
    // An enum range error is the caller's (0007 d9); a declared target without a compiler yet
    // is this build's. Both are logged like Edit's own refusals.
    const bool known = target <= AR_TARGET_WORLD;
    const ar_status st = known ? AR_ERR_UNSUPPORTED : AR_ERR_INVALID_ARGUMENT;
    try {
      const std::string why = known ? "ar_edit_set_graph: target " + std::to_string(target) + " is not implemented in this build"
                                    : "invalid argument: ar_edit_set_graph target " + std::to_string(target) + " out of range";
      h->owner->scene->ctx().log.line("WARNING", why);
      return fail(h, st, why);
    } catch (...) {
      return st;
    }
  }
  return staging_call(h, [&](Edit& e) { return h->owner->scene->materials->set_graph(e, key, desc); });
  });
}
// Point, sphere, rectangle and disk lamps; SPOT and SUN land in M4.
AR_API ar_status ar_edit_set_light(ar_edit h, ar_key key, const ar_light_desc* desc) {
  AR_RECORD(ar_edit_set_light, ar_rec_of(h), h, key, desc);
  AR_RECORD_BODY({
  if (h && h->edit && desc && (desc->type == AR_LIGHT_SPOT || desc->type == AR_LIGHT_SUN)) {
    try {
      const std::string why = "ar_edit_set_light: spot and sun lights are not implemented in this build (M4)";
      h->owner->scene->ctx().log.line("WARNING", why);
      return fail(h, AR_ERR_UNSUPPORTED, why);
    } catch (...) {
      return AR_ERR_UNSUPPORTED;
    }
  }
  return staging_call(h, [&](Edit& e) {
    return astroray::host::set_light(e, key, desc, h->owner->scene->materials->table());
  });
  });
}
AR_API ar_status ar_edit_set_world(ar_edit h, const ar_world_desc* desc) {
  AR_RECORD(ar_edit_set_world, ar_rec_of(h), h, desc);
  AR_RECORD_BODY({
  return edit_call(h, [&](Edit& e) { return h->owner->scene->materials->set_world(e, desc); });
  });
}
AR_API ar_status ar_edit_set_material(ar_edit h, ar_key key, const ar_material_desc* desc) {
  AR_RECORD(ar_edit_set_material, ar_rec_of(h), h, key, desc);
  AR_RECORD_BODY({
  return staging_call(h, [&](Edit& e) { return h->owner->scene->materials->set_material(e, key, desc); });
  });
}
// FILE and MEMORY images decode on a worker at item commit; PIXELS (float data) is M1b.
AR_API ar_status ar_edit_set_image(ar_edit h, const ar_image_desc* desc) {
  AR_RECORD(ar_edit_set_image, ar_rec_of(h), h, desc);
  AR_RECORD_BODY({
  if (h && h->edit && desc && desc->source == AR_IMAGE_SOURCE_PIXELS) {
    try {
      const std::string why = "ar_edit_set_image: PIXELS images are not implemented in this build (M1b)";
      h->owner->scene->ctx().log.line("WARNING", why);
      return fail(h, AR_ERR_UNSUPPORTED, why);
    } catch (...) {
      return AR_ERR_UNSUPPORTED;
    }
  }
  return staging_call(h, [&](Edit& e) { return h->owner->scene->materials->set_image(e, desc); });
  });
}

// A committed or aborted edit is destroyed, which frees the scene's one edit slot; its
// handle must not be used again.
AR_API ar_status ar_edit_commit(ar_edit h, uint64_t* generation) {
  AR_RECORD(ar_edit_commit, ar_rec_of(h), h, generation);
  AR_RECORD_BODY({
  ar_scene owner = h ? h->owner : nullptr;
  ar_status st = edit_call(h, [&](Edit& e) { return e.commit(generation); });
  if (st == AR_OK) owner->scene->materials->end_edit(), owner->edit.reset();
  return st;
  });
}
AR_API ar_status ar_edit_abort(ar_edit h) {
  AR_RECORD(ar_edit_abort, ar_rec_of(h), h);
  AR_RECORD_BODY({
  ar_scene owner = h ? h->owner : nullptr;
  ar_status st = edit_call(h, [](Edit& e) { return e.abort(); });
  if (st == AR_OK) owner->scene->materials->end_edit(), owner->edit.reset();
  return st;
  });
}

AR_API ar_status ar_edit_last_error(ar_edit h, uint32_t capacity, uint8_t* text) {
  if (!h) return AR_ERR_INVALID_ARGUMENT;
  if (capacity > 0 && !text) return fail(h, AR_ERR_INVALID_ARGUMENT, "invalid argument: last_error text buffer is null");
  copy_text(h->last_error, capacity, text);
  return AR_OK;
}

}  // extern "C"
