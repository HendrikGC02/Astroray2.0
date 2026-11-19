// SPDX-License-Identifier: Apache-2.0
// The vendor seam: the host Device interface, implemented once by CpuDevice and once
// by CudaDevice (028). Host C++ only, no CUDA types. The scheduler is written against
// it; dispatch is per launch, never per item. allocate and free are not virtual: they
// are where the ledger is applied, so no device can allocate around it; a device
// supplies the three protected hooks.
// Ref: docs/decisions/0001-product-shape.md D5.2 (Device seam); 0002 D7 (two-method
// device interface), D8 (per-pass kernels), D13 (accounting); 0007 d14 (ledger).
#pragma once
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "host/ledger.h"
#include "kernel/kernel_globals.h"
#include "kernel/state/stage.h"

namespace astroray::host {

// The kernels the host launches outside the step loop (0002 D8's per-pass and scene
// kernels) and the scheduler's own (D6 queue build and sort, D7 refill). Stage values
// are launched through the Stage overload.
enum class KernelId : uint8_t {
  QUEUE_BUILD_COUNT,
  QUEUE_BUILD_SCAN,
  QUEUE_BUILD_SCATTER,
  SORT_COUNT,
  SORT_SCATTER,
  GENERATE_CAMERA,
  GENERATE_LIGHT,
  FILM_RESOLVE,
  FILM_ADAPTIVE,
  FILM_PRESENT,
  FILM_HEATMAP,
  FILM_PREPARE_DENOISE,
  FILM_PACK_DLSS,
  BAKE_ENVIRONMENT,
  FILM_BUILD_ACTIVE,  // per pass, before the refill (0002 D8)
  SORT_SCAN,          // between SORT_COUNT and SORT_SCATTER (sort.h); last so earlier ids keep their value
};
inline constexpr uint32_t kKernelIdCount = static_cast<uint32_t>(KernelId::SORT_SCAN) + 1u;

// A device allocation. A plain value: free() zeroes the one it is given, so a stale
// copy is a double free that the ledger rejects.
struct DeviceBuffer {
  void* ptr = nullptr;  // device address (a host address on the CPU device)
  uint64_t bytes = 0;
  uint64_t ledger_id = 0;
  LedgerCategory category = LedgerCategory::GEOMETRY;
};

// Throws std::out_of_range unless [offset, offset + bytes) lies inside the buffer.
inline void check_range(const DeviceBuffer& b, uint64_t offset, uint64_t bytes) {
  if (offset > b.bytes || bytes > b.bytes - offset)
    throw std::out_of_range("device buffer range [" + std::to_string(offset) + ", +" + std::to_string(bytes) +
                            ") is outside the " + std::to_string(b.bytes) + "-byte buffer");
}

// An AllocationRefused whose message the device wrote (it knows facts the ledger's
// generic text does not, such as the reserve). Status, category and sizes are the base's.
class DeviceAllocationRefused : public AllocationRefused {
 public:
  DeviceAllocationRefused(LedgerCategory category, uint64_t need_bytes, uint64_t available_bytes, std::string text)
      : AllocationRefused(category, need_bytes, available_bytes), text_(std::move(text)) {}
  const char* what() const noexcept override { return text_.c_str(); }

 private:
  std::string text_;
};

// What a development build's step-boundary check found (0002 D14, D15): modules whose file changed and now
// run the new image (`swapped`), modules whose changed file failed to load so the previous image keeps
// running (`failed`, once per changed file), and modules whose failing file is gone again because the file
// matches the running image (`recovered`). `subject` is the module's fixed id for report entries.
struct ModuleReload {
  struct Entry {
    std::string module;
    uint64_t subject = 0;
    std::string error;  // `failed` only
  };
  std::vector<Entry> swapped, recovered, failed;
};

class Device {
 public:
  explicit Device(Ledger& ledger) : ledger_(ledger) {}
  virtual ~Device() = default;
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  Ledger& ledger() const { return ledger_; }

  // Asks may_allocate, allocates, records the allocation in the ledger. Throws
  // std::invalid_argument for zero bytes and AllocationRefused (AR_ERR_OUT_OF_MEMORY)
  // when the device refuses; a failure of the allocator itself is std::bad_alloc.
  // Neither leaves a ledger record.
  DeviceBuffer allocate(LedgerCategory category, uint64_t bytes, Owner owner) {
    if (bytes == 0) throw std::invalid_argument("device allocation of zero bytes");
    uint64_t available = 0;
    if (!may_allocate(bytes, &available)) {
      std::string text = refusal_text(category, bytes, available);
      if (text.empty()) throw AllocationRefused(category, bytes, available);
      throw DeviceAllocationRefused(category, bytes, available, std::move(text));
    }
    void* ptr = alloc_memory(bytes);
    if (!ptr) throw std::bad_alloc();
    DeviceBuffer b;
    b.ptr = ptr;
    b.bytes = bytes;
    b.category = category;
    try {
      b.ledger_id = ledger_.add(owner, category, bytes);
    } catch (...) {
      free_memory(ptr);
      throw;
    }
    return b;
  }
  // Releases the allocation and its ledger record and zeroes the buffer. A buffer
  // with no pointer is ignored; a copy of a freed buffer throws std::invalid_argument
  // before anything is released.
  void free(DeviceBuffer& buffer) {
    if (!buffer.ptr) return;
    ledger_.remove(buffer.ledger_id);
    free_memory(buffer.ptr);
    buffer = DeviceBuffer();
  }

  virtual void upload(const DeviceBuffer& dst, uint64_t offset, const void* src, uint64_t bytes) = 0;
  virtual void download(void* dst, const DeviceBuffer& src, uint64_t offset, uint64_t bytes) = 0;
  virtual void memset(const DeviceBuffer& dst, uint64_t offset, uint8_t value, uint64_t bytes) = 0;
  // Device-to-device copy of `bytes` from src+src_offset to dst+dst_offset (ordered with
  // launches on the device's stream, no host round trip; the ledger is unchanged). Ranges of
  // one buffer must not overlap.
  virtual void copy(const DeviceBuffer& dst, uint64_t dst_offset, const DeviceBuffer& src, uint64_t src_offset,
                    uint64_t bytes) = 0;

  // The session's KernelGlobals, already uploaded into `buffer`. Every launch passes
  // this one pointer to the kernel (0002 D4).
  virtual void set_kernel_globals(const DeviceBuffer& buffer) = 0;
  // Runs the stage over its whole segment: the item count is the device-resident
  // StepEntry::count, never `grid`, which only sizes a GPU grid (0002 D4, D6, D7).
  // A partial shadow-producer launch (D5) is a smaller step count written by the scan.
  virtual void launch(ar::Stage stage, uint32_t grid) = 0;
  // Runs a per-pass or scheduler kernel over its items. Like a stage, the kernel takes only
  // the KernelGlobals pointer and reads its item count from there; `count` only sizes a GPU
  // grid (0002 D4, D8).
  virtual void launch(KernelId kernel, uint32_t count) = 0;
  // Copies the device's step table (0002 D7 step 2).
  virtual void read_step_table(ar::StepTable& out) = 0;
  // Returns when every launched kernel has finished.
  virtual void synchronize() = 0;
  virtual uint32_t kind() const = 0;  // AR_DEVICE_CPU or AR_DEVICE_CUDA
  // The GPU architecture ("sm_86") when the device runs code the runtime tests do not cover (0002 D15,
  // 0001 D3.3: only sm_120 is tested), else empty. The session raises UNTESTED_DEVICE once from it.
  virtual std::string untested_architecture() const { return {}; }
  // Called by the scheduler at each step boundary, before the step's first launch (a test may call it
  // while the render thread is paused): swaps in the stage
  // modules rebuilt in the development kernel directory. Nothing to do on a device without loose
  // modules (CPU stages are rebuilt and restarted, 0002 D14).
  virtual ModuleReload poll_module_reload() { return {}; }

 protected:
  // The device's reserve rule (0007 d14, R19; the CUDA side is 028): true when `need`
  // bytes may be allocated now. When it returns false, *available_bytes is what the
  // refusal reports.
  virtual bool may_allocate(uint64_t need, uint64_t* available_bytes) = 0;
  // The refusal's message, or empty for the ledger's generic "needs X GiB for <category>,
  // Y GiB available".
  virtual std::string refusal_text(LedgerCategory, uint64_t /*need*/, uint64_t /*available*/) const { return {}; }
  // Raw memory for allocate(); nullptr means the allocator failed.
  virtual void* alloc_memory(uint64_t bytes) = 0;
  virtual void free_memory(void* ptr) = 0;

 private:
  Ledger& ledger_;
};

// A failure of the device itself: a CUDA call that failed, a missing module image. It
// reaches the C ABI as AR_ERR_DEVICE (0002 D15). `sticky` marks the errors CUDA documents
// as needing a process restart; the device is then dead for the process (0001 D2.5).
class DeviceError : public std::runtime_error {
 public:
  explicit DeviceError(const std::string& what, bool sticky = false) : std::runtime_error(what), sticky_(sticky) {}
  ar_status status() const noexcept { return AR_ERR_DEVICE; }
  bool sticky() const noexcept { return sticky_; }
  // The message of 0001 D2.5 for a sticky error whose CUDA text is `text`.
  static std::string sticky_text(const std::string& text) {
    return "GPU error: " + text + ". Save your work and restart Blender.";
  }

 private:
  bool sticky_;
};

// What the GPU device adds to the Device interface for tests: loose test modules (never
// embedded) loaded from an explicit file and launched with raw arguments. It lives here
// so tests reach it without including a CUDA path (lint section `seam`).
class ModuleDevice : public Device {
 public:
  using Device::Device;
  // Loads the module image at `path`; returns a handle for launch_module_kernel.
  virtual uint32_t load_module_file(const std::string& path) = 0;
  // Launches extern "C" `function` of a loaded module: `args` holds one pointer per kernel
  // parameter, each to the parameter's value (cuLaunchKernel's kernelParams). Asynchronous:
  // call synchronize().
  virtual void launch_module_kernel(uint32_t module, const char* function, uint32_t blocks, uint32_t threads,
                                    void* const* args) = 0;
};

// R of 0007 d14: bytes that must stay free after any allocation, for Blender and the
// desktop: the headroom Blender can still grow into (P5: Material Preview on B-5M used 1.56 GiB more than
// idle, plus 25% = 1.95 GiB, rounded up; 0007 d14).
inline constexpr uint64_t kCudaReserveBytes = 2147483648ull;  // 2 GiB

// The reserve rule of 0007 d14 as a pure function: an allocation of `need` bytes proceeds
// only if need + reserve <= free, free being what cuMemGetInfo reports at that moment.
// Written so no sum can overflow.
inline bool reserve_allows(uint64_t need, uint64_t free_bytes, uint64_t reserve) {
  return free_bytes >= reserve && need <= free_bytes - reserve;
}

// The message of a CUDA failure (0002 D15): which call, for which kernel when there is one,
// and CUDA's own name and text. A sticky result (cuda_result_is_sticky) gets the 0001 D2.5
// wording, with the call and kernel inside its <text>. `kernel` may be null or empty.
// Defined in cuda_device.cpp.
std::string cuda_failure_message(int cu_result, const char* name, const char* text, const char* call,
                                 const char* kernel);

// Defined in src/device/cuda/cuda_device.cpp, which exists only in ASTRORAY_GPU builds.
// Retains device `device_index`'s primary context; throws DeviceError when CUDA cannot
// be used, including after a sticky error elsewhere in the process.
std::unique_ptr<ModuleDevice> make_cuda_device(Ledger& ledger, uint32_t device_index);
// True for the CUDA driver results that need a process restart (illegal address, launch
// failure, launch timeout, ECC and the other hardware-fault codes); takes the numeric
// CUresult so callers need no CUDA header.
bool cuda_result_is_sticky(int cu_result);
// cudaDeviceSynchronize then cudaGetLastError: the text of a latent error, or empty.
std::string cuda_latent_error();

}  // namespace astroray::host
