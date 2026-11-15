# 037 Embree traversal for the CPU device
Goal: the CPU device cannot trace rays -> each generation builds one Embree scene per geometry key and an instance scene with masks, and the CPU device traces closest-hit and occlusion queries over SoA ray batches, writing the shared hit record.
Read first: 0002 D11 (hit record, mask, robust traversal, opaque shadow rule), C3 (Embree is single precision), 0001 D5.2 (Embree headers only in src/device/cpu), 0007 d11 (one structure per geometry key, TLAS always rebuilt); third_party/deps.toml (Embree entry).
Scope: src/device/cpu/embree_scene.{h,cpp}, src/device/cpu/embree_trace.{h,cpp}, src/kernel/geom/hit.h, cmake/deps.cmake (link Embree from the cache), tests/cpu/test_embree.cpp.
Out of scope: lamp primitives (068 adds them); route_hit and the intersect stages (039); OptiX (040-041); motion (M8).
Reference: Embree 4 API documentation (`rtcNewScene`, `rtcSetGeometryMask`, `RTC_SCENE_FLAG_ROBUST`, `rtcIntersect1M`/`rtcOccluded1M` or the stream variants) from RenderKit/embree (Apache-2.0).
Do:
1. hit.h (kernel, shared with OptiX): `HitRecord { Real t; uint32_t prim; uint32_t inst; Real u, v; }` with u, v the barycentric weights of corners 1 and 2 of the triangle; `inst == ~0u` means miss.
2. embree_scene: one `RTCDevice` per CPU device instance (no process-global device), created with Embree's internal tasking; per geometry key a triangle `RTCScene` built on the scene workers when the generation is prepared (robust flag), per generation an instance scene with each instance's transform and `rtcSetGeometryMask` = its 8-bit mask; host memory recorded in the ledger (acceleration category).
3. embree_trace: `trace_closest(rays SoA, mask per ray, count, HitRecord* out)` and `trace_occluded(rays, mask, tmax, count, uint8_t* occluded)`, chunked over the device thread pool; the CPU intersect stages (039) call these.
4. test_embree.cpp (fast): analytic hits on a unit quad (t, u, v to 1e-6); a miss gives `inst == ~0u`; mask fixture: one instance per mask bit 0-5, each ray type's mask hits exactly the instances sharing a bit (0002 D11 verification, CPU side); occlusion ignores instances without the SHADOW bit.
Acceptance: `pwsh tools/fetch.ps1 deps` then `pwsh tools/check.ps1 fast` passes with `embree/*` cases; `dumpbin /dependents out/bin/astroray_core.dll` still lists no tbb DLL.
Look at: none.
Canary: tasks/037.canary.patch must turn red: astroray_tests_cpu "embree/mask fixture", "embree/quad barycentrics", "embree/corner lookup".
Budget: Embree link time on the clean build; report it.
Depends on: 035, 036, 008.
Size: M (about 350 lines); stop and report at 2x.
Progress:
- 037: embree_scene/embree_trace/hit.h done; embree/* 5 cases pass; canary verified. check fast blocked only by lint seam rule matching our embree_*.h wrapper names (tools/lint.py, needs lead). CMakeLists.txt got one include(cmake/deps.cmake) line (out of scope, needed). Incremental build with Embree link 5.7 s; clean link time not measured.
