// SPDX-License-Identifier: Apache-2.0
// k_film_prepare_denoise: one thread per pixel of the render sub-rectangle, grid-stride. It reads
// the fp64 moments, so cmake lists this module in AR_GPU_F64_MODULES.
// Ref: docs/decisions/0008-film-outputs-denoise.md d15; 0002 D4, D8; film/prepare_denoise.h.
#include "film/prepare_denoise.h"
#include "launch_config.h"

extern "C" __global__ void AR_LAUNCH_BOUNDS(ar::kLaunch_k_film_prepare_denoise.threads,
                                            ar::kLaunch_k_film_prepare_denoise.min_blocks)
    k_film_prepare_denoise(const ar::KernelGlobals* __restrict__ kg) {
  const ar::KernelGlobals& g = *kg;
  for (uint32_t j = blockIdx.x * blockDim.x + threadIdx.x; j < ar::film_prepare_denoise_count(g);
       j += gridDim.x * blockDim.x)
    ar::film_prepare_denoise_pixel(g, j);
}
