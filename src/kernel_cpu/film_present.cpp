// SPDX-License-Identifier: Apache-2.0
// film_present on the host thread pool. Takes a launch parameter beside the KernelGlobals, so it
// is called directly rather than through CpuKernelFn.
// Ref: docs/decisions/0007-blender-integration.md d29; film/present.h.
#include "../kernel/film/present.h"

void cpu_film_present(const ar::KernelGlobals& kg, const ar::PresentParams& p, uint32_t first, uint32_t last) {
  for (uint32_t j = first; j < last; ++j) ar::film_present_pixel(kg, p, j);
}
