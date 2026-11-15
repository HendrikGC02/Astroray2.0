// SPDX-License-Identifier: Apache-2.0
// k_film_present: one thread per pixel of the render sub-rectangle, grid-stride. It reads the
// fp64 moments, so cmake lists this module in AR_GPU_F64_MODULES.
// Ref: docs/decisions/0007-blender-integration.md d29; 0002 D4, D8; film/present.h.
#include "film/present.h"
#include "launch_config.h"

extern "C" __global__ void AR_LAUNCH_BOUNDS(ar::kLaunch_k_film_present.threads, ar::kLaunch_k_film_present.min_blocks)
    k_film_present(const ar::KernelGlobals* __restrict__ kg) {
  const ar::KernelGlobals& g = *kg;
  for (uint32_t j = blockIdx.x * blockDim.x + threadIdx.x; j < ar::film_present_count(g); j += gridDim.x * blockDim.x)
    ar::film_present_pixel(g, j);
}
