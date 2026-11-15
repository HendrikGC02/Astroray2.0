// SPDX-License-Identifier: Apache-2.0
// The one denoiser interface. A backend is created on a worker thread at session start so the
// first frame never waits for it; denoise() writes a DisplayImage and nothing else. Backend NONE
// copies colour through with denoised = false; OIDN and DLSS_RR slot in behind the same
// signature (M1b) and until then fail loudly at creation.
// Ref: docs/decisions/0008-film-outputs-denoise.md d16 (interface, asynchronous creation, failure
// leaves denoised = false and an error), d17 (denoisers write only DisplayImage).
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

class DenoiserState {
 public:
  // Returns at once; the backend loads on a worker thread of ctx (which must outlive the state).
  static std::unique_ptr<DenoiserState> create(ContextState& ctx, DenoiserBackend backend);
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

 private:
  enum Status : int { kLoading = 0, kReady, kFailed };
  DenoiserState(ContextState& ctx, DenoiserBackend backend) : ctx_(ctx), backend_(backend) {}
  void finish(Status s, std::string error);

  ContextState& ctx_;
  DenoiserBackend backend_;
  std::atomic<int> status_{kLoading};
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::string error_;
  std::thread worker_;
};

// Throws std::logic_error when the state is still loading (the caller presents the noisy image
// until ready()), std::runtime_error with the state's error when loading failed, and
// std::invalid_argument for inputs of the wrong shape. `out` is resized to the colour's size.
void denoise(DenoiserState& state, const DenoiseInputs& in, DisplayImage& out, DenoiseMode mode);

}  // namespace astroray::host
