// SPDX-License-Identifier: Apache-2.0
// queue_build_scan on the host thread pool.
// Ref: docs/decisions/0002-kernel-architecture.md D6 (scan, step table); stages/queue_build.h.
#include <type_traits>

#include "../kernel/stages/queue_build.h"
#include "stage_cpu.h"

static_assert(std::is_same_v<decltype(&cpu_queue_build_scan), CpuKernelFn>, "registered as a CpuKernelFn");

// One unit does the whole scan: the chunk that holds item 0 runs it, the others do nothing, so a
// host count above 1 cannot run it twice (the in-place column scan is not repeatable).
void cpu_queue_build_scan(const ar::KernelGlobals& kg, uint32_t first, uint32_t /*last*/) {
  if (first == 0) ar::queue_scan(kg);
}
