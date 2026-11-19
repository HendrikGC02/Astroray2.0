// SPDX-License-Identifier: Apache-2.0
// Test-only: replays an .arrec through the generated replayer and the public ABI.
// Usage: replay_driver <file.arrec> <context log> [record path]
//   Prints "status <n>", "calls <n>" and the message; exits with the status.
// Usage: replay_driver --ledger <file.arrec> <context log> <width> <height>
//   0002 D13's check (task 126): retains the CUDA primary context, reads cuMemGetInfo, replays the
//   recording on CUDA at <width> x <height> with 1 sample, renders it to convergence and reads the
//   free bytes again. Prints one "ledger" line: the drop in free bytes, ar_progress's ledger total,
//   its device free and total bytes and the per-category bytes; exits 0 when it got that far.
//   The driver API comes from nvcuda.dll at run time, so this target needs no CUDA headers.
// Ref: docs/decisions/0002-kernel-architecture.md D13 (check: drop in cuMemGetInfo free memory vs the
// ledger total, within 64 MiB); CUDA Driver API reference (cuMemGetInfo, cuDevicePrimaryCtxRetain).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include <chrono>
#include <string>

#include "replayer.h"

namespace {

// The few driver entry points the check needs, with cuda.h's signatures (CUresult is int, CUdevice
// int, CUcontext a pointer); the _v2 names are the ones cuda.h maps cuMemGetInfo and the context
// calls to.
struct Cu {
  int (*init)(unsigned) = nullptr;
  int (*device_get)(int*, int) = nullptr;
  int (*retain)(void**, int) = nullptr;
  int (*release)(int) = nullptr;
  int (*set_current)(void*) = nullptr;
  int (*mem_get_info)(size_t*, size_t*) = nullptr;
};

template <class F>
bool sym(HMODULE m, const char* name, F& f) {
  f = reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, name)));
  if (!f) fprintf(stderr, "nvcuda.dll has no %s\n", name);
  return f != nullptr;
}

bool free_bytes(const Cu& cu, size_t* free) {
  size_t total = 0;
  return cu.mem_get_info(free, &total) == 0;
}

int ledger_check(const char* rec, const char* log, uint32_t width, uint32_t height) {
  HMODULE m = LoadLibraryA("nvcuda.dll");
  if (!m) {
    fprintf(stderr, "nvcuda.dll not found\n");
    return 3;
  }
  Cu cu;
  if (!sym(m, "cuInit", cu.init) || !sym(m, "cuDeviceGet", cu.device_get) ||
      !sym(m, "cuDevicePrimaryCtxRetain", cu.retain) || !sym(m, "cuDevicePrimaryCtxRelease_v2", cu.release) ||
      !sym(m, "cuCtxSetCurrent", cu.set_current) || !sym(m, "cuMemGetInfo_v2", cu.mem_get_info))
    return 3;
  int dev = 0;
  void* ctx = nullptr;
  if (cu.init(0) != 0 || cu.device_get(&dev, 0) != 0 || cu.retain(&ctx, dev) != 0 || cu.set_current(ctx) != 0) {
    fprintf(stderr, "CUDA primary context unavailable\n");
    return 3;
  }
  // The context exists from here on, so the drop below is the engine's alone (the engine retains the
  // same primary context).
  size_t before = 0, after = 0;
  if (!free_bytes(cu, &before)) return 3;

  ArReplayOptions opt;
  opt.log_path = log;
  opt.keep_alive = true;
  opt.device_kind = AR_DEVICE_CUDA;
  opt.device_index = 0;
  opt.width = width;
  opt.height = height;
  opt.samples = 1;
  ArReplayResult res;
  const ar_status st = ar_replay_file(rec, opt, &res);
  int rc = 0;
  ar_progress p{};
  if (st != AR_OK || !res.matched || res.sessions.empty()) {
    fprintf(stderr, "replay failed: status %u: %s\n", static_cast<unsigned>(st), res.message.c_str());
    rc = 4;
  } else {
    ar_session s = res.sessions[0];
    ar_session_get_progress(s, &p);
    if (p.state == AR_STATE_INITIALIZING) ar_session_start(s);
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    for (;;) {
      ar_session_wait(s, 100);
      if (ar_session_get_progress(s, &p) != AR_OK || p.state == AR_STATE_FAILED) {
        fprintf(stderr, "session failed (state %u)\n", p.state);
        rc = 5;
        break;
      }
      if (p.state == AR_STATE_CONVERGED && p.active_generation == p.pending_generation) break;
      if (std::chrono::steady_clock::now() > end) {
        fprintf(stderr, "session not converged after 120 s\n");
        rc = 5;
        break;
      }
    }
    if (rc == 0) {
      // Read both after the render: the session holds every allocation of its pass now.
      if (!free_bytes(cu, &after)) rc = 3;
      ar_progress q{};
      if (rc == 0 && ar_session_get_progress(s, &q) != AR_OK) rc = 5;
      if (rc == 0) {
        unsigned long long total = 0;
        std::string cats;
        for (size_t i = 0; i < sizeof q.ledger_bytes / sizeof q.ledger_bytes[0]; ++i) {
          total += q.ledger_bytes[i];
          cats += " " + std::to_string(q.ledger_bytes[i]);
        }
        printf("ledger drop=%llu total=%llu progress_free=%llu progress_total=%llu after=%llu categories%s\n",
               static_cast<unsigned long long>(before - after), total,
               static_cast<unsigned long long>(q.device_free_bytes),
               static_cast<unsigned long long>(q.device_total_bytes), static_cast<unsigned long long>(after),
               cats.c_str());
      }
    }
  }
  for (ar_session s : res.sessions) {
    ar_session_close(s);
    ar_session_destroy(s);
  }
  for (ar_scene s : res.scenes) ar_scene_destroy(s);
  if (res.context) ar_context_destroy(res.context);
  cu.release(dev);
  return rc;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 6 && strcmp(argv[1], "--ledger") == 0)
    return ledger_check(argv[2], argv[3], static_cast<uint32_t>(strtoul(argv[4], nullptr, 10)),
                        static_cast<uint32_t>(strtoul(argv[5], nullptr, 10)));
  if (argc < 3) {
    fprintf(stderr, "usage: replay_driver <file.arrec> <context log> [record path]\n"
                    "       replay_driver --ledger <file.arrec> <context log> <width> <height>\n");
    return 2;
  }
  ArReplayOptions opt;
  opt.log_path = argv[2];
  opt.record_path = argc > 3 ? argv[3] : nullptr;
  ArReplayResult res;
  ar_status st = ar_replay_file(argv[1], opt, &res);
  printf("status %u\ncalls %llu\nmatched %d\n%s\n", static_cast<unsigned>(st), static_cast<unsigned long long>(res.calls),
         res.matched ? 1 : 0, res.message.c_str());
  return static_cast<int>(st);
}
