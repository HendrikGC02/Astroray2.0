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

enum class StackClass : uint8_t { VM, OTHER };

struct LaunchRow {
  uint32_t threads;
  uint32_t min_blocks;
  uint32_t shadow_items;
  StackClass stack_class;
};

// Add one line per kernel in the task that adds the kernel. ar_selftest is the test module of
// tests/gpu/kernels/selftest.cu, kept here so the gate has a live row to check.
#define AR_LAUNCH_ROWS(X) \
  X(ar_selftest, 256, 0, 0, OTHER) \
  X(k_queue_build_count, 256, 0, 0, OTHER) \
  X(k_queue_build_scan, 256, 0, 0, OTHER) \
  X(k_queue_build_scatter, 256, 0, 0, OTHER)

#define AR_LAUNCH_ROW_CONST(name, T, B, K, CLASS) \
  inline constexpr LaunchRow kLaunch_##name{T, B, K, StackClass::CLASS};
AR_LAUNCH_ROWS(AR_LAUNCH_ROW_CONST)
#undef AR_LAUNCH_ROW_CONST

// Items of a pool that one queue_build work unit counts and scatters in order (the "block" of
// 0002 D6's block histogram). One unit is one GPU thread or one CPU loop; 1,024 is also the CPU
// chunk of D4, so a chunk is one unit. PROVISIONAL: 0002 D7 remedy (c) revisits it with B-19.
inline constexpr uint32_t kQueueTile = 1024;

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
