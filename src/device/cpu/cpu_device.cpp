// SPDX-License-Identifier: Apache-2.0
#include "device/cpu/cpu_device.h"

#include <malloc.h>

#include <cstring>
#include <limits>
#include <string>

namespace astroray::host {
namespace {

std::string number(uint64_t v) { return std::to_string(v); }

}  // namespace

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
