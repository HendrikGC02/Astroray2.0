// SPDX-License-Identifier: Apache-2.0
// Launch configuration: one row per CUDA kernel and OptiX program, the single source of its
// threads per block T, minimum resident blocks B (0 = no clause), shadow items produced per
// item k, and stack class. tools/kernelgate.py reads the rows below and fails a kernel or OptiX
// program that has no row here or none in budget.toml (which owns registers, stack, spills).
// A row is `X(name, T, B, k, CLASS)`; CLASS is VM (runs the node VM) or OTHER.
// Ref: docs/decisions/0002-kernel-architecture.md D8, D9, D10; 0009 d18.
#pragma once
#include "core/real.h"
#include "portability.h"
#include "state/stage.h"

// -DAR_LAUNCH_BOUNDS=0 builds the probe variant (0002 D10): the macro compiles out, so the
// register count shows uncapped growth. Any other value is a mistake, not a silent default.
#if defined(AR_LAUNCH_BOUNDS) && AR_LAUNCH_BOUNDS != 0
#error "AR_LAUNCH_BOUNDS may only be defined as 0 (probe build)"
#endif
#if defined(AR_LAUNCH_BOUNDS)
#undef AR_LAUNCH_BOUNDS
#define AR_LAUNCH_BOUNDS(T, B)
#elif AR_CUDA_BUILD
// B = 0 is "no minimum-blocks clause" (0002 D9); __launch_bounds__ takes 0 as unspecified.
#define AR_LAUNCH_BOUNDS(T, B) __launch_bounds__(T, B)
#else
#define AR_LAUNCH_BOUNDS(T, B)
#endif

AR_NS_BEGIN

// What optix_traverse.cu reads through its one launch-parameter variable: the traversable of the
// TLAS and the device address of the session's KernelGlobals (integers, so the host fills it
// without OptiX types). OptiX programs have no T and B: their rows carry 0 for both.
struct OptixLaunchParams {
  uint64_t traversable;
  uint64_t kernel_globals;
};

enum class StackClass : uint8_t { VM, OTHER };

struct LaunchRow {
  uint32_t threads;
  uint32_t min_blocks;
  uint32_t shadow_items;
  StackClass stack_class;
};

// Add one line per kernel in the task that adds the kernel. ar_selftest is the test module of
// tests/gpu/kernels/selftest.cu, kept here so the gate has a live row to check. ar_test_fault is
// src/kernel_gpu/test_fault.cu, a gate-hook build module (never embedded, 0001 V14). ar_probe_m1 is
// src/kernel_gpu/probe_m1.cu, the M-1 probe (0005 Risks), built by the probe target only; it
// carries shade_surface's launch bounds.
#define AR_LAUNCH_ROWS(X) \
  X(ar_selftest, 256, 0, 0, OTHER) \
  X(ar_test_fault, 32, 0, 0, OTHER) \
  X(ar_probe_m1, 256, 2, 1, VM) \
  X(k_queue_build_count, 256, 0, 0, OTHER) \
  X(k_queue_build_scan, 256, 0, 0, OTHER) \
  X(k_queue_build_scatter, 256, 0, 0, OTHER) \
  X(k_sort_count, 256, 0, 0, OTHER) \
  X(k_sort_scan, 256, 0, 0, OTHER) \
  X(k_sort_scatter, 256, 0, 0, OTHER) \
  X(k_route_hit, 256, 0, 0, OTHER) \
  X(k_film_resolve, 256, 1, 0, OTHER) \
  X(k_film_present, 256, 0, 0, OTHER) \
  X(k_film_heatmap, 256, 0, 0, OTHER) \
  X(k_film_build_active, 256, 0, 0, OTHER) \
  X(k_generate_camera, 256, 0, 0, OTHER) \
  X(k_shade_background, 256, 0, 0, VM) \
  X(k_shade_surface, 256, 2, 1, VM) \
  X(k_shade_shadow, 256, 0, 0, VM) \
  X(k_shade_light, 256, 0, 0, VM) \
  X(k_bake_environment, 256, 0, 0, VM)

// The OptiX programs of optix_traverse.cu, named as their PTX entries (tools/kernelgate.py matches
// them). No constants are generated: the names contain a double underscore, reserved in C++.
#define AR_OPTIX_LAUNCH_ROWS(X) \
  X(__raygen__intersect_closest, 0, 0, 0, OTHER) \
  X(__raygen__intersect_shadow, 0, 0, 0, OTHER) \
  X(__miss__traverse, 0, 0, 0, OTHER) \
  X(__closesthit__tri, 0, 0, 0, OTHER) \
  X(__intersection__lamp, 0, 0, 0, OTHER) \
  X(__closesthit__lamp, 0, 0, 0, OTHER)

#define AR_LAUNCH_ROW_CONST(name, T, B, K, CLASS) \
  inline constexpr LaunchRow kLaunch_##name{T, B, K, StackClass::CLASS};
AR_LAUNCH_ROWS(AR_LAUNCH_ROW_CONST)
#undef AR_LAUNCH_ROW_CONST

// Items of a pool that one queue_build tile counts and scatters in order (the "block" of 0002
// D6's block histogram), by one GPU warp or one CPU loop. PROVISIONAL; the material sort tiles
// by it too (sort.h).
inline constexpr uint32_t kQueueTile = 1024;

// queue_build_scan's look-back tile (0002 D7 remedy (c)): kQueueScanRows histogram rows, so the
// largest pools (6,144 rows) make 192 scan tiles. PROVISIONAL, not tuned; measured in
// 0002 D7 (c).
inline constexpr uint32_t kQueueScanRows = 32;
// Units that share one queue_build tile or scan tile: one GPU warp (the vote of ar::ballot is
// warp-wide). The CPU runs one unit per tile.
inline constexpr uint32_t kQueueLanes = 32;

// The shade_surface material sort runs when the segment holds at least kSortMinItems paths and
// the scene's materials run at least kSortMinMaterials distinct surface programs (RULINGS 80,
// sort.h sort_program_count); the scheduler (045) reads both.
// PROVISIONAL: set by build-phase measurement (0002 D7 step 3, Risks item 7).
inline constexpr uint32_t kSortMinItems = 65536;
inline constexpr uint32_t kSortMinMaterials = 2;

// The most shadow items k one item of a stage emits in one step (0002 D5), 0 for a stage that
// emits none. The producers are the D8 kernels that write shadow items; a stage task that needs
// more than the default 1 raises its value here, and its AR_LAUNCH_ROWS row repeats it.
AR_FN uint32_t stage_shadow_items(Stage s) {
  switch (s) {
    case Stage::SHADE_SURFACE:
    case Stage::SHADE_SURFACE_EXT:
    case Stage::SHADE_LIGHT_PATH:
    case Stage::SHADE_LIGHT_PATH_EXT:
    case Stage::SHADE_VOLUME:
      return 1;
    default:
      return 0;
  }
}

AR_NS_END
