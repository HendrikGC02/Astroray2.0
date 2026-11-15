# 073 shade_surface (part 2): BSDF sampling, Russian roulette and continuation; A1
Goal: paths stop at the first vertex -> shade_surface samples the lobe stack, updates beta, r_u and r_l in the rescaled balance form, applies Russian roulette and the total bounce limit, advances the event index and spawns the next ray with the offset rule, and the Lambertian furnace A1 passes on CPU and GPU.
Read first: 0004 D1 (estimator, rescaled-probability form, f excludes cosine), D2 (Russian roulette), D3 (bounce semantics; total limit in M1a), D14 (event index), 0003 d6 (surface decisions use lane reductions), d23 (per-event dimension block), 0002 D16 as written by task 038 (ray offset), 0009 d20 (A1 truth).
Scope: src/kernel/stages/shade_surface.h, src/kernel/stages/shade_common.h, src/kernel/launch_config.h, src/kernel/budget.toml, scenes/a1_furnace/meta.toml, tools/check.ps1 (A1 in gate), tests/cpu/test_shade_surface_bsdf.cpp, tests/gpu/test_shade_surface_agree.cpp.
Out of scope: NEE and emission MIS (074; until then emission hit by a BSDF ray has weight 1 and the world is reached only by escaping rays); per-type bounce limits (M2); alpha (M1b).
Reference: pbrt-v4 `src/pbrt/cpu/integrators.cpp` `VolPathIntegrator::Li` (Apache-2.0) for the r_u/r_l update and Russian roulette.
Do:
1. Sample the lobe stack (064) with dims 8+12e (direction u, v; lobe choice), at the shading frame from N; reject reflected samples below the geometric surface (term ZERO_THROUGHPUT); beta *= f |cos| / pdf per lane, r_u updated per 0004 D1 (delta samples set LAST_BOUNCE_SPECULAR); store prev_ng, prev_ns, prev_dist for 074.
2. Russian roulette at depth >= max(2, min bounces): q = max(0, 1 - max_lane(beta * eta_scale / avg(r_u))), survivors weighted 1/(1-q), uniform from the event block; term RUSSIAN_ROULETTE.
3. Total bounce limit from settings (term MAX_BOUNCE); event_index += 1 at every scattering vertex; bounces[total] and the matching type counter += 1; new ray from `offset_ray_origin` with ray_type from the sampled lobe (DIFFUSE or GLOSSY mask bits); next_stage INTERSECT_CLOSEST.
4. Tests: CPU (fast): one hand-built vertex gives the expected beta and r_u for a Lambertian sample; Russian roulette weight on a fixed uniform; gate: the per-stage agreement fixture extended to throughput (p99.9 thresholds of 0002 D12) and direction; A1 (a1_furnace) passes the analytic harness on CPU and GPU (0008 V1 first scene); an A1 variant with Russian roulette disabled gives the same mean within the harness threshold.
5. From 071: sessions never set SchedulerConfig::material_header_count, so the material sort (044) never runs; set it from the pinned scene-table set at each latch, and show in a counter that SHADE_SURFACE items are sorted once more than one material is present.
Acceptance: `pwsh tools/check.ps1 gate` passes with A1 on both devices; kernelgate shows shade_surface's spill and stack delta against task 071's baseline (declared: lobe stack adds stack, spills stay within the VM allowance).
Look at: out/evidence/073/a1.png (32x32 enlarged).
Canary: tasks/073.canary.patch must turn red: `tools/analytic.py --scene a1_furnace --device cpu` (the patch drops the cosine factor), astroray_tests_cpu "shade_surface_bsdf/lambert beta".
Budget: declared shade_surface delta (spills, stack); gate time +5 s.
Depends on: 072, 064, 038.
Size: M (about 350 lines); stop and report at 2x.
Progress:
- 073 implementer: stopped before code; out-of-Scope files and the vacuous A1 RR-off variant reported to the lead.
