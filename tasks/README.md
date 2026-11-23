# Task files for M1b (first light)

Scope: docs/ROADMAP.md, M1b; plan, slices and lanes: docs/PLAN-M1b.md; items without a task: docs/BACKLOG.md. One file per task, `tasks/NNN-slug.md`, <= 60 lines, fields in this order (lint section `taskfile`): Goal, Read first, Scope, Out of scope, Reference, Do, Acceptance, Look at, Canary, Budget, Depends on, Size, Progress. `land` deletes a task file when it lands. Numbers are ids, not order.
Only the first batch is written (136-161; 161 is decision 37's B-31 task). The lead writes the next batch at each slice review, from what the slice measured, with a scope pre-flight per task: grep the call sites, registration lists, budget rows and tests the change forces, and name them in Scope.
Scope lines name what a change forces (its budget and counter rows, registration lists, existing tests it breaks). CMake globs sources: a new file under a globbed directory edits no CMake; a new target does. Gate steps are tagged `cpu` or `local`; hosted `ci` runs the `cpu` ones.
Measurement tasks follow docs/PLAN-M1b.md section 5: metric and command, quiet machine, a decision table written before measuring, their own bench/history line, limits changed listed for the owner. At most two remedy tasks per budget row, then the owner decides.
Size counts source lines without tests: S under 150, M 150-500, L 500-1000. Canary line: `tasks/NNN.canary.patch must turn red: astroray_tests_cpu "suite/case", tests/x/test_y.py "Class.method"`.

## First batch (implementer model S = Sonnet, O = Opus; Opus review unless "skip ok"; gate = declared gate seconds)
| Task | Lane | Model | Depends on | Gate |
|---|---|---|---|---|
| 136 B-5M bench hang | gpu | S | none | +2 |
| 137 canary tool | tools | S | none | +4 |
| 138 land submit, undo, cleanup, history | tools | S | 137 | +1 |
| 139 F12 gate on CPU and CUDA | blender | S | 157 | +8 |
| 140 check perf; 160 its idle and partition shares | gpu | S | 136, 138; 160 after 140 | +1 |
| 141 visible lock, one-test runner | tools | O | 137 | +3 |
| 142 recurring-class lints; 143 reference sources | tools | S | none | 0 |
| 144 M-1 probe spills (0002 D10 ladder) | kernel | O | none | 0 |
| 145 SpectrumRef BLACKBODY, CAUCHY_ABBE | kernel | S | 146 | +3 |
| 146 table-driven folding | cpu | S | none | 0 |
| 147 opacity entry, NON_OPAQUE, scalar op | kernel | O | 146 | +1 |
| 148 Mix Shader selection, three-segment layout | kernel | O | 147 | +3 |
| 149 F82 metal lobe; 153 environment table; 154 light-tree builder | cpu | O | 143 | +3, +3, +4 |
| 150 half and float textures | kernel | S | 144 | +2 |
| 151 op schema and coverage matrix | cpu | S | 146 (tools/opsgen.py) | +1 |
| 152 MediumSample, HG phase lobe | cpu | O | none | +2 |
| 155 documentation truth | docs | O | none | 0 |
| 156 alpha pass-through | kernel | O | 147, 148 | +6 |
| 157 gate headroom | tools | S | none | -28 or more |
| 158 SpectrumRef TABLE | kernel | S | 145; after 156 | +4 |
| 159 addon debts | blender | S | none | +1 |
| 162 `check full` red: gui_colour_chart never converges; 161 B-31 instance loop (owner decision 37) | blender | S | none; 162 before 161 in the lane | 0, 0 |
Gate ledger (owner decision 38: measured seconds replace declared ones once known; hold a landing only if the measured max would pass 170 s): quiet-machine gate after 157 max 132.7 s, median 123.1 s (5 runs). Landed: 155 (+0), 146 (+1, gate case), 157 (scenes cache), 143 (+0). Measured, not landed: 137 +0.6, 152 +0.4. Open declared: +50 s. Landing gates under load: 155 133.6 s, 146 175.5 s (implementers compiling beside it), 157 144.9 s, 143 131.7 s.

## Order
Dispatch at once: 157, 137 (tools, two at a time; then 143, then 142, 138, 141); 146, 152 (cpu); 144 (kernel); 136 (gpu); 159 (blender); 155 (docs). Then as dependencies land: 147, 145, 151, 150; 149, 153, 154; 148, 156, 158; 139, 140, 160; 162 then 161 in the blender lane.
Landing order where files collide. `shade_surface.h`, `ops.def`, `src/kernel_gpu/probe_m1.cu`: 144, 146, 145, 147, 148, 156, 158; 150 after 144. `tools/check.ps1`: 157, 137, 141, 138. `tools/opsgen.py`: 146, 151. `tools/lint.py` SECTIONS and tools/README.md (25 of 30 lines; one line per task): 137, 142, 143, 155 in any order, the later one rebases.
Lanes (docs/PLAN-M1b.md section 4): cpu = work loop and acceptance on the CPU (5-8 at once); kernel = kernel headers and VM ops, CPU work loop, one gate run before hand-off (three at once, one at a time on `shade_surface.h`); gpu = the work loop needs the GPU (one); blender (one); tools (two); docs (one).
Next batch, written at the first slice review (outlines: docs/PLAN-M1b.md section 2): metal in the lobe stack, compiler flags (V11), addon lowerings, EXR image sync, extension mechanism and GRATING, VM volume entry with `medium_eval`, `bake_environment` and world wiring, tree traversal, guides, OIDN loading.
