// SPDX-License-Identifier: Apache-2.0
// film_present on the host thread pool. Not yet in the CpuDevice kernel table.
// Ref: docs/decisions/0007-blender-integration.md d29; film/present.h.
#include "../kernel/film/present.h"

void cpu_film_present(const ar::KernelGlobals& kg, uint32_t first, uint32_t last) {
  for (uint32_t j = first; j < last; ++j) ar::film_present_pixel(kg, j);
}
