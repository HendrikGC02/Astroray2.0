// SPDX-License-Identifier: Apache-2.0
// The CPU entry points of stages and per-pass kernels. AR_CPU_STAGE(name) defines the
// chunk function of stage_<name> (src/kernel/stages/<name>.h): the host thread pool
// calls it once per chunk of 1,024 items, so dispatch costs one call per chunk, never
// per item. CpuDevice keeps a per-device table of these pointers; there is no
// registry of static initialisers (no process-global state).
// Ref: docs/decisions/0002-kernel-architecture.md D4 (CPU: AR_CPU_STAGE loops over the
// same segment in chunks of 1,024 items on the host thread pool).
#pragma once
#include <cstdint>

#include "../kernel/kernel_globals.h"

// A stage's chunk function: items [first, last) of its segment. `step` is the
// stage's entry of the step table; the stage function gets the item's index within
// its launch and reads the pool index from the queue itself (D4, D6).
using CpuStageFn = void (*)(const ar::KernelGlobals& kg, const ar::StepEntry& step, uint32_t first, uint32_t last);

// A per-pass or scheduler kernel's chunk function: items [first, last).
using CpuKernelFn = void (*)(const ar::KernelGlobals& kg, uint32_t first, uint32_t last);

#define AR_CPU_STAGE(name)                                                                              \
  void cpu_stage_##name(const ar::KernelGlobals& kg, const ar::StepEntry& step, uint32_t first,         \
                        uint32_t last) {                                                                \
    for (uint32_t item = first; item < last; ++item) ar::stage_##name(kg, item, step);                  \
  }
