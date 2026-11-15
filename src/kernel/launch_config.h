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
  X(ar_selftest, 256, 0, 0, OTHER)

#define AR_LAUNCH_ROW_CONST(name, T, B, K, CLASS) \
  inline constexpr LaunchRow kLaunch_##name{T, B, K, StackClass::CLASS};
AR_LAUNCH_ROWS(AR_LAUNCH_ROW_CONST)
#undef AR_LAUNCH_ROW_CONST

AR_NS_END
