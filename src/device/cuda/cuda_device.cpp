// SPDX-License-Identifier: Apache-2.0
// CudaDevice. Every CUDA driver call goes through Impl::check; the driver entry points
// are resolved per device from static cudart's cudaGetDriverEntryPointByVersion, so no
// nvcuda import library is linked and nothing is process-global. The one thing shared
// with the rest of the process is the primary context (0001 D2.1): a sticky error in it
// is observed from CUDA itself, because every later call returns the same error.
// Ref: CUDA Driver API reference (cuModuleLoadData, cuLaunchKernel, cuFuncGetAttribute,
// cuMemGetInfo); docs/decisions/0002-kernel-architecture.md D4, D8, D11, D14, D15; 0001 D2.1,
// D2.5; 0007 d14; RULINGS 70; OptiX 9.1 Programming Guide (optixLaunch).
#include "device/cuda/cuda_device.h"

#include <cuda.h>
#include <cuda_runtime_api.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "device/cuda/module_loader.h"
#include "device/cuda/optix_accel.h"
#include "host/device_probe.h"
#include "host/session/test_hooks.h"
#include "kernel/launch_config.h"

namespace astroray::host {
namespace {

// The block size of every kernel comes from launch_config.h, the table its __launch_bounds__
// reads (0002 D4): one name and row per AR_LAUNCH_ROWS line.
struct NamedRow {
  const char* name;
  ar::LaunchRow row;
};
#define AR_NAMED_ROW(name, T, B, K, CLASS) {#name, ar::kLaunch_##name},
constexpr NamedRow kLaunchRows[] = {AR_LAUNCH_ROWS(AR_NAMED_ROW)};
#undef AR_NAMED_ROW

const ar::LaunchRow* launch_row(const std::string& kernel) {
  for (const NamedRow& r : kLaunchRows)
    if (kernel == r.name) return &r.row;
  return nullptr;
}

// Names of the kernel modules, in enum order (0002 D8): the module is <name>.cubin and
// its entry point k_<name>.
constexpr const char* kStageNames[] = {
    "intersect_closest", "intersect_subsurface", "intersect_shadow", "shade_background",
    "shade_light",       "shade_surface",        "shade_surface_ext", "shade_light_path",
    "shade_light_path_ext", "shade_shadow",      "shade_volume",     "shade_subsurface",
    "advance_geodesic",  "terminated",           "shadow_terminated"};
static_assert(sizeof(kStageNames) / sizeof(kStageNames[0]) == ar::kStageCount, "one name per Stage");

constexpr const char* kKernelNames[] = {
    "queue_build_count",  "queue_build_scan", "queue_build_scatter", "sort_count",           "sort_scatter",
    "generate_camera",    "generate_light",   "film_resolve",        "film_adaptive",        "film_present",
    "film_heatmap",       "film_prepare_denoise", "film_pack_dlss",  "bake_environment", "film_build_active",
    "sort_scan"};
static_assert(sizeof(kKernelNames) / sizeof(kKernelNames[0]) == kKernelIdCount, "one name per KernelId");

static_assert(std::is_standard_layout_v<ar::KernelGlobals>, "read_step_table reads a member by offset");

std::string number(uint64_t v) { return std::to_string(v); }

uint32_t blocks_for(uint32_t items, uint32_t threads) {
  return items == 0 ? 1u : static_cast<uint32_t>((uint64_t(items) + threads - 1u) / threads);
}

// The driver functions this file uses; each is resolved by its unversioned name for the
// CUDA version the build compiles against (12.8), so the signatures are cuda.h's.
#define AR_CU_FUNCTIONS(X)                                                                              \
  X(cuInit) X(cuDriverGetVersion) X(cuDeviceGet) X(cuDeviceGetAttribute) X(cuDevicePrimaryCtxRetain)                          \
  X(cuDevicePrimaryCtxRelease) X(cuCtxSetCurrent) X(cuCtxSynchronize) X(cuModuleLoadData)               \
  X(cuModuleUnload) X(cuModuleGetFunction) X(cuFuncGetAttribute) X(cuLaunchKernel) X(cuMemAlloc) X(cuMemFree)                 \
  X(cuMemAllocHost) X(cuMemFreeHost) X(cuMemGetInfo) X(cuMemcpyHtoDAsync) X(cuMemcpyDtoHAsync) X(cuMemcpyDtoDAsync) X(cuMemsetD8Async) X(cuStreamCreate)        \
  X(cuStreamDestroy) X(cuEventCreate) X(cuEventRecord) X(cuEventSynchronize) X(cuEventDestroy)          \
  X(cuGetErrorName) X(cuGetErrorString)

struct Driver {
#define AR_CU_MEMBER(n) decltype(&n) n##_ = nullptr;
  AR_CU_FUNCTIONS(AR_CU_MEMBER)
#undef AR_CU_MEMBER
};

template <class Fn>
void resolve(const char* symbol, Fn& fn) {
  void* p = nullptr;
  cudaDriverEntryPointQueryResult status = cudaDriverEntryPointSymbolNotFound;
  const cudaError_t e = cudaGetDriverEntryPointByVersion(symbol, &p, CUDA_VERSION, cudaEnableDefault, &status);
  if (e != cudaSuccess || status != cudaDriverEntryPointSuccess || !p)
    throw DeviceError(std::string("CUDA driver function ") + symbol + " is unavailable (" +
                      (e != cudaSuccess ? cudaGetErrorString(e) : "symbol not found") + ")");
  fn = reinterpret_cast<Fn>(p);
}

Driver load_driver() {
  Driver d;
#define AR_CU_RESOLVE(n) resolve(#n, d.n##_);
  AR_CU_FUNCTIONS(AR_CU_RESOLVE)
#undef AR_CU_RESOLVE
  return d;
}

}  // namespace

uint32_t checked_launch_threads(const std::string& kernel, const ar::LaunchRow* row, int function_limit) {
  if (!row || row->threads == 0)
    throw std::logic_error("CudaDevice: kernel " + kernel + " has no launch_config.h row with threads (AR_LAUNCH_ROWS)");
  if (function_limit < 0) return row->threads;
  if (function_limit == 0 || row->threads > static_cast<uint32_t>(function_limit))
    throw std::logic_error("CudaDevice: kernel " + kernel + " admits " + number(uint64_t(function_limit)) +
                           " threads per block, launch_config.h gives T = " + number(row->threads));
  return row->threads;
}

bool cuda_result_is_sticky(int r) {
  // The codes cuda.h documents as leaving the process unusable ("the process must be
  // terminated and relaunched", or the context "cannot be used anymore"), plus the ECC
  // error 0002 D15 names.
  switch (r) {
    case CUDA_ERROR_ECC_UNCORRECTABLE:
    case CUDA_ERROR_CONTAINED:
    case CUDA_ERROR_ILLEGAL_ADDRESS:
    case CUDA_ERROR_LAUNCH_TIMEOUT:
    case CUDA_ERROR_ASSERT:
    case CUDA_ERROR_HARDWARE_STACK_ERROR:
    case CUDA_ERROR_ILLEGAL_INSTRUCTION:
    case CUDA_ERROR_MISALIGNED_ADDRESS:
    case CUDA_ERROR_INVALID_ADDRESS_SPACE:
    case CUDA_ERROR_INVALID_PC:
    case CUDA_ERROR_LAUNCH_FAILED:
    case CUDA_ERROR_TENSOR_MEMORY_LEAK:
    case CUDA_ERROR_MPS_CLIENT_TERMINATED:
    case CUDA_ERROR_EXTERNAL_DEVICE:
      return true;
    default:
      return false;
  }
}

std::string cuda_failure_message(int r, const char* name, const char* text, const char* call, const char* kernel) {
  const std::string what = std::string(name ? name : "CUDA_ERROR_?") + ": " + (text ? text : "unknown error");
  const std::string where = std::string(call) + ((kernel && *kernel) ? std::string(" (") + kernel + ")" : "");
  if (cuda_result_is_sticky(r)) return DeviceError::sticky_text(what + " in " + where);
  return "CUDA error in " + where + ": " + what;
}

std::string cuda_latent_error() {
  cudaError_t e = cudaDeviceSynchronize();
  if (e != cudaSuccess) return std::string("cudaDeviceSynchronize: ") + cudaGetErrorName(e) + ": " + cudaGetErrorString(e);
  e = cudaGetLastError();
  if (e != cudaSuccess) return std::string("cudaGetLastError: ") + cudaGetErrorName(e) + ": " + cudaGetErrorString(e);
  return {};
}

struct CudaDevice::Impl {
  Driver cu;
  CUdevice device = 0;
  CUcontext ctx = nullptr;
  CUstream stream = nullptr;
  CUevent done = nullptr;
  std::string arch;  // "sm_120"
  TestHooks hooks;   // read once at construction (gate builds; empty otherwise)
  bool fault_pending = false;  // ASTRORAY_TEST_GPU_FAULT: the next launch runs test_fault.cu first
  // ASTRORAY_TEST_LAUNCH_TIMING: every launch is waited for and timed; the longest is printed when the
  // device goes (0002 D15's debug wait, 0009 B-17).
  double longest_launch_ms = 0.0;
  std::string longest_launch_name;
  uint64_t timed_launches = 0;
  bool dead = false;
  std::string dead_message;  // the sticky error's message, thrown again by every later call
  CUdeviceptr globals = 0;
  DeviceBuffer globals_buffer;  // the same allocation, as the traversal launch takes it
  std::unique_ptr<TraversalPipeline> traversal;  // INTERSECT_CLOSEST and INTERSECT_SHADOW
  uint64_t traversable = 0;
  // Pinned host memory for the per-step transfers (0002 D7 step 2): the step-table readback at
  // [0, kTableBytes), then a ring that upload_async stages into. Every wait (sync) proves the
  // stream has consumed the ring, so it starts over there.
  static constexpr uint64_t kTableBytes = 256;
  static constexpr uint64_t kPinnedBytes = 4096;
  uint8_t* pinned = nullptr;
  uint64_t ring_head = kTableBytes;
  // The step-table address the bound KernelGlobals holds, read once and kept until the globals
  // are rebound or written (invalidate_globals).
  CUdeviceptr table = 0;

