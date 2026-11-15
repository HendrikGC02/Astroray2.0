// SPDX-License-Identifier: Apache-2.0
// Devices and contexts (0001 D2.1, D2.5, D2.8).
#include <stdlib.h>
#include <string.h>

#include <exception>
#include <memory>
#include <string>

#include "astroray/astroray.h"
#include "host/build_info.h"
#include "host/device_probe.h"
#include "host/fault.h"
#include "host/log.h"
#include "host/scene/scene.h"
#include "recorder_hooks.h"

struct ar_context_s : astroray::ContextState {
  std::string last_error;
  std::string record_path;  // the effective recording path, empty when off
  ar_recorder* recorder = nullptr;  // owned; null when recording is off
  uint32_t kind = 0;
  uint32_t index = 0;
  ~ar_context_s() { ar_rec_close(recorder, nullptr); }
};

namespace {

// The build record the recording header carries (0001 D2.7).
ar_build_record build_record() {
  ar_build_record r;
  ar_build_info(&r);
  return r;
}

// The recorder a context creation opens: the descriptor's path, else ASTRORAY_RECORD
// (Ref: 0009 d19). It records the creation call itself, so it exists before the call is
// validated; the holder closes it unless the new context took it over.
struct RecHolder {
  ar_recorder* r = nullptr;
  std::string path;
  std::string err;
  explicit RecHolder(const ar_context_desc* desc) {
    if (!desc) return;
    if (desc->record_path && desc->record_path[0]) {
      path = desc->record_path;
    } else {
      char* env = nullptr;
      size_t len = 0;
      if (_dupenv_s(&env, &len, "ASTRORAY_RECORD") == 0 && env) {
        path = env;
        free(env);
      }
    }
    if (!path.empty()) r = ar_rec_open(path.c_str(), build_record(), &err);
  }
  ~RecHolder() { ar_rec_close(r, nullptr); }
  RecHolder(const RecHolder&) = delete;
  RecHolder& operator=(const RecHolder&) = delete;
};

// Stores the message as the handle's last error (ctx may be null: no handle yet).
ar_status fail_internal(ar_context ctx, const char* what) {
  if (ctx) {
    try {
      ctx->last_error = what;
      ctx->log.line("ERROR", std::string("internal error: ") + what);
    } catch (...) {
      // Out of memory while reporting: the status is all that is left.
    }
  }
  return AR_ERR_INTERNAL;
}

// Copies s into a fixed byte array, NUL-terminated, truncated to fit.
template <size_t N>
void put(uint8_t (&dst)[N], const std::string& s) {
  memset(dst, 0, N);
  memcpy(dst, s.data(), s.size() < N - 1 ? s.size() : N - 1);
}

// A rejected argument on a live handle: the text is kept as its last error.
ar_status reject(ar_context ctx, const char* why) {
  ctx->last_error = why;
  ctx->log.line("WARNING", why);
  return AR_ERR_INVALID_ARGUMENT;
}

// True when a fault poisoned the context (ctx may be null: no handle); the fixed
// message becomes its last error.
bool refuse_if_poisoned(ar_context ctx) {
  if (!ctx || !ctx->poisoned.load(std::memory_order_acquire)) return false;
  ctx->last_error = ctx->poison_message;
  return true;
}

}  // namespace

// Every exported body runs inside this: a poisoned context refuses the call, and a
// C++ exception becomes AR_ERR_INTERNAL and the handle's last error (0001 D2.5).
// The body may `return` directly.
#define AR_GUARD(ctx, ...)                                                  \
  if (refuse_if_poisoned(ctx)) return AR_ERR_INTERNAL;                     \
  AR_GUARD_NOCHECK(ctx, __VA_ARGS__)

// For the two calls that must still work on a poisoned context: reading its last
// error and destroying it.
#define AR_GUARD_NOCHECK(ctx, ...)                          \
  try {                                                     \
    __VA_ARGS__                                             \
  } catch (const std::exception& e) {                       \
    return fail_internal((ctx), e.what());                  \
  } catch (...) {                                           \
    return fail_internal((ctx), "unknown exception");       \
  }

extern "C" {

AR_API ar_status ar_device_count(uint32_t* count) {
  AR_GUARD(nullptr, {
    if (!count) return AR_ERR_INVALID_ARGUMENT;
    *count = static_cast<uint32_t>(astroray::list_devices().devices.size());
    return AR_OK;
  })
}

AR_API ar_status ar_device_get_info(uint32_t index, ar_device_info* info) {
  AR_GUARD(nullptr, {
    if (!info) return AR_ERR_INVALID_ARGUMENT;
    astroray::DeviceList list = astroray::list_devices();
    if (index >= list.devices.size()) return AR_ERR_INVALID_ARGUMENT;
    const astroray::DeviceRecord& d = list.devices[index];
    ar_device_info r;
    memset(&r, 0, sizeof r);
    r.kind = d.kind;
    r.index = d.index;
    r.status = d.status.status;
    r.compute_capability = d.compute_capability;
    r.memory_bytes = d.memory_bytes;
    put(r.name, d.name);
    put(r.reason, d.status.reason);
    *info = r;
    return AR_OK;
  })
}

AR_API ar_status ar_context_create(const ar_context_desc* desc, ar_context* out) {
  AR_GUARD(nullptr, {
    RecHolder rec(desc);
    AR_RECORD(ar_context_create, rec.r, desc, out);
    AR_RECORD_BODY({
      if (!out) return AR_ERR_INVALID_ARGUMENT;
      *out = nullptr;
      if (!desc || !desc->log_path || !desc->log_path[0]) return AR_ERR_INVALID_ARGUMENT;

      astroray::DeviceList list = astroray::list_devices();
      const astroray::DeviceRecord* dev = nullptr;
      if (desc->device_kind == AR_DEVICE_CPU) {
        if (desc->device_index != 0) return AR_ERR_INVALID_ARGUMENT;
        dev = &list.devices[0];
      } else if (desc->device_kind == AR_DEVICE_CUDA) {
        for (const astroray::DeviceRecord& d : list.devices)
          if (d.kind == AR_DEVICE_CUDA && d.index == desc->device_index) dev = &d;
        // With no CUDA device, index 0 is a missing device (AR_ERR_DEVICE, the reason is
        // the probe's own); any other index is a bad argument. No fallback to the CPU.
        if (!dev) return (list.devices.size() == 1 && desc->device_index == 0) ? AR_ERR_DEVICE : AR_ERR_INVALID_ARGUMENT;
      } else {
        return AR_ERR_INVALID_ARGUMENT;
      }
      if (dev->status.status != AR_OK) return dev->status.status;

      std::unique_ptr<ar_context_s> ctx(new ar_context_s);
      std::string err;
      ctx->log_path = desc->log_path;
      if (!ctx->log.open(desc->log_path, &err)) return AR_ERR_INVALID_ARGUMENT;
      if (!rec.err.empty()) {  // the recording path cannot be opened: say why in the new log
        ctx->log.line("ERROR", rec.err);
        return AR_ERR_INVALID_ARGUMENT;
      }
      ctx->kind = dev->kind;
      ctx->index = dev->index;
      ctx->record_path = rec.path;
      ctx->recorder = rec.r;  // owned by the context from here on
      rec.r = nullptr;

      // Every field of the build record, so a log alone identifies the binary (0001 D2.7).
      const astroray::BuildInfo& b = astroray::kBuildInfo;
      ctx->log.line("INFO", std::string("build version=") + b.version + " git=" + b.git_sha +
                                " dirty=" + std::to_string(b.dirty) + " cuda=" + b.cuda + " optix=" + b.optix +
                                " embree=" + b.embree + " oidn=" + b.oidn + " type=" + b.build_type +
                                " kernels=" + astroray::kernel_source());
      ctx->log.line("INFO", std::string("device kind=") + (dev->kind == AR_DEVICE_CPU ? "cpu" : "cuda") +
                                " index=" + std::to_string(dev->index) + " name=" + dev->name);
      *out = ctx.release();
      AR_RECORD_BIND(*out);
      return AR_OK;
    });
  })
}

AR_API ar_status ar_context_destroy(ar_context ctx) {
  AR_RECORD(ar_context_destroy, ar_rec_of(ctx), ctx);
  AR_RECORD_SET(AR_OK);  // destroy cannot fail, and the recorder closes below
  AR_GUARD_NOCHECK(ctx, {
    if (!ctx) return AR_ERR_INVALID_ARGUMENT;
    ar_recorder* rec = ctx->recorder;
    ctx->recorder = nullptr;
    if (rec) {  // flushed here so a write failure is reported in the log
      std::string rerr;
      ar_rec_close(rec, &rerr);
      if (!rerr.empty()) ctx->log.line("ERROR", "recording " + ctx->record_path + ": " + rerr);
    }
    ctx->log.line("INFO", "context destroyed");
    delete ctx;
    return AR_OK;
  })
}

AR_API ar_status ar_context_last_error(ar_context ctx, uint32_t capacity, uint8_t* text) {
  AR_GUARD_NOCHECK(ctx, {
    if (!ctx) return AR_ERR_INVALID_ARGUMENT;
    refuse_if_poisoned(ctx);  // a poisoned context answers with its fault message
    if (capacity > 0 && !text) return reject(ctx, "invalid argument: last_error text buffer is null");
    if (capacity == 0) return AR_OK;
    size_t n = ctx->last_error.size() < capacity - 1 ? ctx->last_error.size() : capacity - 1;
    memcpy(text, ctx->last_error.data(), n);
    text[n] = 0;
    return AR_OK;
  })
}

}  // extern "C"

astroray::ContextState& astroray::context_state(ar_context ctx) { return *ctx; }
ar_recorder* ar_rec_of(ar_context ctx) { return ctx ? ctx->recorder : nullptr; }
ar_recorder* ar_rec_of_state(astroray::ContextState& state) { return static_cast<ar_context_s&>(state).recorder; }
void astroray::context_set_error(ar_context ctx, const std::string& text) { ctx->last_error = text; }
void astroray::context_device(ar_context ctx, uint32_t* kind, uint32_t* index) {
  *kind = ctx->kind;
  *index = ctx->index;
}
// Declared in host/session/session.h: a session reaches its context only through its scene.
namespace astroray {
void context_device_of_state(ContextState& state, uint32_t* kind, uint32_t* index) {
  context_device(&static_cast<ar_context_s&>(state), kind, index);
}
}  // namespace astroray

#ifdef AR_GATE_HOOKS
astroray::ContextState& astroray::fault_test_context_state(ar_context ctx) { return *ctx; }
#endif
