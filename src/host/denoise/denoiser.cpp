// SPDX-License-Identifier: Apache-2.0
#include "host/denoise/denoiser.h"

#include <chrono>
#include <stdexcept>
#include <utility>

#include "host/thread.h"

namespace astroray::host {

namespace {
const char* backend_name(DenoiserBackend b) {
  switch (b) {
    case DenoiserBackend::NONE: return "NONE";
    case DenoiserBackend::OIDN: return "OIDN";
    case DenoiserBackend::DLSS_RR: return "DLSS_RR";
  }
  return "?";
}
}  // namespace

std::unique_ptr<DenoiserState> DenoiserState::create(ContextState& ctx, DenoiserBackend backend) {
  std::unique_ptr<DenoiserState> s(new DenoiserState(ctx, backend));
  DenoiserState* p = s.get();
  p->worker_ = start_engine_thread(ctx, [p]() {
    // NONE has nothing to load; the real backends are not built yet and must not look ready.
    if (p->backend_ == DenoiserBackend::NONE) return p->finish(kReady, {});
    p->finish(kFailed, std::string("denoiser backend ") + backend_name(p->backend_) + " is not built");
  });
  return s;
}

DenoiserState::~DenoiserState() {
  if (worker_.joinable()) worker_.join();
}

void DenoiserState::finish(Status s, std::string error) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    error_ = std::move(error);
    status_.store(s, std::memory_order_release);
  }
  cv_.notify_all();
}

std::string DenoiserState::error() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return error_;
}

void DenoiserState::wait() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (status_.load(std::memory_order_acquire) == kLoading) {
    // A faulting worker never calls finish(); the poisoned context ends the wait.
    if (ctx_.poisoned.load(std::memory_order_acquire)) {
      error_ = "denoiser loading stopped: context poisoned: " + ctx_.poison_message;
      status_.store(kFailed, std::memory_order_release);
      return;
    }
    cv_.wait_for(lock, std::chrono::milliseconds(10));
  }
}

void denoise(DenoiserState& state, const DenoiseInputs& in, DisplayImage& out, DenoiseMode mode) {
  (void)mode;  // NONE treats both modes alike; real backends select quality by it
  if (state.failed()) throw std::runtime_error("denoiser unavailable: " + state.error());
  if (!state.ready()) throw std::logic_error("denoise: the denoiser is still loading");
  const ImageView& c = in.color;
  if (c.data == nullptr || c.width == 0 || c.height == 0 || (c.components != 3 && c.components != 4))
    throw std::invalid_argument("denoise: colour must be a non-empty image of 3 or 4 components");
  for (const ImageView* g : {&in.albedo, &in.normal})
    if (g->data != nullptr && (g->width != c.width || g->height != c.height || g->components != 3))
      throw std::invalid_argument("denoise: a guide must match the colour size and have 3 components");

  // Only NONE can be ready today: it passes colour through, undenoised (0008 d16).
  out.width = c.width;
  out.height = c.height;
  out.rgba.resize(size_t(c.width) * c.height * 4);
  for (size_t i = 0; i < size_t(c.width) * c.height; ++i) {
    for (uint32_t k = 0; k < 3; ++k) out.rgba[4 * i + k] = c.data[i * c.components + k];
    out.rgba[4 * i + 3] = c.components == 4 ? c.data[i * 4 + 3] : 1.0f;
  }
  out.denoised = false;
}

}  // namespace astroray::host
