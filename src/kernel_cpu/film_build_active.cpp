// SPDX-License-Identifier: Apache-2.0
// film_build_active on the host thread pool.
// Ref: docs/decisions/0008-film-outputs-denoise.md d3; stages/film_build_active.h.
#include <type_traits>

#include "../kernel/stages/film_build_active.h"
#include "stage_cpu.h"

static_assert(std::is_same_v<decltype(&cpu_film_build_active), CpuKernelFn>, "registered as a CpuKernelFn");

void cpu_film_build_active(const ar::KernelGlobals& kg, uint32_t first, uint32_t last) {
  for (uint32_t j = first; j < last; ++j) ar::film_build_active_pixel(kg, j);
}
