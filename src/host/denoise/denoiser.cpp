// SPDX-License-Identifier: Apache-2.0
// Ref: docs/decisions/0008-film-outputs-denoise.md d15 (negative colour clamped to 0 in the copy
// handed to a denoiser), d16, d18; OIDN 2.5.0 oidn.h (third_party/_cache/oidn-2.5.0/extracted/
// include); Cycles integrator/denoiser_oidn_base.cpp and denoiser_oidn_gpu.cpp (refs cache
// cycles-ed69ef2c6bd3) as the cross-check of the filter set-up: "RT" filters, hdr, quality,
// prefilters running in place on the guide buffers, then cleanAux.
#include "host/denoise/denoiser.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <utility>
#include <vector>

#include "host/denoise/oidn_loader.h"
#include "host/thread.h"

#ifndef AR_OIDN_DEV_LIBRARY_DIR
#error "AR_OIDN_DEV_LIBRARY_DIR must name the OIDN bin folder (cmake/deps.cmake)"
#endif

namespace astroray::host {

#ifdef ASTRORAY_GPU
// Defined in src/device/cuda/cuda_device.cpp (declared in its header), which exists only in
// ASTRORAY_GPU builds.
uint64_t cuda_free_device_bytes(int ordinal);
#endif

namespace {
const char* backend_name(DenoiserBackend b) {
  switch (b) {
    case DenoiserBackend::NONE: return "NONE";
    case DenoiserBackend::OIDN: return "OIDN";
    case DenoiserBackend::DLSS_RR: return "DLSS_RR";
  }
  return "?";
}

// A load step that saw the destructor's request ends here.
struct Cancelled {};

#ifdef ASTRORAY_GPU
constexpr bool kCudaBuild = true;
#else
constexpr bool kCudaBuild = false;
#endif
}  // namespace

std::string oidn_dev_library_dir() { return AR_OIDN_DEV_LIBRARY_DIR; }

// The OIDN backend. One device; per mode an "RT" filter, and for FINAL the two in-place guide
// prefilters; four 3-float host-written buffers shared by both modes. Members are released in the
// reverse order of creation, filters and buffers before the device, the library last.
struct DenoiserState::Oidn {
  // The filters of one mode and the images they were committed with.
  struct Filters {
    OIDNFilter rt = nullptr;
    OIDNFilter albedo = nullptr;  // FINAL only
    OIDNFilter normal = nullptr;  // FINAL only
    uint32_t width = 0, height = 0;
    bool with_albedo = false, with_normal = false;
  };

  std::unique_ptr<OidnLibrary> lib;
  const char* type = "CPU";
  int cuda_ordinal = -1;
  Ledger* ledger = nullptr;
  Owner owner;
  std::string name;
  OIDNDevice device = nullptr;
  Filters filters[2];  // by DenoiseMode
  OIDNBuffer color = nullptr, albedo = nullptr, normal = nullptr, output = nullptr;
  uint32_t buffer_width = 0, buffer_height = 0;
  // DENOISER ledger records (id 0: none). The device's own memory (device, weights, kernels,
  // measured at creation) and the configuration's (buffers, commits and what OIDN keeps of them),
  // replaced at every reconfiguration by its net change (run()), so sizes that come and go never
  // add up.
  uint64_t creation_record = 0, creation_bytes = 0;
  uint64_t config_record = 0, config_bytes = 0;
  std::vector<float> staging;     // packed colour in, output back

  ~Oidn() { release(); }

  std::string prefix() const { return "OIDN " + (lib ? lib->path() : std::string("?")) + ": "; }

  // The first error OIDN stored for this thread (device creation failures sit on the null device).
  // Throws std::runtime_error with the library path, `what` and OIDN's text.
  void check(OIDNDevice dev, const char* what) {
    const char* message = nullptr;
    const OIDNError e = lib->oidnGetDeviceError(dev, &message);
    if (e != OIDN_ERROR_NONE)
      throw std::runtime_error(prefix() + what + " failed (OIDN error " + std::to_string(int(e)) + "): " +
                               (message && *message ? message : "no message"));
  }

  uint64_t free_bytes() const {
#ifdef ASTRORAY_GPU
    return cuda_free_device_bytes(cuda_ordinal);
#else
    return 0;
#endif
  }

