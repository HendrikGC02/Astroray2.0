// SPDX-License-Identifier: Apache-2.0
#include "device/cpu/cpu_device.h"

#include <malloc.h>

#include <cstring>
#include <limits>
#include <string>

#include "kernel/stages/queue_build.h"
#include "kernel/stages/route_hit.h"
#include "kernel/stages/sort.h"

namespace astroray::host {
namespace {

std::string number(uint64_t v) { return std::to_string(v); }

// The tile grid a tiled kernel needs (queue_build.h, sort.h), 0 for any other kernel.
// Ref: docs/decisions/0002-kernel-architecture.md D4, D6; stages/queue_build.h, stages/sort.h host contract.
const char* tiled_grid(const ar::KernelGlobals& kg, KernelId kernel, uint32_t* grid) {
  switch (kernel) {
    case KernelId::QUEUE_BUILD_COUNT:
    case KernelId::QUEUE_BUILD_SCATTER:
      *grid = ar::queue_build_grid(kg.step_table->high_water[static_cast<uint32_t>(ar::Pool::MAIN)],
                                   kg.step_table->high_water[static_cast<uint32_t>(ar::Pool::SHADOW)]);
      return kernel == KernelId::QUEUE_BUILD_COUNT ? "QUEUE_BUILD_COUNT" : "QUEUE_BUILD_SCATTER";
    case KernelId::QUEUE_BUILD_SCAN:
      *grid = 1;
      return "QUEUE_BUILD_SCAN";
    case KernelId::SORT_COUNT:
    case KernelId::SORT_SCATTER:
      *grid = ar::sort_grid(ar::sort_segment_count(kg));
      return kernel == KernelId::SORT_COUNT ? "SORT_COUNT" : "SORT_SCATTER";
    default:
      return nullptr;
  }
}

}  // namespace

void CpuDevice::register_scheduler_kernels() {
  register_kernel(KernelId::QUEUE_BUILD_COUNT, &cpu_queue_build_count);
  register_kernel(KernelId::QUEUE_BUILD_SCAN, &cpu_queue_build_scan);
  register_kernel(KernelId::QUEUE_BUILD_SCATTER, &cpu_queue_build_scatter);
  register_kernel(KernelId::SORT_COUNT, &cpu_sort_count);
  register_kernel(KernelId::SORT_SCATTER, &cpu_sort_scatter);
}

void CpuDevice::register_intersect_stages() {
  register_stage(ar::Stage::INTERSECT_CLOSEST, &cpu_stage_intersect_closest);
  register_stage(ar::Stage::INTERSECT_SHADOW, &cpu_stage_intersect_shadow);
}

void CpuDevice::register_stage(ar::Stage stage, CpuStageFn fn) {
  const uint32_t i = static_cast<uint32_t>(stage);
  if (!fn) throw std::invalid_argument("CpuDevice: null entry point for stage " + number(i));
  if (i >= ar::kStageCount || stage == ar::Stage::TERMINATED || stage == ar::Stage::SHADOW_TERMINATED)
    throw std::invalid_argument("CpuDevice: stage " + number(i) + " is not launchable");
  if (stages_[i]) throw std::logic_error("CpuDevice: stage " + number(i) + " is already registered");
  stages_[i] = fn;
}

void CpuDevice::register_kernel(KernelId kernel, CpuKernelFn fn) {
  const uint32_t i = static_cast<uint32_t>(kernel);
  if (!fn) throw std::invalid_argument("CpuDevice: null entry point for kernel " + number(i));
  if (i >= kKernelIdCount) throw std::invalid_argument("CpuDevice: kernel " + number(i) + " does not exist");
  if (kernels_[i]) throw std::logic_error("CpuDevice: kernel " + number(i) + " is already registered");
  kernels_[i] = fn;
}

void CpuDevice::upload(const DeviceBuffer& dst, uint64_t offset, const void* src, uint64_t bytes) {
  check_range(dst, offset, bytes);
  std::memcpy(static_cast<char*>(dst.ptr) + offset, src, bytes);
}

void CpuDevice::download(void* dst, const DeviceBuffer& src, uint64_t offset, uint64_t bytes) {
  check_range(src, offset, bytes);
  std::memcpy(dst, static_cast<const char*>(src.ptr) + offset, bytes);
}

void CpuDevice::memset(const DeviceBuffer& dst, uint64_t offset, uint8_t value, uint64_t bytes) {
  check_range(dst, offset, bytes);
  std::memset(static_cast<char*>(dst.ptr) + offset, value, bytes);
}

void CpuDevice::copy(const DeviceBuffer& dst, uint64_t dst_offset, const DeviceBuffer& src, uint64_t src_offset,
                     uint64_t bytes) {
  check_range(dst, dst_offset, bytes);
  check_range(src, src_offset, bytes);
  std::memcpy(static_cast<char*>(dst.ptr) + dst_offset, static_cast<const char*>(src.ptr) + src_offset, bytes);
}

void CpuDevice::set_kernel_globals(const DeviceBuffer& buffer) {
  if (!buffer.ptr || buffer.bytes < sizeof(ar::KernelGlobals))
    throw std::invalid_argument("CpuDevice: the KernelGlobals buffer holds " + number(buffer.bytes) +
                                " bytes, need " + number(sizeof(ar::KernelGlobals)));
  globals_ = static_cast<const ar::KernelGlobals*>(buffer.ptr);
}

const ar::KernelGlobals& CpuDevice::globals() const {
  if (!globals_) throw std::logic_error("CpuDevice: no KernelGlobals bound (call set_kernel_globals first)");
  return *globals_;
}

void CpuDevice::launch(ar::Stage stage, uint32_t /*grid*/) {
  const ar::KernelGlobals& kg = globals();
  const uint32_t i = static_cast<uint32_t>(stage);
  if (i >= ar::kStageCount) throw std::invalid_argument("CpuDevice: stage " + number(i) + " does not exist");
  const CpuStageFn fn = stages_[i];
  if (!fn) throw std::runtime_error("CpuDevice: no entry point registered for stage " + number(i));
  if (!kg.step_table) throw std::logic_error("CpuDevice: the bound KernelGlobals has no step table");
  const ar::StepEntry& step = kg.step_table->stage[i];
  // The segment is the device-resident step entry's count; the host's grid value only
  // sizes a GPU grid (0002 D4, D6), so it never limits the items run here.
  pool_.parallel_for(step.count, kDefaultChunk,
                     [&kg, &step, fn](uint32_t first, uint32_t last) { fn(kg, step, first, last); });
}

void CpuDevice::launch(KernelId kernel, uint32_t count) {
  const ar::KernelGlobals& kg = globals();
  const uint32_t i = static_cast<uint32_t>(kernel);
  if (i >= kKernelIdCount) throw std::invalid_argument("CpuDevice: kernel " + number(i) + " does not exist");
  const CpuKernelFn fn = kernels_[i];
  if (!fn) throw std::runtime_error("CpuDevice: no entry point registered for kernel " + number(i));
  uint32_t grid = 0;
  if (kg.step_table)
    if (const char* name = tiled_grid(kg, kernel, &grid))
      if (count < grid)
        throw std::runtime_error(std::string("CpuDevice: ") + name + " launched with count " + number(count) +
                                 ", its tile grid is " + number(grid) + " (a short launch skips tiles)");
  pool_.parallel_for(count, kDefaultChunk, [&kg, fn](uint32_t first, uint32_t last) { fn(kg, first, last); });
}

void CpuDevice::read_step_table(ar::StepTable& out) {
  const ar::KernelGlobals& kg = globals();
  if (!kg.step_table) throw std::logic_error("CpuDevice: the bound KernelGlobals has no step table");
  out = *kg.step_table;
}

bool CpuDevice::may_allocate(uint64_t, uint64_t* available_bytes) {
  // Host memory: the OS decides, and a failure surfaces as std::bad_alloc.
  *available_bytes = std::numeric_limits<uint64_t>::max();
  return true;
}

void* CpuDevice::alloc_memory(uint64_t bytes) {
  if (bytes > std::numeric_limits<size_t>::max()) return nullptr;
  return _aligned_malloc(static_cast<size_t>(bytes), kCpuAlignment);
}

void CpuDevice::free_memory(void* ptr) { _aligned_free(ptr); }

}  // namespace astroray::host