  std::mutex mutex;  // guards the module and function tables
  std::unordered_map<std::string, CUmodule> modules;  // embedded or dev-directory, by name
  // Development reload (0002 D14): the directory as it was at creation, and per loaded module the stamp of
  // the file that was loaded and the stamp of the last changed file that failed to load.
  struct Watch {
    ModuleStamp loaded;
    std::string unreadable;    // the filesystem's message when the file exists but cannot be read; empty: absent
    bool has_pending = false;  // a stamp that differs from `loaded`, seen at the previous poll
    ModuleStamp pending;
    bool failed = false;       // the file at `failed_stamp` could not be loaded
    ModuleStamp failed_stamp;
  };
  std::atomic<uint64_t> load_attempts{0};
  std::string dev_dir;
  std::unordered_map<std::string, Watch> watched;
  std::vector<CUmodule> file_modules;                 // test modules, by handle
  std::unordered_map<std::string, CUfunction> functions;
  std::unordered_map<std::string, uint32_t> block_threads;  // checked block size, by kernel name

  Impl() : cu(load_driver()) {}

  // Separate from the constructor so a failure part-way still runs ~Impl and gives back
  // the context, stream and event acquired so far.
  void init(uint32_t index) {
    hooks = read_test_hooks();
    dev_dir = dev_kernel_dir();
    fault_pending = hooks.gpu_fault;
    check(cu.cuInit_(0), "cuInit");
    int driver = 0;
    check(cu.cuDriverGetVersion_(&driver), "cuDriverGetVersion");
    check(cu.cuDeviceGet_(&device, static_cast<int>(index)), "cuDeviceGet");
    int major = 0, minor = 0;
    check(cu.cuDeviceGetAttribute_(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, device),
          "cuDeviceGetAttribute");
    check(cu.cuDeviceGetAttribute_(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, device),
          "cuDeviceGetAttribute");
    // Test hooks pretend another driver or GPU, so the refusal and the warning run on this machine.
    if (hooks.driver_version != 0) driver = static_cast<int>(hooks.driver_version);
    if (hooks.compute_capability != 0) {
      major = static_cast<int>(hooks.compute_capability / 10);
      minor = static_cast<int>(hooks.compute_capability % 10);
    }
    arch = "sm_" + std::to_string(major) + std::to_string(minor);
    // Ref: 0001 D3.5 (R590 floor, compute capability floor), D2.8; 0002 D15 "Driver too old, unsupported
    // GPU": the context is refused, with the found and required driver and the GPU architecture.
    const astroray::DeviceStatus status = astroray::cuda_device_status(driver, major, minor);
    if (status.status != AR_OK)
      throw DeviceError("GPU device unavailable: " + status.reason + " (GPU " + arch + ")");
    check(cu.cuDevicePrimaryCtxRetain_(&ctx, device), "cuDevicePrimaryCtxRetain");
    bind();
    // Observes a sticky error left by any other user of the primary context: the call
    // returns that error and the constructor refuses with its message (0001 D2.5).
    check(cu.cuCtxSynchronize_(), "cuCtxSynchronize");
    check(cu.cuStreamCreate_(&stream, CU_STREAM_NON_BLOCKING), "cuStreamCreate");
    check(cu.cuEventCreate_(&done, CU_EVENT_DISABLE_TIMING), "cuEventCreate");
    // 4 KiB of page-locked host memory. Not a ledger record: the ledger accounts device memory
    // (0007 d14), and this is host memory.
    void* host = nullptr;
    check(cu.cuMemAllocHost_(&host, kPinnedBytes), "cuMemAllocHost");
    pinned = static_cast<uint8_t*>(host);
  }