  // Ref: 0008 d16 (the denoiser's device memory is reported to the ledger); ledger.h DEVICE_CONTEXT:
  // memory a library takes outside our allocations is the drop in free device bytes around the
  // call that takes it. The CPU device takes none: 0 there, and 0 when free memory rose.
  uint64_t drop_since(uint64_t before) const {
    if (cuda_ordinal < 0) return 0;
    const uint64_t now = free_bytes();
    return now < before ? before - now : 0;
  }

  // Replaces the record `id` holding `held` bytes by one of `bytes` (no record for 0).
  void set_record(uint64_t& id, uint64_t& held, uint64_t bytes) {
    if (id) ledger->remove(id);
    id = 0;
    held = 0;
    if (bytes == 0) return;
    id = ledger->add(owner, LedgerCategory::DENOISER, bytes);
    held = bytes;
  }

  uint64_t bytes() const { return creation_bytes + config_bytes; }

  OIDNFilter new_filter(DenoiseMode mode) {
    OIDNFilter f = lib->oidnNewFilter(device, "RT");
    check(device, "oidnNewFilter(RT)");
    // d18: FINAL is high quality, VIEWPORT balanced; both hdr (scene-linear radiance).
    lib->oidnSetFilterInt(f, "quality", mode == DenoiseMode::FINAL ? OIDN_QUALITY_HIGH : OIDN_QUALITY_BALANCED);
    return f;
  }

  void create(const DenoiserConfig& c, const std::atomic<bool>& cancel) {
    lib = std::make_unique<OidnLibrary>(c.library_dir);
    if (cancel.load()) throw Cancelled{};
    cuda_ordinal = c.cuda_device;
    ledger = c.ledger;
    owner = c.owner;
    uint64_t before = 0;
    if (cuda_ordinal >= 0) {
      type = "CUDA";
      if constexpr (!kCudaBuild) throw std::runtime_error(prefix() + "a CUDA device was requested from a build without the CUDA device");
      if (!c.cuda_stream) throw std::invalid_argument(prefix() + "a CUDA device needs the engine's stream (cuda_stream)");
      if (!ledger) throw std::invalid_argument(prefix() + "a CUDA device needs a ledger for its memory");
      // d16: no CPU fallback for a GPU render; an unsupported GPU is the failure itself.
      if (!lib->oidnIsCUDADeviceSupported(cuda_ordinal)) {
        const char* message = nullptr;
        lib->oidnGetDeviceError(nullptr, &message);
        throw std::runtime_error(prefix() + "CUDA device " + std::to_string(cuda_ordinal) +
                                 " is not supported (oidnIsCUDADeviceSupported is false)" +
                                 (message && *message ? std::string(": ") + message : std::string()));
      }
      before = free_bytes();
      // d18: on the engine's stream, so denoising is ordered with the render's launches.
      const cudaStream_t stream = static_cast<cudaStream_t>(c.cuda_stream);
      device = lib->oidnNewCUDADevice(&cuda_ordinal, &stream, 1);
      check(nullptr, "oidnNewCUDADevice");
    } else {
      if (!lib->oidnIsCPUDeviceSupported()) {
        const char* message = nullptr;
        lib->oidnGetDeviceError(nullptr, &message);
        throw std::runtime_error(prefix() + "the CPU device is not supported" +
                                 (message && *message ? std::string(": ") + message : std::string()));
      }
      device = lib->oidnNewDevice(OIDN_DEVICE_TYPE_CPU);
      check(nullptr, "oidnNewDevice(CPU)");
    }
    if (!device) throw std::runtime_error(prefix() + type + " device creation returned no device and no error");
    lib->oidnCommitDevice(device);
    check(device, "oidnCommitDevice");
    if (cancel.load()) throw Cancelled{};
    for (DenoiseMode m : {DenoiseMode::FINAL, DenoiseMode::VIEWPORT}) {
      Filters& f = filters[size_t(m)];
      f.rt = new_filter(m);
      if (m == DenoiseMode::FINAL) {
        f.albedo = new_filter(m);
        f.normal = new_filter(m);
      }
    }
    check(device, "filter set-up");
    if (cancel.load()) throw Cancelled{};
    // d16: filter commit and weight loading belong to creation, off the first frame's path. The image
    // size is not known yet, so both modes are committed and run once on a 1 x 1 image: the weights
    // and OIDN's kernels load here, and the first real denoise only sizes its buffers.
    const float zero[3] = {0.0f, 0.0f, 0.0f};
    DenoiseInputs warm;
    warm.color = {zero, 1, 1, 3};
    warm.albedo = warm.color;
    warm.normal = warm.color;
    DisplayImage scratch;
    for (DenoiseMode m : {DenoiseMode::FINAL, DenoiseMode::VIEWPORT}) execute(warm, scratch, m);
    // The 1 x 1 images are size-dependent: they go, so the creation record holds only what stays.
    drop_images();
    set_record(creation_record, creation_bytes, drop_since(before));
    name = describe();
  }

