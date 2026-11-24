// SPDX-License-Identifier: Apache-2.0
#include "device/cpu/cpu_device.h"

#include <malloc.h>

#include <cstring>
#include <limits>
#include <memory>
#include <string>

#include "kernel/film/present.h"
#include "kernel/film/resolve.h"
#include "kernel/stages/bake_environment.h"
#include "kernel/stages/film_build_active.h"
#include "kernel/stages/generate_camera.h"
#include "kernel/stages/queue_build.h"
#include "kernel/stages/route_hit.h"
#include "kernel/stages/shade_background.h"
#include "kernel/stages/shade_light.h"
#include "kernel/stages/shade_shadow.h"
#include "kernel/stages/shade_surface.h"
#include "kernel/stages/shade_volume.h"
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
    case KernelId::SORT_SCAN:
      *grid = 1;
      return "SORT_SCAN";
    default:
      return nullptr;
  }
}

// The scheduler's refill launch of generate_camera: it is no Stage, so the device runs it as a
// kernel over the TERMINATED entry the scheduler rewrote for this launch (scheduler.h).
// Ref: docs/decisions/0002-kernel-architecture.md D7 step 4, D8.
void cpu_generate_camera_refill(const ar::KernelGlobals& kg, uint32_t first, uint32_t last) {
  cpu_stage_generate_camera(kg, kg.step_table->stage[static_cast<uint32_t>(ar::Stage::TERMINATED)], first, last);
}

}  // namespace

void CpuDevice::register_scheduler_kernels() {
  register_kernel(KernelId::QUEUE_BUILD_COUNT, &cpu_queue_build_count);
  register_kernel(KernelId::QUEUE_BUILD_SCAN, &cpu_queue_build_scan);
  register_kernel(KernelId::QUEUE_BUILD_SCATTER, &cpu_queue_build_scatter);
  register_kernel(KernelId::SORT_COUNT, &cpu_sort_count);
  register_kernel(KernelId::SORT_SCAN, &cpu_sort_scan);
  register_kernel(KernelId::SORT_SCATTER, &cpu_sort_scatter);
  // The per-pass film kernels the session launches after every drained pass.
  register_kernel(KernelId::FILM_RESOLVE, &cpu_film_resolve);
  register_kernel(KernelId::FILM_PRESENT, &cpu_film_present);
  register_kernel(KernelId::FILM_HEATMAP, &cpu_film_heatmap);
  // Every pass starts by writing the active-pixel list the resolve and present kernels read.
  register_kernel(KernelId::FILM_BUILD_ACTIVE, &cpu_film_build_active);
  // The environment table's grid (0004 D11); launched by host/light/env_bake.cpp when the world changes.
  register_kernel(KernelId::BAKE_ENVIRONMENT, &cpu_bake_environment);
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

// Declared in host/session/session.h. The returned pointer shares ownership of the ledger and the
// thread pool the device needs, so the session's C API handle names no device implementation.
std::shared_ptr<Device> make_cpu_session_device(ContextState& ctx, unsigned workers) {
  struct Owned {
    Ledger ledger;
    ThreadPool pool;
    CpuDevice device;
    Owned(ContextState& c, unsigned w) : pool(c, w), device(ledger, pool) {
      device.register_production_stages();
      // Not in register_production_stages: the scheduler tests register stub generate kernels.
      device.register_kernel(KernelId::GENERATE_CAMERA, &cpu_generate_camera_refill);
      // Not in register_production_stages either: the camera_wiring tests stack their own stub.
      device.register_stage(ar::Stage::SHADE_BACKGROUND, &cpu_stage_shade_background);
      device.register_stage(ar::Stage::SHADE_SURFACE, &cpu_stage_shade_surface);
      device.register_stage(ar::Stage::SHADE_SHADOW, &cpu_stage_shade_shadow);
      device.register_stage(ar::Stage::SHADE_LIGHT, &cpu_stage_shade_light);
      device.register_stage(ar::Stage::SHADE_VOLUME, &cpu_stage_shade_volume);
    }
  };
  auto owned = std::make_shared<Owned>(ctx, workers);
  return std::shared_ptr<Device>(owned, &owned->device);
}

}  // namespace astroray::host
