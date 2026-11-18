# 075 shade_light (lamp crossings) and A4 point and sphere
Goal: lamps seen by camera and BSDF rays add nothing -> `shade_light` adds MIS-weighted front-facing lamp radiance for camera paths and re-queues the ray past the lamp without changing bounce counters or the event index, and A4 (point and sphere lamps over a Lambertian plane) passes on CPU and GPU.
Read first: 0004 D8 (lamp intersection and shade_light), D1 (MIS for lamp hits), D6 (energy conventions), D7 (pdf consistency), 0004 Verification rows D6, D8; 0009 d20 (A4 sphere and point lights).
Scope: src/kernel/stages/shade_light.h, src/kernel_cpu/shade_light.cpp, src/kernel_gpu/shade_light.cu, src/kernel/launch_config.h, src/kernel/budget.toml, scenes/a4_point/, scenes/a4_sphere/, scenes/lib/truths.py, tools/check.ps1 (gate steps), tests/cpu/test_shade_light.cpp, tests/gpu/test_shade_light_agree.cpp.
Out of scope: A3, the lit furnace, NEE on/off and stacked lamps with the NEE test hook (106); spot and sun lamps, IES and light node trees (M4); light paths through lamps (M5, they pass through already by route_hit and this stage).
Reference: Cycles `kernel/integrator/shade_light.h` (Apache-2.0).
Do:
1. shade_light: for camera paths, Le of the lamp at the hit (front face only for area lamps) times beta times the MIS weight with the previous vertex's context (074's function; weight 1 after a delta bounce or at the first vertex); `sample_add`; crossings counter += 1; re-queue INTERSECT_CLOSEST from just past the lamp (038's offset along the ray) with the same direction; bounce counters and event_index unchanged; light paths pass through with no contribution.
2. a4_point and a4_sphere: a lamp over a Lambertian plane, camera above: pixel = rho/pi * E with the closed-form irradiance (truths.py); the sphere lamp is also seen directly.
3. check.ps1 `gate` runs both scenes on CPU (`cpu` tag) and GPU at 32x32; the per-stage agreement fixture covers shade_light.
4. Tests: CPU (fast): a hand-built lamp hit adds the weighted Le once, re-queues INTERSECT_CLOSEST from past the lamp and leaves bounces and event_index unchanged.
Acceptance: `pwsh tools/check.ps1 gate` passes with both scenes on both devices; `out/bin/astroray_cli.exe render` of a4_sphere at 512x512 writes an image the lead inspects.
Look at: out/evidence/075/a4_sphere.png.
Canary: tasks/075.canary.patch must turn red: `tools/analytic.py --scene a4_sphere --device cpu` (the patch uses 4 pi r^2 instead of 4 pi^2 r^2 in the sphere's radiance), astroray_tests_cpu "shade_light/requeue keeps event index".
Budget: one VM-class device TU (lamp emission is constant in M1a); gate time +6 s.
Depends on: 074, 105.
Size: M (about 300 lines); stop and report at 2x.
Progress:
- started
- blocked before code: 3 files outside Scope are needed (cpu_device.cpp stage registration; a crossings counter home; gate membership in tools/analytic.py GATE_SCENES + tools/budgets.toml [family_members]); asked the lead.
- reading: front-facing Le applies to sphere lamps too (sphere.h, D8: outward only), so a camera ray through the sphere adds L once; shade_light adds the moved distance to prev_dist (0003 d19 row 17).