  std::string describe() const {
    std::string s = std::string("OIDN ") + OIDN_VERSION_STRING + " " + type + " device";
    if (cuda_ordinal >= 0) s += " " + std::to_string(cuda_ordinal) + " (oidnIsCUDADeviceSupported true)";
    // OIDN names physical devices, not created ones: every physical device of this type.
    const int want = cuda_ordinal >= 0 ? OIDN_DEVICE_TYPE_CUDA : OIDN_DEVICE_TYPE_CPU;
    std::string names;
    const int n = lib->oidnGetNumPhysicalDevices();
    for (int i = 0; i < n; ++i) {
      if (lib->oidnGetPhysicalDeviceInt(i, "type") != want) continue;
      const char* nm = lib->oidnGetPhysicalDeviceString(i, "name");
      std::string one = nm ? nm : "?";
      while (!one.empty() && one.back() == ' ') one.pop_back();  // CPU brand strings are space-padded
      names += (names.empty() ? "" : ", ") + one;
    }
    return names.empty() ? s : s + ": " + names;
  }

  // Unbinds every image of every filter, then releases the buffers. A bound image keeps its buffer
  // alive, so a buffer is freed only once no filter of either mode holds it.
  void drop_images() {
    for (Filters& f : filters) {
      if (f.rt)
        for (const char* image : {"color", "albedo", "normal", "output"}) lib->oidnUnsetFilterImage(f.rt, image);
      if (f.albedo)
        for (const char* image : {"albedo", "output"}) lib->oidnUnsetFilterImage(f.albedo, image);
      if (f.normal)
        for (const char* image : {"normal", "output"}) lib->oidnUnsetFilterImage(f.normal, image);
      f.width = f.height = 0;  // their images are gone: commit again
    }
    for (OIDNBuffer* b : {&output, &normal, &albedo, &color}) {
      if (*b) lib->oidnReleaseBuffer(*b);
      *b = nullptr;
    }
    buffer_width = buffer_height = 0;
  }

  // Teardown cannot throw; OIDN's objects go before the library that holds their code.
  void release() noexcept {
    if (lib) {
      if (device) lib->oidnSyncDevice(device);
      for (Filters& f : filters)
        for (OIDNFilter* p : {&f.normal, &f.albedo, &f.rt}) {
          if (*p) lib->oidnReleaseFilter(*p);
          *p = nullptr;
        }
      drop_images();
      if (device) lib->oidnReleaseDevice(device);
      device = nullptr;
    }
    for (uint64_t* id : {&config_record, &creation_record}) {
      try {
        if (*id) ledger->remove(*id);
      } catch (const std::exception& e) {
        std::fprintf(stderr, "astroray: releasing a denoiser ledger record failed: %s\n", e.what());
      }
      *id = 0;
    }
    creation_bytes = config_bytes = 0;
    lib.reset();
  }

  void set_image(OIDNFilter f, const char* image, OIDNBuffer b, uint32_t w, uint32_t h) {
    lib->oidnSetFilterImage(f, image, b, OIDN_FORMAT_FLOAT3, w, h, 0, 0, 0);
  }

  bool configured(DenoiseMode mode, const DenoiseInputs& in) const {
    const Filters& f = filters[size_t(mode)];
    return f.width == in.color.width && f.height == in.color.height && f.with_albedo == (in.albedo.data != nullptr) &&
           f.with_normal == (in.normal.data != nullptr);
  }

