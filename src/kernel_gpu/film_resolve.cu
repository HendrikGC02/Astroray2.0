// SPDX-License-Identifier: Apache-2.0
// k_film_resolve: one thread per active pixel, grid-stride over the device-read count.
// The only FP64 work is the moment accumulation of 0008 d3 (cmake lists this module in
// AR_GPU_F64_MODULES).
// Ref: docs/decisions/0008-film-outputs-denoise.md d3; 0002 D4, D8; film/resolve.h.
#include "film/resolve.h"
#include "launch_config.h"

extern "C" __global__ void AR_LAUNCH_BOUNDS(ar::kLaunch_k_film_resolve.threads, ar::kLaunch_k_film_resolve.min_blocks)
    k_film_resolve(const ar::KernelGlobals* __restrict__ kg) {
  const ar::KernelGlobals& g = *kg;
  for (uint32_t j = blockIdx.x * blockDim.x + threadIdx.x; j < ar::film_resolve_count(g); j += gridDim.x * blockDim.x)
    ar::film_resolve_pixel(g, j);
}
