# 049 Statistics channels and deterministic AOV channels
Goal: the film has only colour -> sample count, bounce count (mean and max), invalid count and relative error exist as 0008 defines them, the deterministic primary-hit AOVs (position, normal, material id, UV) needed by the agreement ladder exist, and invalid samples reach the report.
Read first: 0008 d10 (primary-hit AOVs, `AOV_WRITTEN`), d12 (statistics table, reset rule, defaults), d6 (invalid samples), 0003 d21 (`AOV_WRITTEN` bit), 0004 D28 (path-end statistics); 0001 D2.11 (`INVALID_SAMPLE`).
Scope: src/kernel/film/film_write.h (stat and AOV helpers), src/host/film/film_layout.{h,cpp}, src/kernel/film/resolve.h (rel error, invalid counter), src/host/film/film_stats.{h,cpp}, tests/cpu/test_film_stats.cpp, tests/cpu/test_film_resolve.cpp, src/kernel/sample/keys.h (relative includes only)
Out of scope: writing them from shading stages (071 and 073 call these helpers); heat-map display (051); Blender passes beyond Combined (M1b, M8).
Reference: Cycles `intern/cycles/kernel/film/data_passes.h` (Apache-2.0): read which data passes accumulate and which keep only sample 0, and quote the rule in a code comment.
Do:
1. Channels: `sample count` (COUNT, shared with the MOMENT n), `bounces` (ADD mean plus COUNT max), `invalid` (COUNT, always), `rel error` derived at readback (n >= 2: max over display channels of sqrt(Var[mean_c]) / max(|mean_c|, 1e-6); n < 2: -1).
2. Path-end helper `film_path_end_stats(kg, slot, bounces_total, term)`: atomic add and atomic max per pixel of the slot.
3. AOV channels position, normal (world, shading), material id and UV, written by the surface stage at the primary hit once across pass-throughs (`AOV_WRITTEN`); accumulation versus sample-0-only follows data_passes.h as quoted (expected: material id and position from the first sample only, so ids stay exact).
4. The resolve's invalid counter for the pass becomes one `INVALID_SAMPLE` report entry per film (count = samples not written), raised by the host after each resolve, cleared on film reset.
5. Tests (fast, synthetic slot data through the resolve of 047): with adaptive off, sample count equals spp exactly per pixel; three NaN slots give invalid = 3 and one INVALID_SAMPLE entry with count 3; rel error is -1 where n < 2; bounce mean and max for a synthetic set; material id AOV unchanged across 4 samples with different ids after the first (sample-0 rule).
Acceptance: `pwsh tools/check.ps1 fast` passes with `film_stats/*` cases; GPU agreement of these helpers is covered when the stages run (073, 077).
Look at: none.
Canary: tasks/049.canary.patch must turn red: astroray_tests_cpu "film_stats/invalid counted and reported", "film_stats/sample count equals spp".
Budget: none.
Depends on: 047, 016.
Size: S (about 250 lines); stop and report at 2x.
Progress:
- 2026-11-15 049: stats channels, AOV helpers, InvalidSampleReporter, film_stats tests; fast green, canary red on both named cases. Cycles data_passes.h unavailable, sample-0 split follows the task's expectation.
