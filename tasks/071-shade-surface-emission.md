# 071 shade_surface (part 1): hit context, VM, emission, AOVs and statistics
Goal: hits are never shaded -> the capped `shade_surface` kernel builds the shading context, runs the material's surface program into the OpenPBR record, adds emission seen by camera paths, writes primary-hit AOVs and path-end statistics; paths end after their first vertex until task 073 adds continuation.
Read first: 0002 D9 (fused kernel, launch bounds (256,2)), D10 (VM kernel stack class, declare deltas), D8 (shade_surface row), 0005 item 3 (one register file per kernel), items 32-33 (entries, context), 0004 D5 (accumulation through the slot), D28 (path-end statistics), 0003 d21-d22 (flags, termination).
Scope: src/kernel/stages/shade_surface.h, src/kernel/stages/shade_common.h (context build; path end is 103's), src/kernel_cpu/shade_surface.cpp, src/kernel_gpu/shade_surface.cu, src/kernel/launch_config.h, src/kernel/budget.toml, tests/cpu/test_shade_surface_emission.cpp, tests/gpu/test_shade_surface_agree.cpp.
Out of scope: shade_background (103); BSDF sampling and continuation (073), NEE and MIS (074), lamps (075), alpha pass-through and guides (M1b).
Reference: Cycles `kernel/integrator/shade_surface.h` (Apache-2.0) for stage structure.
Do:
1. Context: from the hit record and scene views: P (barycentric interpolation of positions), Ng, interpolated corner normal N, UV set 0, `FRONT_FACE` flag (geometric normal vs ray, surviving the instance transform), wo, object and instance ids, lambda_t.
2. Run the material's `surface` entry (CONSTANT_SURFACE skips the interpreter and only resolves slots) with one `VmRegs` declared in the kernel; magenta materials arrive already replaced by the compiler.
3. Emission: Le from the emission segment, added with `sample_add(slot, beta * Le)` for camera paths (weight 1 at the first vertex; 074 adds MIS at later vertices); a NaN or Inf contribution sets term INVALID and is not added.
4. AOVs at the primary hit (049, once via `AOV_WRITTEN`); path end through 103's `path_end` with term `MAX_BOUNCE` (temporary, until 073).
5. launch_config rows (shade_surface T = 256, B = 2, VM class) and budget.toml rows recorded from the first gate run; state the declared spill and stack in Progress.
6. Tests: CPU (fast): a hand-built hit on an emissive constant material adds exactly beta * Le to its slot; AOVs are written once; GPU (gate): per-stage agreement fixture of shade_surface on 10^5 synthetic hits (Le within the eval thresholds, integer fields bitwise).
Acceptance: `pwsh tools/check.ps1 gate` passes; kernelgate prints shade_surface's registers, spills and stack under (256,2) within the VM allowances; `out/bin/astroray_cli.exe render out/scenes/a5_emitter.arrec -o out/tmp/a5.fits --spp 4 --size 32x32 --device cuda` writes an image whose mean equals L to 1e-6 (checked statistically in 072).
Look at: none.
Canary: tasks/071.canary.patch must turn red: astroray_tests_cpu "shade_surface/emission added once", astroray_tests_gpu "shade_surface/agreement".
Budget: declares shade_surface's first spill and stack baseline (VM kernel: <= 128/192 B spills, <= 512 B stack).
Depends on: 041, 049, 061, 070, 102, 103, 029.
Size: M (about 400 lines); stop and report at 2x.
Progress:
