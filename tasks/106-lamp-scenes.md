# 106 Lamp scenes A3, lit furnace, NEE on/off and stacked lamps, with the NEE test hook
Goal: area-lamp transport and MIS are not checked against physics -> A3 (disk over a plane), the small lit furnace, NEE on/off/BSDF-only agreement and stacked lamps pass on CPU and GPU, driven by a gate-build hook that switches the NEE mode.
Read first: 0004 D1 (point and area check, lit furnace), D8 (stacked lamps), 0004 Verification rows D1, D8; 0009 d20 (A3 disk emitter over a plane), d16 (harness); src/host/session/test_hooks.h (101).
Scope: src/host/session/test_hooks.{h,cpp} (NEE row), src/kernel/kernel_globals.h (SessionUniforms field), src/kernel/stages/shade_surface.h, shade_background.h and shade_light.h (read the NEE mode), scenes/a3_disk/, scenes/lit_furnace_small/, scenes/d1_nee_onoff/, scenes/d8_stacked_lamps/, scenes/lib/truths.py, tools/analytic.py (two-render comparison), tools/check.ps1, tests/cpu/test_nee.cpp (Do 4).
Out of scope: spot and sun lamps (M4); light paths (M5); an ABI setting for NEE (none: this is a test hook).
Reference: Veach 1997, ch. 9 (estimators combined by MIS must agree); Howell, Menguc and Siegel, Thermal Radiation Heat Transfer, disk-to-differential-area configuration factor (A3 truth).
Do:
1. NEE hook: `ASTRORAY_TEST_NEE=on|off|bsdf_only` sets a SessionUniforms field through 101's test_hooks (AR_GATE_HOOKS builds only): off emits no shadow items and weights emitter hits 1; bsdf_only also drops the light pdf from MIS; the CLI report prints it when set.
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
