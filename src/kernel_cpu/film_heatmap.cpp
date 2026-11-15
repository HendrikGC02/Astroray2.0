// SPDX-License-Identifier: Apache-2.0
// film_heatmap on the host thread pool; see film_present.cpp for the launch parameter.
// Ref: docs/decisions/0008-film-outputs-denoise.md d12; film/present.h.
#include "../kernel/film/present.h"

void cpu_film_heatmap(const ar::KernelGlobals& kg, const ar::HeatmapParams& p, uint32_t first, uint32_t last) {
  for (uint32_t j = first; j < last; ++j) ar::film_heatmap_pixel(kg, p, j);
}