  // Drops the cached step-table address when [p, p + bytes) touches the bound KernelGlobals.
  void invalidate_globals(CUdeviceptr p, uint64_t bytes) {
    if (globals && p < globals + sizeof(ar::KernelGlobals) && globals < p + bytes) table = 0;
  }

  ~Impl() {
    if (timed_launches != 0)
      std::fprintf(stderr, "astroray: longest launch %.3f ms (%s) over %llu launches\n", longest_launch_ms,
                   longest_launch_name.c_str(), static_cast<unsigned long long>(timed_launches));
    // Teardown cannot throw; a failure is printed, never dropped.
    if (!ctx) return;
    note(cu.cuCtxSetCurrent_(ctx), "cuCtxSetCurrent");
    traversal.reset();  // waits for its launches; the owner detaches it first, while the scene lives
    if (stream) note(cu.cuEventRecord_(done, stream), "cuEventRecord");
    if (done) note(cu.cuEventSynchronize_(done), "cuEventSynchronize");
    if (pinned) note(cu.cuMemFreeHost_(pinned), "cuMemFreeHost");
    for (auto& m : modules) note(cu.cuModuleUnload_(m.second), "cuModuleUnload");
    for (CUmodule m : file_modules) note(cu.cuModuleUnload_(m), "cuModuleUnload");
    if (done) note(cu.cuEventDestroy_(done), "cuEventDestroy");
    if (stream) note(cu.cuStreamDestroy_(stream), "cuStreamDestroy");
    note(cu.cuDevicePrimaryCtxRelease_(device), "cuDevicePrimaryCtxRelease");
  }