  // Sizes the buffers and commits the mode's filters for a new size or guide set.
  void configure(DenoiseMode mode, uint32_t w, uint32_t h, bool with_albedo, bool with_normal) {
    Filters& f = filters[size_t(mode)];
    const size_t bytes_per_image = size_t(w) * h * 3 * sizeof(float);
    if (buffer_width != w || buffer_height != h) {
      drop_images();
      color = lib->oidnNewBuffer(device, bytes_per_image);
      albedo = lib->oidnNewBuffer(device, bytes_per_image);
      normal = lib->oidnNewBuffer(device, bytes_per_image);
      output = lib->oidnNewBuffer(device, bytes_per_image);
      check(device, "oidnNewBuffer");
      buffer_width = w;
      buffer_height = h;
    }
    lib->oidnSetFilterBool(f.rt, "hdr", true);
    set_image(f.rt, "color", color, w, h);
    set_image(f.rt, "output", output, w, h);
    // A guide the previous commit had and this one lacks must not stay bound.
    if (with_albedo) set_image(f.rt, "albedo", albedo, w, h);
    else lib->oidnUnsetFilterImage(f.rt, "albedo");
    if (with_normal) set_image(f.rt, "normal", normal, w, h);
    else lib->oidnUnsetFilterImage(f.rt, "normal");
    // d18: FINAL prefilters the guides in place and then tells the main filter they are clean;
    // VIEWPORT uses the noisy guides as they are.
    const bool final_mode = mode == DenoiseMode::FINAL;
    lib->oidnSetFilterBool(f.rt, "cleanAux", final_mode && (with_albedo || with_normal));
    if (final_mode && with_albedo) {
      set_image(f.albedo, "albedo", albedo, w, h);
      set_image(f.albedo, "output", albedo, w, h);
      lib->oidnCommitFilter(f.albedo);
    }
    if (final_mode && with_normal) {
      set_image(f.normal, "normal", normal, w, h);
      set_image(f.normal, "output", normal, w, h);
      lib->oidnCommitFilter(f.normal);
    }
    lib->oidnCommitFilter(f.rt);
    check(device, "filter commit");
    f.width = w;
    f.height = h;
    f.with_albedo = with_albedo;
    f.with_normal = with_normal;
  }

  // A denoise. A reconfiguration is measured as one window: from before a new size drops every
  // filter's images and buffers to after the commit and first execution. The configuration record is
  // then replaced by its old bytes plus that window's net drop, which is negative when the window gave
  // memory back. OIDN keeps memory across a smaller size (on the 5070 Ti a 128 x 128 configuration
  // after a 512 x 512 one holds 84 MiB more than before it), so replacing the record by the new
  // window's drop alone would under-count; and the record cannot ratchet, because every byte OIDN
  // frees is freed inside such a window and counted.
  void run(const DenoiseInputs& in, DisplayImage& out, DenoiseMode mode) {
    const bool reconfigure = !configured(mode, in);
    const uint64_t before = reconfigure && cuda_ordinal >= 0 ? free_bytes() : 0;
    if (in.color.width != buffer_width || in.color.height != buffer_height) drop_images();
    execute(in, out, mode);
    if (reconfigure && cuda_ordinal >= 0) set_record(config_record, config_bytes, net_after(config_bytes, before));
  }

  // `held` plus the net drop in free device bytes since `before`; 0 if that is negative.
  uint64_t net_after(uint64_t held, uint64_t before) const {
    const uint64_t now = free_bytes();
    const uint64_t up = held + before;
    return up > now ? up - now : 0;
  }

