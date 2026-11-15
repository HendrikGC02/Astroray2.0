# 072 Analytic scene harness and A5
Goal: rendered images are not checked against physics -> a harness renders analytic scenes on CPU and GPU at 32x32 through the CLI, reads the fp64 moments from the film dump and compares each pixel with the closed-form truth using the analytic family's threshold, and A5 (view-filling emitter) passes on both devices.
Read first: 0009 d16 (analytic test: |mean - truth| <= z SE + eps_sys, not constant, not black), d15 (family size, seeds), d11 (analytic scenes on CPU and GPU at 32x32 in gate), d20 (A5 truth); 0008 V1 (film normalisation); 0003 check 11 (an RGB white emitter of strength 1 gives display Y = 1).
Scope: tools/analytic.py, scenes/lib/truths.py, scenes/a5_emitter/meta.toml, scenes/y_anchor/{build.py,meta.toml}, tools/check.ps1 (gate step), tests/abi/test_analytic_harness.py.
Out of scope: A1 (073), A3 and A4 (075); oracle scenes (M2).
Reference: Sidak 1967 (per-pixel threshold from the analytic family m in budgets.toml).
Do:
1. tools/analytic.py `--scene <name> --device cpu|cuda --spp N`: renders out/scenes/<name>.arrec with `astroray_cli dump` at 32x32, reads mean and variance of the mean per pixel and channel with fits_read.py, evaluates the scene's truth (scenes/lib/truths.py by the meta.toml formula name), and passes when every pixel satisfies |mean - truth| <= z SE + eps_sys with z from the analytic family's per-test alpha, and the image is neither constant where truth varies nor black where truth is not; each scene counts as one family member per channel; prints the worst pixel's z and a reproduce command.
2. Seeds: hash(scene name, repo constant) through the same rule as 020 (fixed in gate).
3. A5 truth: every pixel equals the emitter's display RGB (working space) within the threshold; with independent sampler mode the variance estimate is exact (0008 d3), so the harness runs analytic scenes in independent mode.
4. y_anchor: an RGB white emitter of strength 1 filling the view; truth display Y = 1 within 1e-3 (0003 check 11, render part).
5. check.ps1 `gate` runs a5_emitter and y_anchor on CPU and GPU after regenerating recordings.
6. test_analytic_harness.py: on a synthetic dump, a pixel offset by 10 SE fails, a constant image where truth varies fails, a black image fails, the exact truth passes.
Acceptance: `<python> tools/analytic.py --scene a5_emitter --device cuda --spp 64` prints PASS and the worst z; the same on cpu; `pwsh tools/check.ps1 gate` includes both scenes on both devices; `<python> -m unittest discover -s tests/abi -p "test_analytic_harness.py"` passes.
Look at: out/evidence/072/a5.png (the 32x32 render enlarged, converted by tools/report.py's PNG helper).
Canary: tasks/072.canary.patch must turn red: tests/abi/test_analytic_harness.py "TestHarness.test_offset_pixel_fails", and `tools/analytic.py --scene a5_emitter --device cpu` (the patch scales emission by 1.01).
Budget: about 10 s added to gate; report it.
Depends on: 071, 115.
Size: M (about 300 lines Python); stop and report at 2x.
Progress:
- Blocked before code: sampler mode is compile-time SOBOL (session.cpp kSamplerMode); ar_settings has no mode field (R60), so independent mode needs files outside Scope. Sobol A5 64 spp: per-pixel max |z| 0.26/1024 px, SE R/G/B 0.045/0.010/0.149.