  static void note(CUresult r, const char* call) {
    if (r != CUDA_SUCCESS) std::fprintf(stderr, "astroray: CUDA teardown: %s failed with %d\n", call, static_cast<int>(r));
  }

  [[noreturn]] void fail(CUresult r, const char* call, const char* detail) {
    const char* name = nullptr;
    const char* text = nullptr;
    cu.cuGetErrorName_(r, &name);
    cu.cuGetErrorString_(r, &text);
    const std::string message = cuda_failure_message(static_cast<int>(r), name, text, call, detail);
    const bool sticky = cuda_result_is_sticky(static_cast<int>(r));
    if (sticky) {
      dead = true;
      dead_message = message;
    }
    throw DeviceError(message, sticky);
  }

  void check(CUresult r, const char* call, const char* detail = nullptr) {
    if (r != CUDA_SUCCESS) fail(r, call, detail);
  }

  void ensure_alive() const {
    if (dead) throw DeviceError(dead_message, true);
  }

  // The context is per thread; every public operation binds it first.
  void bind() {
    ensure_alive();
    check(cu.cuCtxSetCurrent_(ctx), "cuCtxSetCurrent");
  }

  // The per-step event wait of 0002 D15: where an asynchronous error surfaces.
  // `kernel`, when given, is named in the error: the debug wait after a launch (D15).
  void sync(const char* kernel = nullptr) {
    bind();
    check(cu.cuEventRecord_(done, stream), "cuEventRecord", kernel);
    check(cu.cuEventSynchronize_(done), "cuEventSynchronize", kernel);
    ring_head = kTableBytes;  // the stream has consumed every staged upload
  }

  void require_globals() const {
    if (!globals) throw std::logic_error("CudaDevice: no KernelGlobals bound (call set_kernel_globals first)");
  }

  CUmodule load_image(const std::string& label, const ModuleImage& image) {
    CUmodule m = nullptr;
    const CUresult r = cu.cuModuleLoadData_(&m, image.bytes.data());
    // 0002 D15 "No module image for this GPU": a missing or damaged image makes the device unavailable
    // and the message names the architecture, whatever the driver calls the defect.
    if (r == CUDA_ERROR_NO_BINARY_FOR_GPU || r == CUDA_ERROR_INVALID_IMAGE || r == CUDA_ERROR_INVALID_PTX ||
        r == CUDA_ERROR_UNSUPPORTED_PTX_VERSION || r == CUDA_ERROR_INVALID_SOURCE) {
      const char* name = nullptr;
      cu.cuGetErrorName_(r, &name);
      throw DeviceError("GPU device unavailable: no usable module image for " + arch + ": " + label + " (" +
                        image.origin + "): " + (name ? name : "CUDA error"));
    }
    check(r, "cuModuleLoadData", label.c_str());
    return m;
  }

  // The function `fn` of the named module, loading the module on first use.
  CUfunction named_function(const std::string& module, const std::string& fn) {
    std::lock_guard<std::mutex> lock(mutex);
    const std::string key = module + "/" + fn;
    auto f = functions.find(key);
    if (f != functions.end()) return f->second;
    auto m = modules.find(module);
    if (m == modules.end()) {
      Watch w;
      if (!dev_dir.empty() && !module_file_stamp(module, dev_dir, &w.loaded))
        throw DeviceError("kernel module file for " + module + " is missing from " + dev_dir);
      m = modules.emplace(module, load_image(module, load_module_image(module, hooks.corrupt_cubin, dev_dir))).first;
      if (!dev_dir.empty()) watched[module] = w;  // stat before the read: a later edit differs from it
    }
    CUfunction func = nullptr;
    check(cu.cuModuleGetFunction_(&func, m->second, fn.c_str()), "cuModuleGetFunction", fn.c_str());
    functions.emplace(key, func);
    return func;
  }

  // launch_config.h's T of `kernel`, checked once against the compiled function's limit (a
  // kernel built under __launch_bounds__(T, B) admits exactly T, the probe build more).
  uint32_t threads_of(const std::string& kernel, CUfunction f) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = block_threads.find(kernel);
    if (it != block_threads.end()) return it->second;
    int limit = 0;
    check(cu.cuFuncGetAttribute_(&limit, CU_FUNC_ATTRIBUTE_MAX_THREADS_PER_BLOCK, f), "cuFuncGetAttribute", kernel.c_str());
    const uint32_t t = checked_launch_threads(kernel, launch_row(kernel), limit);
    block_threads.emplace(kernel, t);
    return t;
  }

  void launch(CUfunction f, const char* name, uint32_t blocks, uint32_t threads, void** params) {
    const auto t0 = std::chrono::steady_clock::now();
    check(cu.cuLaunchKernel_(f, blocks, 1, 1, threads, 1, 1, 0, stream, params, nullptr), "cuLaunchKernel", name);
#ifndef NDEBUG
    sync(name);  // a debug build waits after every launch so an error names its kernel (D15)
#endif
    note_launch(name, t0);
  }

  // With ASTRORAY_TEST_LAUNCH_TIMING: waits for the launch that began at `t0` and keeps the longest.
  void note_launch(const char* name, std::chrono::steady_clock::time_point t0) {
    if (!hooks.launch_timing) return;
    sync(name);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    ++timed_launches;
    if (ms > longest_launch_ms) {
      longest_launch_ms = ms;
      longest_launch_name = name;
    }
  }

  // Ref: 0001 V14 (gate-only fault hook: an out-of-bounds kernel write). Once per device, before the
  // first real launch, runs test_fault.cu's write far past the bound KernelGlobals allocation and waits:
  // the sticky error surfaces through sync() exactly as a stage's would.
  void inject_fault_once() {
    if (!fault_pending) return;
    fault_pending = false;
    require_globals();
    bind();
    const ModuleImage image = load_module_image("test_fault");  // gate-hook builds only, loose (ASTRORAY_KERNEL_DIR)
    const CUmodule m = load_image("test_fault", image);
    {
      std::lock_guard<std::mutex> lock(mutex);
      file_modules.push_back(m);
    }
    CUfunction f = nullptr;
    check(cu.cuModuleGetFunction_(&f, m, "ar_test_fault"), "cuModuleGetFunction", "ar_test_fault");
    CUdeviceptr base = globals;
    uint64_t index = uint64_t(1) << 40;  // 4 TiB of uint32_t past the allocation: unmapped
    void* params[] = {&base, &index};
    check(cu.cuLaunchKernel_(f, 1, 1, 1, 1, 1, 1, 0, stream, params, nullptr), "cuLaunchKernel", "ar_test_fault");
    sync("ar_test_fault");  // throws the sticky DeviceError; the device is dead for the process
    throw DeviceError("test hook ASTRORAY_TEST_GPU_FAULT: ar_test_fault did not fault");
  }

  void copy_from(void* dst, CUdeviceptr src, uint64_t bytes) {
    if (bytes == 0) return;
    bind();
    check(cu.cuMemcpyDtoHAsync_(dst, src, bytes, stream), "cuMemcpyDtoHAsync");
    sync();
  }
};

CudaDevice::CudaDevice(Ledger& ledger, uint32_t device_index)
    : ModuleDevice(ledger), impl_(std::make_unique<Impl>()) {
  impl_->init(device_index);
}

CudaDevice::~CudaDevice() = default;

std::unique_ptr<ModuleDevice> make_cuda_device(Ledger& ledger, uint32_t device_index) {
  return std::make_unique<CudaDevice>(ledger, device_index);
}

void CudaDevice::upload(const DeviceBuffer& dst, uint64_t offset, const void* src, uint64_t bytes) {
  check_range(dst, offset, bytes);
  if (bytes == 0) return;
  Impl& s = *impl_;
  s.bind();
  s.invalidate_globals(reinterpret_cast<CUdeviceptr>(dst.ptr) + offset, bytes);
  s.check(s.cu.cuMemcpyHtoDAsync_(reinterpret_cast<CUdeviceptr>(dst.ptr) + offset, src, bytes, s.stream),
          "cuMemcpyHtoDAsync");
  s.sync();
}

void CudaDevice::upload_async(const DeviceBuffer& dst, uint64_t offset, const void* src, uint64_t bytes) {
  check_range(dst, offset, bytes);
  if (bytes == 0) return;
  Impl& s = *impl_;
  constexpr uint64_t ring = Impl::kPinnedBytes - Impl::kTableBytes;
  if (bytes > ring)
    throw std::invalid_argument("CudaDevice::upload_async: " + number(bytes) + " bytes exceed the " + number(ring) +
                                "-byte pinned ring (use upload)");
  s.bind();
  uint64_t pos = (s.ring_head + 15u) / 16u * 16u;
  if (pos + bytes > Impl::kPinnedBytes) {
    s.sync();  // the ring is full: wait once, it starts over
    pos = Impl::kTableBytes;
  }
  std::memcpy(s.pinned + pos, src, bytes);
  s.ring_head = pos + bytes;
  const CUdeviceptr d = reinterpret_cast<CUdeviceptr>(dst.ptr) + offset;
  s.invalidate_globals(d, bytes);
  s.check(s.cu.cuMemcpyHtoDAsync_(d, s.pinned + pos, bytes, s.stream), "cuMemcpyHtoDAsync");
}

void CudaDevice::download(void* dst, const DeviceBuffer& src, uint64_t offset, uint64_t bytes) {
  check_range(src, offset, bytes);
  impl_->copy_from(dst, reinterpret_cast<CUdeviceptr>(src.ptr) + offset, bytes);
}

void CudaDevice::memset(const DeviceBuffer& dst, uint64_t offset, uint8_t value, uint64_t bytes) {
  check_range(dst, offset, bytes);
  if (bytes == 0) return;
  Impl& s = *impl_;
  s.bind();
  s.invalidate_globals(reinterpret_cast<CUdeviceptr>(dst.ptr) + offset, bytes);
  s.check(s.cu.cuMemsetD8Async_(reinterpret_cast<CUdeviceptr>(dst.ptr) + offset, value, bytes, s.stream),
          "cuMemsetD8Async");
  s.sync();
}

void CudaDevice::copy(const DeviceBuffer& dst, uint64_t dst_offset, const DeviceBuffer& src, uint64_t src_offset,
                      uint64_t bytes) {
  check_range(dst, dst_offset, bytes);
  check_range(src, src_offset, bytes);
  if (bytes == 0) return;
  Impl& s = *impl_;
  s.bind();
  s.invalidate_globals(reinterpret_cast<CUdeviceptr>(dst.ptr) + dst_offset, bytes);
  // Stream-ordered with the launches, no wait (device.h); an error surfaces at the next wait.
  s.check(s.cu.cuMemcpyDtoDAsync_(reinterpret_cast<CUdeviceptr>(dst.ptr) + dst_offset,
                                  reinterpret_cast<CUdeviceptr>(src.ptr) + src_offset, bytes, s.stream),
          "cuMemcpyDtoDAsync");
}

void CudaDevice::set_kernel_globals(const DeviceBuffer& buffer) {
  if (!buffer.ptr || buffer.bytes < sizeof(ar::KernelGlobals))
    throw std::invalid_argument("CudaDevice: the KernelGlobals buffer holds " + number(buffer.bytes) + " bytes, need " +
                                number(sizeof(ar::KernelGlobals)));
  impl_->globals = reinterpret_cast<CUdeviceptr>(buffer.ptr);
  impl_->globals_buffer = buffer;
  impl_->table = 0;
}

void CudaDevice::launch(ar::Stage stage, uint32_t grid) {
  Impl& s = *impl_;
  s.require_globals();
  const uint32_t i = static_cast<uint32_t>(stage);
  if (i >= ar::kStageCount) throw std::invalid_argument("CudaDevice: stage " + number(i) + " does not exist");
  s.inject_fault_once();
  if (stage == ar::Stage::TERMINATED || stage == ar::Stage::SHADOW_TERMINATED)
    throw std::invalid_argument("CudaDevice: stage " + number(i) + " is not launchable");
  const std::string name = kStageNames[i];
  if (stage == ar::Stage::INTERSECT_CLOSEST || stage == ar::Stage::INTERSECT_SHADOW) {
    // OptiX raygens (0002 D8, D11), never k_ modules.
    if (!s.traversal)
      throw std::logic_error("CudaDevice: stage " + name + " runs on the OptiX traversal pipeline and none is attached");
    if (!s.traversable)
      throw std::logic_error("CudaDevice: stage " + name + " has no traversable (set_traversable with the snapshot's accel)");
    s.bind();
    // The raygen strides over the step table's count like a kernel; `grid` only sizes the launch.
    const uint32_t width = grid == 0 ? 1u : grid;
    const auto t0 = std::chrono::steady_clock::now();
    if (stage == ar::Stage::INTERSECT_CLOSEST)
      s.traversal->launch_closest(s.traversable, s.globals_buffer, width);
    else
      s.traversal->launch_shadow(s.traversable, s.globals_buffer, width);
#ifndef NDEBUG
    s.sync(name.c_str());  // as Impl::launch: an error names its stage (D15)
#endif
    s.note_launch(name.c_str(), t0);
    return;
  }
  if (stage == ar::Stage::INTERSECT_SUBSURFACE)
    throw std::invalid_argument("CudaDevice: stage " + name + " has no CUDA route (its OptiX raygen is not built)");
  s.bind();
  const std::string fn = "k_" + name;
  checked_launch_threads(fn, launch_row(fn), -1);  // the row first, before any module is looked up
  const CUfunction f = s.named_function(name, fn);
  const uint32_t threads = s.threads_of(fn, f);
  // The kernel is grid-stride over the step table's count, read on the device (0002 D4,
  // D6): `grid` only sizes the grid, so a stale or zero value never drops items.
  const uint32_t blocks = blocks_for(grid, threads);
  CUdeviceptr kg = s.globals;
  void* params[] = {&kg};
  s.launch(f, name.c_str(), blocks, threads, params);
}

TraversalPipeline::Times CudaDevice::attach_traversal(AccelBuilder& scene_accel, Owner owner) {
  Impl& s = *impl_;
  auto* optix = dynamic_cast<SceneOptixBuilder*>(&scene_accel);
  if (!optix) throw std::invalid_argument("CudaDevice: the scene's acceleration builder is not OptiX (a CPU scene?)");
  if (s.traversal) throw std::logic_error("CudaDevice: a traversal pipeline is already attached");
  s.bind();
  s.traversal = std::make_unique<TraversalPipeline>(optix->optix_context(), *this, s.stream, owner);
  return s.traversal->times();
}

void CudaDevice::detach_traversal() {
  Impl& s = *impl_;
  s.traversable = 0;
  if (!s.traversal) return;
  // The pipeline goes whatever happens: on a dead device bind() throws, and a pipeline left attached
  // would outlive the scene's OptiX context. Its destructor does not throw; the bind error is
  // reported after it ran.
  std::exception_ptr failed;
  try {
    s.bind();
  } catch (...) {
    failed = std::current_exception();
  }
  s.traversal.reset();
  if (failed) std::rethrow_exception(failed);
}

void CudaDevice::poison_for_test(const std::string& why) {
  impl_->dead = true;
  impl_->dead_message = DeviceError::sticky_text(why);
}

void CudaDevice::set_traversable(uint64_t handle) { impl_->traversable = handle; }

namespace {
CudaDevice& as_cuda(Device& d) {
  auto* c = dynamic_cast<CudaDevice*>(&d);
  if (!c) throw std::invalid_argument("CUDA traversal: the session's device is not a CudaDevice");
  return *c;
}
}  // namespace

TraversalAttach attach_cuda_traversal(Device& session_device, AccelBuilder& scene_accel, Owner owner) {
  const TraversalPipeline::Times t = as_cuda(session_device).attach_traversal(scene_accel, owner);
  return TraversalAttach{t.module_ms, t.pipeline_ms};
}
void detach_cuda_traversal(Device& session_device) { as_cuda(session_device).detach_traversal(); }
void set_cuda_traversable(Device& session_device, uint64_t handle) { as_cuda(session_device).set_traversable(handle); }

void CudaDevice::launch(KernelId kernel, uint32_t count) {
  Impl& s = *impl_;
  s.require_globals();
  const uint32_t i = static_cast<uint32_t>(kernel);
  if (i >= kKernelIdCount) throw std::invalid_argument("CudaDevice: kernel " + number(i) + " does not exist");
  s.inject_fault_once();
  s.bind();
  const std::string name = kKernelNames[i];
  const std::string fn = "k_" + name;
  checked_launch_threads(fn, launch_row(fn), -1);  // the row first, before any module is looked up
  const CUfunction f = s.named_function(name, fn);
  const uint32_t threads = s.threads_of(fn, f);
  const uint32_t blocks = blocks_for(count, threads);
  CUdeviceptr kg = s.globals;
  void* params[] = {&kg};
  s.launch(f, name.c_str(), blocks, threads, params);
}

void CudaDevice::read_step_table(ar::StepTable& out) {
  Impl& s = *impl_;
  s.require_globals();
  if (!s.table) {
    CUdeviceptr table = 0;
    s.copy_from(&table, s.globals + offsetof(ar::KernelGlobals, step_table), sizeof(table));
    if (!table) throw std::logic_error("CudaDevice: the bound KernelGlobals has no step table");
    s.table = table;
  }
  static_assert(sizeof(ar::StepTable) <= Impl::kTableBytes, "the readback area holds the step table");
  s.bind();
  s.check(s.cu.cuMemcpyDtoHAsync_(s.pinned, s.table, sizeof(out), s.stream), "cuMemcpyDtoHAsync");
  s.sync();
  std::memcpy(&out, s.pinned, sizeof(out));
}

void CudaDevice::synchronize() { impl_->sync(); }

std::string CudaDevice::untested_architecture() const {
  // 0001 D3.3: sm_120 is the only tested hardware; this build has no PTX JIT path to time.
  return impl_->arch == "sm_120" ? std::string() : impl_->arch;
}

// The report subject of a module: its place in the fixed module tables (stages, then scheduler and film
// kernels), so one KERNEL_MODULE_FALLBACK entry belongs to one module (0002 D8, D15).
static uint64_t module_subject(const std::string& name) {
  for (uint32_t i = 0; i < ar::kStageCount; ++i)
    if (name == kStageNames[i]) return 1u + i;
  for (uint32_t i = 0; i < kKernelIdCount; ++i)
    if (name == kKernelNames[i]) return 1000u + i;
  throw std::logic_error("CudaDevice: module " + name + " is in neither module table");
}

uint64_t CudaDevice::module_load_attempts() const { return impl_->load_attempts.load(); }

