# 039 route_hit and the CPU intersect stages
Goal: hits are not routed to stages -> the table-driven `route_hit` function decides the next stage for every hit and miss, and the CPU intersect_closest and intersect_shadow stages trace their segments with Embree and write the hit record and next stage.
Read first: 0002 D8 (kernel list, Stage enum, light-path routing), D11 (route_hit, opaque shadow outcomes, camera rays without any-hit), D4 (stage function shape), 0003 d19 rows 18-21 (hit record fields), 0005 item 17 (EXTENSION flag routing).
Scope: src/kernel/stages/route_hit.h, src/kernel/scene/scene_view.h (instance, material-table and mesh lookups for kernels), src/device/cpu/intersect_cpu.cpp, src/kernel/launch_config.h and src/kernel/budget.toml (rows, CPU stages carry none but intersect_closest/shadow rows exist for the GPU in 041), tests/cpu/test_route_hit.cpp, tests/cpu/test_intersect_cpu.cpp.
Out of scope: OptiX programs and `k_route_hit` (041); the curved-region branch (M7); any shading stage.
Reference: Cycles `kernel/integrator/intersect_closest.h` (Apache-2.0) for the routing structure.
Do:
1. scene_view.h: read-only views of the device-resident scene arrays added by 035-036 (instance records, material table pool, per-triangle material slot) through `ar::load_ro`; material flags come from the material header array added by task 059 (until then all materials are `Common`).
2. route_hit(kg, HitRecord, path flags) -> Stage: miss (`inst == ~0u`): camera path -> SHADE_BACKGROUND, light path -> TERMINATED; lamp instance (`light_index != ~0u`, 036) -> SHADE_LIGHT; material with EXTENSION -> SHADE_SURFACE_EXT (camera) or SHADE_LIGHT_PATH_EXT (light); otherwise SHADE_SURFACE (camera) or SHADE_LIGHT_PATH (light). Pure table logic, no VM, texture or BSDF code (it also runs inside OptiX raygens).
3. intersect_cpu.cpp (in src/device/cpu because it calls Embree): `stage_intersect_closest` reads its segment from the step table, traces each item's ray with its ray-type mask, writes hit_t/hit_prim/hit_inst/hit_uv and next_stage from route_hit; `stage_intersect_shadow` traces occlusion with the SHADOW mask up to tmax: occluded -> SHADOW_TERMINATED, unoccluded -> SHADE_SHADOW (delivery, task 074). Both registered as CPU stages.
4. Tests (fast): test_route_hit.cpp covers every input class above with expected Stage; test_intersect_cpu.cpp runs both stages on a two-instance scene through CpuDevice with a hand-filled step table and checks hit records and next stages.
Acceptance: `pwsh tools/check.ps1 fast` passes with `route_hit/*` and `intersect_cpu/*` cases.
Look at: none.
Canary: tasks/039.canary.patch must turn red: astroray_tests_cpu "route_hit/light path miss terminates", "intersect_cpu/occluded shadow terminates".
Budget: none (CPU only).
Depends on: 037, 038, 025.
Size: M (about 350 lines); stop and report at 2x.
Progress:
