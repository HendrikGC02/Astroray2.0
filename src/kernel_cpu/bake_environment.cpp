// SPDX-License-Identifier: Apache-2.0
// bake_environment on the host thread pool.
// Ref: docs/decisions/0004-integrator.md D11; stages/bake_environment.h.
#include <type_traits>

#include "../kernel/stages/bake_environment.h"
#include "stage_cpu.h"

static_assert(std::is_same_v<decltype(&cpu_bake_environment), CpuKernelFn>, "registered as a CpuKernelFn");

void cpu_bake_environment(const ar::KernelGlobals& kg, uint32_t first, uint32_t last) {
  for (uint32_t cell = first; cell < last; ++cell) ar::bake_environment_write(kg, cell);
}