ModuleReload CudaDevice::poll_module_reload() {
  Impl& s = *impl_;
  ModuleReload out;
  if (s.dev_dir.empty()) return out;
  s.bind();
  std::lock_guard<std::mutex> lock(s.mutex);
  // A missing file has this stamp, so it goes through the same two-poll settling as an edit.
  const ModuleStamp missing{~0ull, 0};
  for (auto& kv : s.watched) {
    const std::string& name = kv.first;
    Impl::Watch& w = kv.second;
    ModuleStamp now;
    if (!module_file_stamp(name, s.dev_dir, &now, &w.unreadable)) now = missing;
    if (now == w.loaded) {  // the file matches the running image again
      w.has_pending = false;
      if (w.failed) out.recovered.push_back({name, module_subject(name), {}});
      w.failed = false;
      continue;
    }
    // A rebuild writes the file in place, in pieces: load only a stamp seen at two consecutive polls.
    if (!w.has_pending || w.pending != now) {
      w.has_pending = true;
      w.pending = now;
      continue;
    }
    if (w.failed && now == w.failed_stamp) continue;  // already reported; wait for the next edit
    ++s.load_attempts;
    try {
      if (now == missing)
        throw DeviceError(w.unreadable.empty() ? "kernel module file is missing from " + s.dev_dir
                                               : "kernel module file cannot be read: " + w.unreadable);
      const ModuleImage image = load_module_image(name, s.hooks.corrupt_cubin, s.dev_dir);
      // The loader's own result, short: the report detail holds 128 bytes.
      CUmodule fresh = nullptr;
      const CUresult lr = s.cu.cuModuleLoadData_(&fresh, image.bytes.data());
      if (lr != CUDA_SUCCESS) {
        if (cuda_result_is_sticky(static_cast<int>(lr))) s.fail(lr, "cuModuleLoadData", name.c_str());
        const char* text = nullptr;
        s.cu.cuGetErrorName_(lr, &text);
        throw DeviceError(std::string("cuModuleLoadData ") + (text ? text : "failed"));
      }
      s.sync();  // nothing launched from the old image is still running
      s.check(s.cu.cuModuleUnload_(s.modules[name]), "cuModuleUnload", name.c_str());
      s.modules[name] = fresh;
      const std::string prefix = name + "/";
      for (auto f = s.functions.begin(); f != s.functions.end();) {
        if (f->first.compare(0, prefix.size(), prefix) == 0) {
          s.block_threads.erase(f->first.substr(prefix.size()));
          f = s.functions.erase(f);
        } else {
          ++f;
        }
      }
      w.loaded = now;
      w.has_pending = false;
      w.failed = false;
      out.swapped.push_back({name, module_subject(name), {}});
    } catch (const DeviceError& e) {
      if (e.sticky()) throw;
      w.failed = true;
      w.failed_stamp = now;
      out.failed.push_back({name, module_subject(name), e.what()});
    }
  }
  return out;
}

uint32_t CudaDevice::load_module_file(const std::string& path) {
  Impl& s = *impl_;
  s.bind();
  const CUmodule m = s.load_image(path, load_module_image_file(path));
  std::lock_guard<std::mutex> lock(s.mutex);
  s.file_modules.push_back(m);
  return static_cast<uint32_t>(s.file_modules.size() - 1);
}

void CudaDevice::launch_module_kernel(uint32_t module, const char* function, uint32_t blocks, uint32_t threads,
                                      void* const* args) {
  Impl& s = *impl_;
  if (blocks == 0 || threads == 0) throw std::invalid_argument("CudaDevice: a launch needs at least one block and one thread");
  s.bind();
  CUfunction f = nullptr;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    if (module >= s.file_modules.size())
      throw std::invalid_argument("CudaDevice: module handle " + number(module) + " was not loaded");
    const std::string key = "file" + number(module) + "/" + function;
    auto it = s.functions.find(key);
    if (it == s.functions.end()) {
      s.check(s.cu.cuModuleGetFunction_(&f, s.file_modules[module], function), "cuModuleGetFunction", function);
      s.functions.emplace(key, f);
    } else {
      f = it->second;
    }
  }
  s.launch(f, function, blocks, threads, const_cast<void**>(args));
}

bool CudaDevice::may_allocate(uint64_t need, uint64_t* available_bytes) {
  Impl& s = *impl_;
  s.bind();
  size_t free_bytes = 0, total_bytes = 0;
  s.check(s.cu.cuMemGetInfo_(&free_bytes, &total_bytes), "cuMemGetInfo");
  // Ref: 0007 d14 reserve rule (reserve_allows); the refusal reports the raw free bytes
  // and refusal_text adds the reserve (0002 D15: category, size and free VRAM).
  *available_bytes = free_bytes;
  return reserve_allows(need, free_bytes, kCudaReserveBytes);
}

std::string CudaDevice::refusal_text(LedgerCategory category, uint64_t need, uint64_t available) const {
  constexpr double kGiB = 1073741824.0;
  char buf[200];
  std::snprintf(buf, sizeof buf, "needs %.2f GiB for %s, %.2f GiB free (reserve %.2f GiB)",
                static_cast<double>(need) / kGiB, ledger_category_name(category), static_cast<double>(available) / kGiB,
                static_cast<double>(kCudaReserveBytes) / kGiB);
  return buf;
}

void* CudaDevice::alloc_memory(uint64_t bytes) {
  Impl& s = *impl_;
  s.bind();
  CUdeviceptr p = 0;
  const CUresult r = s.cu.cuMemAlloc_(&p, bytes);
  if (r == CUDA_ERROR_OUT_OF_MEMORY) return nullptr;  // Device::allocate throws std::bad_alloc
  s.check(r, "cuMemAlloc");
  return reinterpret_cast<void*>(p);
}

void CudaDevice::free_memory(void* ptr) {
  Impl& s = *impl_;
  // After a sticky error every call returns it and the context is lost with the process (0001 D2.5), so
  // there is nothing to give back. Film, active-pixel and table destructors free through here and cannot
  // throw: a throw would std::terminate the process that is meant to stay up on its CPU device. The failure
  // itself was reported when the error surfaced.
  if (s.dead) return;
  s.bind();
  s.check(s.cu.cuMemFree_(reinterpret_cast<CUdeviceptr>(ptr)), "cuMemFree");
}

}  // namespace astroray::host
