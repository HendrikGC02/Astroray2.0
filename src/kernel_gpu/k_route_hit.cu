// SPDX-License-Identifier: Apache-2.0
// k_route_hit: route_hit as a plain CUDA kernel over the path-state slots [0, count), for the
// fixture test and the per-merge register gate. The OptiX raygens run the same function
// inside the driver-compiled module, which has no per-merge gate (0002 D10).
// Ref: docs/decisions/0002-kernel-architecture.md D8 (k_route_hit), D10, D11 (route_hit).
#include "stages/route_hit.h"

extern "C" __global__ void AR_LAUNCH_BOUNDS(ar::kLaunch_k_route_hit.threads, ar::kLaunch_k_route_hit.min_blocks)
    k_route_hit(const ar::KernelGlobals* __restrict__ kg, uint32_t count) {
  const ar::KernelGlobals& g = *kg;
  for (uint32_t slot = blockIdx.x * blockDim.x + threadIdx.x; slot < count; slot += gridDim.x * blockDim.x) {
    const ar::Real2 uv = ar::ps_hit_uv(g, slot);
    const ar::HitRecord h{ar::ps_hit_t(g, slot), ar::ps_hit_prim(g, slot), ar::ps_hit_inst(g, slot), uv.x, uv.y};
    ar::ps_next_stage(g, slot) = static_cast<uint8_t>(ar::route_hit(g, h, ar::ps_ray_d(g, slot), ar::ps_flags(g, slot)));
  }
}
