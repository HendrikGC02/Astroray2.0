# 040 OptiX acceleration structures and device scene data
Goal: the GPU has no scene -> each generation uploads mesh data in the shared kernel layout, builds one compacted GAS per geometry key in batches and rebuilds the TLAS with instance masks and flags, all through the ledger.
Read first: 0002 D11 (instance flags, DISABLE_ANYHIT for opaque instances, SBT per primitive type), D13 (scene data layout, ledger), 0007 d11 (compacted GAS per key, batches, TLAS always rebuilt), d14 (reserve rule); src/kernel/scene/scene_view.h.
Scope: src/device/cuda/optix_context.{h,cpp}, src/device/cuda/optix_accel.{h,cpp}, src/device/cuda/scene_upload.{h,cpp}, cmake/deps.cmake (OptiX headers from the fetched cache, include path for src/device/cuda only), tests/gpu/test_optix_accel.cpp.
Out of scope: the OptiX module, pipeline and raygens (041); lamp primitives (068); motion TLAS (M8).
Reference: OptiX 9.1 Programming Guide, acceleration structure build and compaction; Cycles `src/bvh/optix.cpp` (Apache-2.0) and pbrt-v4 `src/pbrt/gpu/optix/aggregate.cpp` (Apache-2.0) as patterns.
Do:
1. optix_context: OptiX function table from the fetched headers (`optix_function_table_definition.h` in exactly one TU), `optixInit`, one `OptixDeviceContext` per scene on its CUDA context, created on the scene's init thread (032); failures follow 0002 D15 (GPU device unavailable for the session, message with the OptiX log).
2. scene_upload: mesh positions, triangle corner indices, corner normals, UVs and per-triangle material slots into flat device arrays in the same layout scene_view.h reads on the CPU; instance records and material table pool likewise; all allocations tagged geometry or tables in the ledger.
3. optix_accel: per geometry key a triangle GAS with `PREFER_FAST_TRACE | ALLOW_COMPACTION`, built in batches from one temporary arena with one stream sync per batch, then compacted; GAS persist across edits and are rebuilt only for changed keys; one TLAS per generation over all instances with transform, mask (task 036), `OPTIX_INSTANCE_FLAG_DISABLE_ANYHIT` (all materials are opaque in M1a), SBT offset for the triangle hit group; always rebuilt, never refit.
4. test_optix_accel.cpp (gate): build a scene of 3 meshes and 10 instances; ledger acceleration bytes equal the sum of compacted sizes plus TLAS; a transform update rebuilds only the TLAS; removing a key frees its GAS (ledger returns to baseline +- 0).
Acceptance: `pwsh tools/check.ps1 gate` passes with `optix_accel/*` cases; the test prints GAS build time per 10^6 triangles for a 1M-triangle grid (compare with spike B's 2.9 ms in Progress).
Look at: none.
Canary: tasks/040.canary.patch must turn red: astroray_tests_gpu "optix_accel/remove frees gas".
Budget: none on kernels (host code).
Depends on: 028, 035, 036.
Size: M (about 450 lines); stop and report at 2x.
Progress:
- GAS build+compaction of a 1,002,528-triangle grid: 3.57 ms per 10^6 (spike B 2.9 ms, build only). Gate cases optix_accel/* pass run directly under the machine lock.
- scene_view.h does not exist yet (039 pending): the device layout is DeviceMeshView/DeviceInstanceRecord in scene_upload.h; 039 should read those.
- Needed outside Scope: CMakeLists.txt (2 lines calling deps.cmake); tools/lint.py rejects optix_*.h names and tests/gpu including device/cuda headers.
