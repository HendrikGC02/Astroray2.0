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
};
inline constexpr uint32_t kKernelIdCount = static_cast<uint32_t>(KernelId::BAKE_ENVIRONMENT) + 1u;

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
    if (!may_allocate(bytes, &available)) throw AllocationRefused(category, bytes, available);
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

  // The session's KernelGlobals, already uploaded into `buffer`. Every launch passes
  // this one pointer to the kernel (0002 D4).
  virtual void set_kernel_globals(const DeviceBuffer& buffer) = 0;
  // Runs the stage over its whole segment: the item count is the device-resident
  // StepEntry::count, never `grid`, which only sizes a GPU grid (0002 D4, D6, D7).
  // A partial shadow-producer launch (D5) is a smaller step count written by the scan.
  virtual void launch(ar::Stage stage, uint32_t grid) = 0;
  // Runs `count` items of a per-pass or scheduler kernel.
  virtual void launch(KernelId kernel, uint32_t count) = 0;
  // Copies the device's step table (0002 D7 step 2).
  virtual void read_step_table(ar::StepTable& out) = 0;
  // Returns when every launched kernel has finished.
  virtual void synchronize() = 0;
  virtual uint32_t kind() const = 0;  // AR_DEVICE_CPU or AR_DEVICE_CUDA

 protected:
  // The device's reserve rule (0007 d14, R19; the CUDA side is 028): true when `need`
  // bytes may be allocated now. When it returns false, *available_bytes is what the
  // refusal message reports.
  virtual bool may_allocate(uint64_t need, uint64_t* available_bytes) = 0;
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
