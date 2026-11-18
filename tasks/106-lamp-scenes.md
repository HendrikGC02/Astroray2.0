# 106 Lamp scenes A3, lit furnace, NEE on/off and stacked lamps, with the NEE test hook
Goal: area-lamp transport and MIS are not checked against physics -> A3 (disk over a plane), the small lit furnace, NEE on/off/BSDF-only agreement and stacked lamps pass on CPU and GPU, driven by a gate-build hook that switches the NEE mode.
Read first: 0004 D1 (point and area check, lit furnace), D8 (stacked lamps), 0004 Verification rows D1, D8; 0009 d20 (A3 disk emitter over a plane), d16 (harness); src/host/session/test_hooks.h (101).
Scope: src/host/session/test_hooks.{h,cpp} (NEE row), src/kernel/kernel_globals.h (SessionUniforms field), src/kernel/stages/shade_surface.h, shade_background.h and shade_light.h (read the NEE mode), scenes/a3_disk/, scenes/lit_furnace_small/, scenes/d1_nee_onoff/, scenes/d8_stacked_lamps/, scenes/lib/truths.py, tools/analytic.py (two-render comparison), tools/check.ps1, tests/cpu/test_nee.cpp (Do 4); lead ruling: tools/budgets.toml [family_members] analytic_scenes only, by exactly the added members.
Out of scope: spot and sun lamps (M4); light paths (M5); an ABI setting for NEE (none: this is a test hook).
Reference: Veach 1997, ch. 9 (estimators combined by MIS must agree); Howell, Menguc and Siegel, Thermal Radiation Heat Transfer, disk-to-differential-area configuration factor (A3 truth).
Do:
1. NEE hook (lead ruling, R77): `ASTRORAY_TEST_NEE=on|off|light_only` sets two TEST_HOOK_* bits of SessionUniforms::test_hooks through 101's test_hooks (AR_GATE_HOOKS builds only; unknown value throws): off is the BSDF-only estimator (no shadow items, emitter hits weighted 1); light_only drops the BSDF pdf from the NEE weight and drops emitter hits after a non-delta bounce (shade_light, shade_surface, shade_background); first-vertex and post-delta emission count. The CLI report line is dropped (076 Do 6). d8 crossings per ray come from ar_session_read_counters through the Python bindings, never cli/bench.cpp (R71).
2. Scenes and truths: a3_disk (disk lamp over a Lambertian plane, camera above: per-pixel truth from the disk configuration factor); lit_furnace_small (max_depth = 1: L_r = rho L_e (R/d)^2 within 0.5%, 0004 D1); d1_nee_onoff (on, off and bsdf_only renders of an area-lamp scene agree: `tools/analytic.py --compare` checks two renders' means within the joint SE); d8_stacked_lamps (two stacked area lamps facing the camera, on and off agree; crossings per ray reported).
3. check.ps1 `gate` runs every scene above on CPU (`cpu` tag) and GPU at 32x32.
4. From 074: "nee/program emission is evaluated at the light point" uses an emission that is constant over the triangle, so swapped barycentrics or vertex order would pass. Make the program's Le vary over the triangle (for example a TEX_IMAGE on UV, 061) and check the item against the value at the sampled point computed by hand; extend the canary to swap two barycentrics.
5. From 075: shade_background.h line 11 holds a literal `
` inside a comment, which joins its two Ref lines; split them while editing that file.
Acceptance: `pwsh tools/check.ps1 gate` passes with the four scenes on both devices and prints crossings per ray for d8; `out/bin/astroray_cli.exe render` of a3_disk at 512x512 writes an image the lead inspects.
Look at: out/evidence/106/a3_disk.png.
Canary: tasks/106.canary.patch must turn red: `tools/analytic.py --scene d8_stacked_lamps --device cuda` (the patch skips the MIS weight in shade_light), `tools/analytic.py --scene a3_disk --device cpu` (the patch makes the disk lamp two-sided), astroray_tests_cpu "nee/program emission is evaluated at the light point" (the patch swaps two barycentrics where the light point's emission is evaluated).
Budget: gate time +10 s.
Depends on: 075.
Size: M (about 300 lines); stop and report at 2x.
Progress:
- started (task/106-lamp-scenes)
- lead rulings: modes on|off|light_only (R77), CLI hook line dropped, counters via Python ABI, budgets family row, fix shade_surface/background formatting
- four scenes pass CPU and CUDA in gate; family 34 -> 60; canary turns all three named cases red
- pair checks test pixel blocks: BSDF-only gives zero-hit pixels (mean and VAR 0) per pixel; comparison is per scene formula, no --compare flag
- furnace no-bias check: 16384 spp CPU, Y image mean 0.268208 vs truth 0.268276 (z -1.43), resolves 0.073%; worst pixel z +3.53
- lead option (c): d1, d8 render 64x64 at 64 spp, tested in 8x8 blocks (4096 samples each); a3, furnace stay 32x32 at 1024
- null calibration `analytic.py --scene S --device cpu --null 100 --null-mode M` (d1 on/off/light_only, d8 on/off): 0 block and 0 image alarms of 500 checks; G |z| tail normal (max 3.89), Y conservative (Minkowski SE); numbers in analytic.py
- CPU vs CUDA a3 dumps differ (display max rel 4.5e-7, VAR 8.8e-7; NSAMP and bounces equal): same integer-RNG paths, float rounding apart; DEVICE header cpu / cuda
