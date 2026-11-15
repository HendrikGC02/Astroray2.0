// SPDX-License-Identifier: Apache-2.0
// The one OptiX module: traversal only, no material, medium or VM code (ruling 18). The two
// raygens walk their step-table segment with a grid-stride loop, trace, write the hit record or
// the shadow outcome the CPU intersect stages write, and run route_hit; nothing else is here.
// Payload: 0 t, 1 primitive, 2 instance, 3 u, 4 v (the HitRecord fields of kernel/geom/hit.h).
// Ref: docs/decisions/0002-kernel-architecture.md D11 (programs, flags, mask table, payload);
// OptiX 9.1 Programming Guide (optixTrace, payload access); Cycles kernel/device/optix/kernel.cu
// (Apache-2.0) for the program structure.
#include <optix_device.h>

#include "geom/ray_offset.h"
#include "stages/route_hit.h"

// The launch-parameter variable the pipeline names in its compile options.
extern "C" {
__constant__ ar::OptixLaunchParams ar_params;
}

namespace {

// 0002 D11 mask table: a shadow ray traces the SHADOW bit only. Equal to
// astroray::host::kShadowMaskBit (tests/gpu/test_optix_trace.cpp checks it).
constexpr uint32_t kShadowRayMask = 1u << 4;

__forceinline__ __device__ const ar::KernelGlobals& globals() {
  return *reinterpret_cast<const ar::KernelGlobals*>(static_cast<uintptr_t>(ar_params.kernel_globals));
}

}  // namespace

extern "C" __global__ void __raygen__intersect_closest() {
  const ar::KernelGlobals& kg = globals();
  const ar::StepEntry step = kg.step_table->stage[static_cast<uint32_t>(ar::Stage::INTERSECT_CLOSEST)];
  const uint3 idx = optixGetLaunchIndex(), dim = optixGetLaunchDimensions();
  for (uint32_t item = idx.x; item < step.count; item += dim.x) {
    const uint32_t slot = kg.main_queue[step.start + item];
    // ray_tmax exists only with a curved region or subsurface; a flat scene's segment is unbounded.
    const float tmax = kg.path->ray_tmax ? kg.path->ray_tmax[slot] : __int_as_float(0x7f800000);
    const ar::Real3 o = ar::ps_ray_o(kg, slot), d = ar::ps_ray_d(kg, slot);
    // A miss keeps these: t = tmax and instance = none.
    uint32_t p0 = __float_as_uint(tmax), p1 = ~0u, p2 = ar::kNoInstance, p3 = 0u, p4 = 0u;
    optixTrace(ar_params.traversable, make_float3(o.x, o.y, o.z), make_float3(d.x, d.y, d.z), ar::kRayTmin, tmax,
               0.0f, ar::ps_ray_type(kg, slot), OPTIX_RAY_FLAG_DISABLE_ANYHIT, 0, 1, 0, p0, p1, p2, p3, p4);
    const ar::HitRecord h{__uint_as_float(p0), p1, p2, __uint_as_float(p3), __uint_as_float(p4)};
    ar::ps_hit_t(kg, slot) = h.t;
    ar::ps_hit_prim(kg, slot) = h.prim;
    ar::ps_hit_inst(kg, slot) = h.inst;
    ar::ps_hit_uv(kg, slot) = ar::Real2{h.u, h.v};
    ar::ps_next_stage(kg, slot) = static_cast<uint8_t>(ar::route_hit(kg, h, ar::ps_flags(kg, slot)));
  }
}

extern "C" __global__ void __raygen__intersect_shadow() {
  const ar::KernelGlobals& kg = globals();
  const ar::StepEntry step = kg.step_table->stage[static_cast<uint32_t>(ar::Stage::INTERSECT_SHADOW)];
  const uint3 idx = optixGetLaunchIndex(), dim = optixGetLaunchDimensions();
  for (uint32_t item = idx.x; item < step.count; item += dim.x) {
    const uint32_t slot = kg.shadow_queue[step.start + item];
    const ar::Real3 o = ar::sh_ray_o(kg, slot), d = ar::sh_ray_d(kg, slot);
    // The instance payload starts as "something was hit": with TERMINATE_ON_FIRST_HIT and no
    // closest-hit program, the miss program is the only writer, and only when nothing blocks.
    uint32_t p0 = 0u, p1 = 0u, p2 = 0u, p3 = 0u, p4 = 0u;
    optixTrace(ar_params.traversable, make_float3(o.x, o.y, o.z), make_float3(d.x, d.y, d.z), ar::kRayTmin,
               ar::sh_tmax(kg, slot), 0.0f, kShadowRayMask,
               OPTIX_RAY_FLAG_TERMINATE_ON_FIRST_HIT | OPTIX_RAY_FLAG_DISABLE_CLOSESTHIT | OPTIX_RAY_FLAG_DISABLE_ANYHIT,
               0, 1, 0, p0, p1, p2, p3, p4);
    ar::sh_next_stage(kg, slot) =
        static_cast<uint8_t>(p2 != ar::kNoInstance ? ar::Stage::SHADOW_TERMINATED : ar::Stage::SHADE_SHADOW);
  }
}

extern "C" __global__ void __miss__traverse() { optixSetPayload_2(ar::kNoInstance); }

extern "C" __global__ void __closesthit__tri() {
  const float2 b = optixGetTriangleBarycentrics();
  optixSetPayload_0(__float_as_uint(optixGetRayTmax()));
  optixSetPayload_1(optixGetPrimitiveIndex());
  optixSetPayload_2(optixGetInstanceId());
  optixSetPayload_3(__float_as_uint(b.x));
  optixSetPayload_4(__float_as_uint(b.y));
}
