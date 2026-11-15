// SPDX-License-Identifier: Apache-2.0
// k_film_build_active: one thread per listed pixel, grid-stride over the device-read count.
// Ref: docs/decisions/0008-film-outputs-denoise.md d3; 0002 D4, D8; stages/film_build_active.h.
#include "launch_config.h"
#include "stages/film_build_active.h"

extern "C" __global__ void AR_LAUNCH_BOUNDS(ar::kLaunch_k_film_build_active.threads,
                                            ar::kLaunch_k_film_build_active.min_blocks)
    k_film_build_active(const ar::KernelGlobals* __restrict__ kg) {
  const ar::KernelGlobals& g = *kg;
  for (uint32_t j = blockIdx.x * blockDim.x + threadIdx.x; j < ar::film_build_active_count(g);
       j += gridDim.x * blockDim.x)
    ar::film_build_active_pixel(g, j);
}
