// SPDX-License-Identifier: Apache-2.0
// k_film_heatmap: one thread per pixel of the render sub-rectangle, grid-stride. No FP64.
// Ref: docs/decisions/0008-film-outputs-denoise.md d12; 0002 D4, D8; film/present.h.
#include "film/present.h"
#include "launch_config.h"

extern "C" __global__ void AR_LAUNCH_BOUNDS(ar::kLaunch_k_film_heatmap.threads, ar::kLaunch_k_film_heatmap.min_blocks)
    k_film_heatmap(const ar::KernelGlobals* __restrict__ kg, const ar::HeatmapParams* __restrict__ params) {
  const ar::KernelGlobals& g = *kg;
  const ar::HeatmapParams p = *params;
  for (uint32_t j = blockIdx.x * blockDim.x + threadIdx.x; j < ar::film_present_count(g); j += gridDim.x * blockDim.x)
    ar::film_heatmap_pixel(g, p, j);
}
