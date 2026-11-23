// SPDX-License-Identifier: Apache-2.0
// k_bake_environment: one thread per grid cell, grid-stride over the device-read cell count.
// Ref: docs/decisions/0004-integrator.md D11; 0002 D4, D8; stages/bake_environment.h.
#include "launch_config.h"
#include "stages/bake_environment.h"

extern "C" __global__ void AR_LAUNCH_BOUNDS(ar::kLaunch_k_bake_environment.threads,
                                            ar::kLaunch_k_bake_environment.min_blocks)
    k_bake_environment(const ar::KernelGlobals* __restrict__ kg) {
  const ar::KernelGlobals& g = *kg;
  for (uint32_t cell = blockIdx.x * blockDim.x + threadIdx.x; cell < ar::bake_environment_count(g);
       cell += gridDim.x * blockDim.x)
    ar::bake_environment_write(g, cell);
}
