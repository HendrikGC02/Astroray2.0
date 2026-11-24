// SPDX-License-Identifier: Apache-2.0
// The one denoiser interface. A backend is created on a worker thread at session start so the
// first frame never waits for it; denoise() writes a DisplayImage and nothing else. Backend NONE
// copies colour through with denoised = false; OIDN runs on a CUDA device on the engine's stream
// (GPU renders) or on the CPU device (CPU renders); DLSS_RR is not built and fails loudly at
// creation.
// Ref: docs/decisions/0008-film-outputs-denoise.md d16 (interface, asynchronous creation, failure
// leaves denoised = false and an error, no CPU fallback for a GPU render), d17 (denoisers write
// only DisplayImage), d18 (OIDN devices and filter settings).
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "host/fault.h"
#include "host/film/display_image.h"
#include "host/ledger.h"

namespace astroray::host {

enum class DenoiserBackend : uint8_t { NONE = 0, OIDN, DLSS_RR };
enum class DenoiseMode : uint8_t { FINAL = 0, VIEWPORT };

// A read-only float image: `components` floats per pixel, rows bottom-up.
struct ImageView {
  const float* data = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t components = 0;
};

struct DenoiseMetadata {
  uint32_t samples_done = 0;
  uint64_t generation = 0;
};

// Display colour (3 or 4 components) and optional guides (albedo, normal: 3 components). Band,
// cube, photon and statistics channels are never inputs (0008 d17).
struct DenoiseInputs {
  ImageView color;
  ImageView albedo;
  ImageView normal;
  DenoiseMetadata metadata;
};

// The folder of astroray_oidn.dll this build was configured with: the development dependency
// cache (cmake/deps.cmake).
std::string oidn_dev_library_dir();

// What a backend runs on. The defaults suit NONE and a CPU-device OIDN.
struct DenoiserConfig {
  // Folder of astroray_oidn.dll and its sibling modules (OIDN only); empty fails creation.
  std::string library_dir = oidn_dev_library_dir();
  // CUDA device ordinal of a GPU render (CudaDevice::device_ordinal()), or -1: the CPU device.
  int cuda_device = -1;
  // The engine's CUstream on that device (CudaDevice::native_stream()); required with cuda_device.
  void* cuda_stream = nullptr;
  // Records the CUDA device's memory under DENOISER; required with cuda_device. Must outlive the state.
  Ledger* ledger = nullptr;
  Owner owner;
};

class DenoiserState {
 public:
  // Returns at once; the backend loads on a worker thread of ctx (which must outlive the state).
  // A CUDA device's stream and the device itself must outlive the state too: the destructor
  // releases OIDN's filters, buffers and device first.
  static std::unique_ptr<DenoiserState> create(ContextState& ctx, DenoiserBackend backend,
                                               const DenoiserConfig& config = DenoiserConfig());
  // Stops a load in progress at its next step, joins the worker, then releases the backend and
  // removes its ledger records.
  ~DenoiserState();
  DenoiserState(const DenoiserState&) = delete;
  DenoiserState& operator=(const DenoiserState&) = delete;

  DenoiserBackend backend() const { return backend_; }
  // True once loading finished and the backend works.
  bool ready() const { return status_.load(std::memory_order_acquire) == kReady; }
  bool failed() const { return status_.load(std::memory_order_acquire) == kFailed; }
  // Why loading failed; empty unless failed().
  std::string error() const;
  // Blocks until loading finished or failed (a poisoned context counts as failed, with that
  // message). A final render calls this only before its denoise step.
  void wait();
  // Once ready(): the device the backend runs on, e.g. "OIDN 2.5.0 CUDA device 0 (<GPU name>)";
  // empty for NONE.
  std::string description() const;
  // Device memory the backend holds: the drops in free device bytes recorded under DENOISER
  // (0 on the CPU device). Read it only while no denoise() runs.
  uint64_t device_bytes() const;

 private:
  enum Status : int { kLoading = 0, kReady, kFailed };
  struct Oidn;
  DenoiserState(ContextState& ctx, DenoiserBackend backend, const DenoiserConfig& config);
  void finish(Status s, std::string error);
  void load();
  friend void denoise(DenoiserState& state, const DenoiseInputs& in, DisplayImage& out, DenoiseMode mode);

  ContextState& ctx_;
  DenoiserBackend backend_;
  DenoiserConfig config_;
  std::atomic<int> status_{kLoading};
  std::atomic<bool> cancel_{false};
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::string error_;
  // Written by the worker before it reports ready, then used only by the denoise() caller.
  std::unique_ptr<Oidn> oidn_;
  std::thread worker_;
};

// Throws std::logic_error when the state is still loading (the caller presents the noisy image
// until ready()), std::runtime_error with the state's error when loading failed or with OIDN's
// error when denoising fails, and std::invalid_argument for inputs of the wrong shape; `out` is
// untouched by every throw. On success `out` has the colour's size and denoised is true for OIDN
// (false for NONE, which copies colour). Mode FINAL or VIEWPORT selects OIDN's quality (d18).
void denoise(DenoiserState& state, const DenoiseInputs& in, DisplayImage& out, DenoiseMode mode);

}  // namespace astroray::host