  void execute(const DenoiseInputs& in, DisplayImage& out, DenoiseMode mode) {
    const ImageView& c = in.color;
    const bool with_albedo = in.albedo.data != nullptr, with_normal = in.normal.data != nullptr;
    if (!configured(mode, in)) configure(mode, c.width, c.height, with_albedo, with_normal);
    const size_t n = size_t(c.width) * c.height;
    staging.resize(n * 3);
    // d15: neither OIDN nor DLSS documents negative input; the denoiser's copy is clamped at 0.
    for (size_t i = 0; i < n; ++i)
      for (uint32_t k = 0; k < 3; ++k) staging[3 * i + k] = std::max(0.0f, c.data[i * c.components + k]);
    const size_t image_bytes = n * 3 * sizeof(float);
    lib->oidnWriteBuffer(color, 0, image_bytes, staging.data());
    if (with_albedo) lib->oidnWriteBuffer(albedo, 0, image_bytes, in.albedo.data);
    if (with_normal) lib->oidnWriteBuffer(normal, 0, image_bytes, in.normal.data);
    const Filters& f = filters[size_t(mode)];
    if (mode == DenoiseMode::FINAL && with_albedo) lib->oidnExecuteFilter(f.albedo);
    if (mode == DenoiseMode::FINAL && with_normal) lib->oidnExecuteFilter(f.normal);
    lib->oidnExecuteFilter(f.rt);
    lib->oidnReadBuffer(output, 0, image_bytes, staging.data());
    check(device, "denoise");
    // Only now is `out` written: a failure above leaves the caller's image as it was.
    out.width = c.width;
    out.height = c.height;
    out.rgba.resize(n * 4);
    for (size_t i = 0; i < n; ++i) {
      for (uint32_t k = 0; k < 3; ++k) out.rgba[4 * i + k] = staging[3 * i + k];
      // d18: alpha is not denoised; it is copied from the film.
      out.rgba[4 * i + 3] = c.components == 4 ? c.data[i * 4 + 3] : 1.0f;
    }
    out.denoised = true;
  }
};

DenoiserState::DenoiserState(ContextState& ctx, DenoiserBackend backend, const DenoiserConfig& config)
    : ctx_(ctx), backend_(backend), config_(config) {}

std::unique_ptr<DenoiserState> DenoiserState::create(ContextState& ctx, DenoiserBackend backend,
                                                     const DenoiserConfig& config) {
  std::unique_ptr<DenoiserState> s(new DenoiserState(ctx, backend, config));
  DenoiserState* p = s.get();
  p->worker_ = start_engine_thread(ctx, [p]() { p->load(); });
  return s;
}

void DenoiserState::load() {
  // NONE has nothing to load; DLSS_RR is not built and must not look ready.
  if (backend_ == DenoiserBackend::NONE) return finish(kReady, {});
  if (backend_ != DenoiserBackend::OIDN)
    return finish(kFailed, std::string("denoiser backend ") + backend_name(backend_) + " is not built");
  auto oidn = std::make_unique<Oidn>();
  try {
    oidn->create(config_, cancel_);
  } catch (const Cancelled&) {
    oidn->release();
    return finish(kFailed, "denoiser loading stopped: the state was destroyed");
  } catch (const std::exception& e) {
    oidn->release();  // a failed state holds no device memory and no library
    return finish(kFailed, e.what());
  }
  oidn_ = std::move(oidn);
  finish(kReady, {});
}

DenoiserState::~DenoiserState() {
  cancel_.store(true);
  if (worker_.joinable()) worker_.join();
  // Filters, buffers and the OIDN device go here, while the caller's CUDA device still lives.
  oidn_.reset();
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

std::string DenoiserState::description() const { return ready() && oidn_ ? oidn_->name : std::string(); }

uint64_t DenoiserState::device_bytes() const { return ready() && oidn_ ? oidn_->bytes() : 0; }

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
  if (state.failed()) throw std::runtime_error("denoiser unavailable: " + state.error());
  if (!state.ready()) throw std::logic_error("denoise: the denoiser is still loading");
  const ImageView& c = in.color;
  if (c.data == nullptr || c.width == 0 || c.height == 0 || (c.components != 3 && c.components != 4))
    throw std::invalid_argument("denoise: colour must be a non-empty image of 3 or 4 components");
  for (const ImageView* g : {&in.albedo, &in.normal})
    if (g->data != nullptr && (g->width != c.width || g->height != c.height || g->components != 3))
      throw std::invalid_argument("denoise: a guide must match the colour size and have 3 components");

  if (state.oidn_) return state.oidn_->run(in, out, mode);

  // NONE passes colour through, undenoised (0008 d16), in either mode.
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
