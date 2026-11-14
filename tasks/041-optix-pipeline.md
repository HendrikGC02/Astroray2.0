# 041 OptiX module, pipeline, raygens and k_route_hit
Goal: the GPU cannot trace -> one traversal-only PTX module with the closest and shadow raygens, miss and triangle closest-hit programs is created asynchronously with the disk cache, the motion variant compiles once for measurement, and `route_hit` also exists as a plain CUDA kernel for the fixture test and register gate.
Read first: 0002 D11 (programs, pipeline options, PTX format, async creation, disk cache), D8 (k_route_hit), D10 (OptiX rows in `check full`), R18 (no material, medium or VM code in OptiX), 0007 d45 (cache_dir), R47.
Scope: src/kernel_gpu/optix_traverse.cu, src/kernel_gpu/k_route_hit.cu, src/device/cuda/optix_pipeline.{h,cpp}, cmake/kernels.cmake (PTX rule, embedding), src/kernel/launch_config.h, src/kernel/budget.toml, tests/gpu/test_optix_trace.cpp, tests/gpu/test_route_hit_gpu.cpp.
Out of scope: lamp programs (068); `__raygen__intersect_subsurface`, curves and spheres (M7); any-hit recording (M2); using the motion variant (M8).
Reference: OptiX 9.1 Programming Guide (programs, SBT, pipeline options); Cycles `kernel/device/optix/kernel.cu` (Apache-2.0) for structure.
Do:
1. optix_traverse.cu, compiled with nvcc `-ptx` (same flags as cubins otherwise) and embedded as RCDATA: `__raygen__intersect_closest` (grid-stride over its step-table segment, `optixTrace` with the item's ray-type mask and `DISABLE_ANYHIT`, writes the hit record, calls route_hit, writes next_stage); `__raygen__intersect_shadow` (`TERMINATE_ON_FIRST_HIT | DISABLE_CLOSESTHIT | DISABLE_ANYHIT`, SHADOW mask, up to tmax; occluded -> SHADOW_TERMINATED, else SHADE_SHADOW); `__miss__traverse` (inst = ~0u); `__closesthit__tri` (t, primitive, instance, barycentrics). Nothing else runs in these programs.
2. optix_pipeline: module and pipeline created on the scene init thread with `OptixModuleCompileOptions` defaults for release, the cache directory from `ar_scene_desc.cache_dir` (`optixDeviceContextSetCacheLocation`), traversable graph flags `ALLOW_SINGLE_LEVEL_INSTANCING | ALLOW_SINGLE_GAS`, primitive type triangle, `maxTraceDepth = 1`, no SER; one SBT record per program, hit group per primitive type. A second pipeline with motion blur enabled and `ALLOW_MOTION` is created by `create_motion_variant()`, called only by the measurement in task 094.
3. k_route_hit.cu: `route_hit` over a fixture array as a CUDA kernel; launch_config and budget.toml rows for k_route_hit (merge gate) and for every OptiX program (`gated = "full"`).
4. Tests (gate): test_route_hit_gpu.cpp compares k_route_hit with the CPU route_hit over every fixture case; test_optix_trace.cpp runs both raygens on the 040 test scene with a hand-filled step table and compares hit records and next stages with Embree's within 1e-5 relative t (exact instance and primitive).
Acceptance: `pwsh tools/check.ps1 gate` passes; kernelgate lists `k_route_hit` with its registers and spills; the test prints module and pipeline creation times warm and with an empty cache directory.
Look at: none.
Canary: tasks/041.canary.patch must turn red: astroray_tests_gpu "route_hit_gpu/matches cpu", "optix_trace/shadow occluded".
Budget: k_route_hit spills 0 and stack <= 128 B (declared); OptiX module registers reported in `check full`.
Depends on: 040, 039.
Size: M (about 450 lines); stop and report at 2x.
Progress:
