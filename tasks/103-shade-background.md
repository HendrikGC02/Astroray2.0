# 103 shade_background: the constant world on camera-path misses
Goal: camera rays that miss add nothing -> `shade_background` adds the constant world radiance (067) times beta for camera-path misses and ends the path with ESCAPED and its statistics, on CPU and GPU, so an empty scene renders its world colour.
Read first: 0002 D8 (shade_background row), D10 (OTHER stack class), 0004 D5 (accumulation through the slot), D6 (world row), D28 (path-end statistics); 0003 d22 (termination); src/kernel/light/world_const.h.
Scope: src/kernel/stages/shade_background.h, src/kernel/stages/shade_common.h (path end, shared with 071), src/kernel_cpu/shade_background.cpp, src/kernel_gpu/shade_background.cu, src/kernel/launch_config.h, src/kernel/budget.toml, src/kernel/film/film_write.h (sample_mark_invalid, reused by 071), src/kernel/kernel_globals.h (world_light member), src/host/light/light_builder.{h,cpp} (the table records the world's index), src/host/session/kg_builder.cpp (copies it), src/device/cpu/cpu_device.cpp (registers the stage in make_cpu_session_device), tests/cpu/test_shade_background.cpp, tests/gpu/test_shade_background_agree.cpp.
Out of scope: MIS weighting of world hits after a BSDF bounce (074); environment maps (M1b); light-path misses (route_hit terminates them, 039).
Reference: Cycles `kernel/integrator/shade_background.h` (Apache-2.0) for stage structure.
Do:
1. shade_common.h: `path_end(kg, slot, term)` = `film_path_end_stats` (049), `sample_set_flags`, next_stage TERMINATED.
2. shade_background: Le of the world light at the path's transport wavelengths times beta, `sample_add` (weight 1 until 074), term ESCAPED, `path_end`; a NaN or Inf contribution sets term INVALID and adds nothing.
3. launch_config and budget.toml rows (T = 256, OTHER class, spills 0).
4. Tests: CPU (fast): a hand-built miss adds exactly beta * Lw to its slot and sets ESCAPED; gate: a CPU session on an empty scene with a constant RGB world (host classes, 101's wiring) resolves every pixel's display mean to the world's display RGB within 5 standard errors; GPU (gate): per-stage agreement fixture (029) on 10^5 synthetic misses (eval thresholds, integer fields bitwise).
Acceptance: `pwsh tools/check.ps1 gate` passes with `shade_background/*` on both devices; kernelgate prints shade_background with 0 spills.
Look at: none.
Canary: tasks/103.canary.patch must turn red: astroray_tests_cpu "shade_background/empty scene shows world", astroray_tests_gpu "shade_background/agreement".
Budget: one device TU; declared spills 0, stack <= 128 B.
Depends on: 102, 067, 029.
Size: S (about 200 lines); stop and report at 2x.
Progress:
- Started: world light is found by a reverse scan of the light table (no world index in KernelGlobals); registering the stage in CpuDevice is outside Scope.
- Done: stage, CPU+GPU TUs, launch+budget rows (k_shade_background 40 regs, 0 stack, 0 spills), tests, canary. Production wiring of the CPU stage (CpuDevice::register_production_stages) is outside Scope: tests register it themselves.
- Lead rulings: world index (LightTable::world_light, KernelGlobals::world_light, kg_builder) replaces the scan; CPU stage registered in make_cpu_session_device; CUDA name table already maps SHADE_BACKGROUND to k_shade_background; stack class now VM (0002 D10), row unchanged at 40 regs, 0 stack, 0 spills.
- Review fixes: non-finite miss now poisons the slot via sample_mark_invalid (film_write.h) so the resolve counts it invalid and n stays; tests for it, for s != 1 and for the light-table world index; Scope line extended.
