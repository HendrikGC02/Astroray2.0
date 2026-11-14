# 105 Lamp primitives in Embree and OptiX
Goal: camera and BSDF rays cannot hit lamps -> the lamp instances of 068 get primitives in both acceleration structures (rectangle as two triangles, disk as a custom primitive, sphere as a built-in sphere), the hit record identifies the lamp, and no shadow ray is ever blocked by one.
Read first: 0002 D11 (lamp intersection program, built-in spheres, mask, SBT per primitive type), 0004 D8; src/device/cpu/embree_scene.h; src/device/cuda/optix_accel.h, optix_pipeline.h.
Scope: src/device/cpu/embree_scene.cpp, src/kernel/geom/lamp_intersect.h, src/kernel_gpu/optix_traverse.cu (lamp hit group, `__intersection__lamp`, `__closesthit__lamp`), src/device/cuda/optix_accel.cpp, src/device/cuda/optix_pipeline.cpp, src/kernel/budget.toml, tests/cpu/test_lamp_prims.cpp, tests/gpu/test_lamp_traversal.cpp.
Out of scope: shading a lamp hit (075); spot and sun (M4); motion (M8).
Reference: OptiX 9.1 Programming Guide (custom primitives, built-in spheres through `optixBuiltinISModuleGet`); Embree 4 API (`RTC_GEOMETRY_TYPE_USER`, `RTC_GEOMETRY_TYPE_SPHERE_POINT`) from RenderKit/embree (Apache-2.0).
Do:
1. lamp_intersect.h (kernel, shared): purely geometric ray-disk intersection; one-sidedness is left to shading.
2. Embree: triangles for rectangles, user geometry calling lamp_intersect for disks, `RTC_GEOMETRY_TYPE_SPHERE_POINT` for spheres, each instanced with its lamp instance's mask.
3. OptiX: triangles, a custom-primitive `__intersection__lamp` (disk), built-in spheres, a `__closesthit__lamp` writing the hit record; one SBT record per primitive type; budget.toml rows for the new programs (`gated = "full"`).
4. Tests: CPU (fast): disk and sphere hits at analytic t (1e-6); GPU (gate): OptiX and Embree return the same lamp hits (instance exact, t within 1e-5 relative) for 10^5 rays over a scene with each lamp type, and a shadow ray whose only blocker is a lamp is never occluded.
Acceptance: `pwsh tools/check.ps1 gate` passes with `lamp_prims/*` and `lamp_traversal/*`.
Look at: none.
Canary: tasks/105.canary.patch must turn red: astroray_tests_gpu "lamp_traversal/lamps never block shadows", astroray_tests_cpu "lamp_prims/disk analytic t".
Budget: the OptiX module grows by one intersection and one closest-hit program; reported in `full`.
Depends on: 068, 041, 037.
Size: M (about 300 lines); stop and report at 2x.
Progress:
