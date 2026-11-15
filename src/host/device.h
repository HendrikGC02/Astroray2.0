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
  // Runs `count` items of the stage's segment (count <= the step table's count for it).
  virtual void launch(ar::Stage stage, uint32_t count) = 0;
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

}  // namespace astroray::host
