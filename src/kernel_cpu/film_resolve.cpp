// SPDX-License-Identifier: Apache-2.0
// film_resolve on the host thread pool.
// Ref: docs/decisions/0008-film-outputs-denoise.md d3; film/resolve.h.
#include <type_traits>

#include "../kernel/film/resolve.h"
#include "stage_cpu.h"

static_assert(std::is_same_v<decltype(&cpu_film_resolve), CpuKernelFn>, "registered as a CpuKernelFn");

void cpu_film_resolve(const ar::KernelGlobals& kg, uint32_t first, uint32_t last) {
  for (uint32_t j = first; j < last; ++j) ar::film_resolve_pixel(kg, j);
}
