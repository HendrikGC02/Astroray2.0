# Task files for M1b (first light)

Scope: docs/ROADMAP.md, M1b; plan, slices and lanes: docs/PLAN-M1b.md; items without a task: docs/BACKLOG.md. One file per task, `tasks/NNN-slug.md`, <= 60 lines, fields in this order (lint section `taskfile`): Goal, Read first, Scope, Out of scope, Reference, Do, Acceptance, Look at, Canary, Budget, Depends on, Size, Progress. `land` deletes a task file when it lands. Numbers are ids, not order.
Only the first batch is written (136-158). The lead writes the next batch at each slice review, from what the slice measured, with a scope pre-flight per task: grep the call sites, registration lists, budget rows and tests the change forces, and name them in Scope.
Scope lines name what a change forces (its budget and counter rows, registration lists, existing tests it breaks). CMake globs sources: a new file under a globbed directory edits no CMake; a new target does. Gate steps are tagged `cpu` or `local`; hosted `ci` runs the `cpu` ones.
Measurement tasks follow docs/PLAN-M1b.md section 5: metric and command, quiet machine, a decision table written before measuring, their own bench/history line, limits changed listed for the owner. At most two remedy tasks per budget row, then the owner decides.
Size counts source lines without tests: S under 150, M 150-500, L 500-1000. Canary line: `tasks/NNN.canary.patch must turn red: astroray_tests_cpu "suite/case", tests/x/test_y.py "Class.method"`.

## First batch (lane; implementer model S = Sonnet, O = Opus; review by Opus unless "skip ok")
| Task | Lane | Model | Depends on |
|---|---|---|---|
| 136 B-5M bench hang | gpu | S | none |
| 137 canary tool | tools | S | none |
| 138 land submit, undo, cleanup, history | tools | S | 137 |
| 139 F12 gate on CPU and CUDA | blender | S | 157 |
| 140 check perf | gpu | S | 136, 138 |
| 141 visible lock, one-test runner | tools | O | 137 |
| 142 recurring-class lints | tools | S | none |
| 143 reference sources | tools | S | none |
| 144 M-1 probe spills (0002 D10 ladder) | gpu | O | none |
| 145 SpectrumRef BLACKBODY, CAUCHY_ABBE | kernel | S | none; lands after 146 |
| 146 table-driven folding | cpu | S | none |
| 147 opacity entry, NON_OPAQUE, scalar op | kernel | O | none; lands after 146 |
| 148 Mix Shader selection, three-segment layout | kernel | O | 147 |
| 149 F82 metal lobe | cpu | O | 143 |
| 150 half and float textures | kernel | S | none |
| 151 op schema and coverage matrix | cpu | S (skip ok) | none |
| 152 MediumSample, HG phase lobe | cpu | O | none |
| 153 environment importance table | cpu | O | 143 |
| 154 light-tree layout, builder, descent | cpu | O | 143 |
| 155 documentation truth | docs | O | none |
| 156 alpha pass-through | kernel | O | 147; lands after 148 |
| 157 gate headroom | tools | S | none |
| 158 SpectrumRef TABLE | kernel | S | 145 |

## Order
Dispatch at once: 157, 137, 142, 143 (tools); 146, 151, 152 (cpu); 147, 150 (kernel); 144, then 136 (gpu); 155 (docs). Then as dependencies land: 138, 141; 145, 148, 149, 153, 154; 139, 140, 156, 158.
Landing order where files collide: `ops.def` and `src/kernel_gpu/probe_m1.cu`: 144, 146, 145, 147, 148, 156, 158. `tools/check.ps1`: 157, 137, 141, 138. `tools/lint.py` SECTIONS and tools/README.md (one line each): 137, 142, 143, 155 in any order, the later one rebases.
Lanes (docs/PLAN-M1b.md section 4): cpu = work loop and acceptance on the CPU (5-8 at once); kernel = CPU work loop, kernel headers or VM ops, one gate run before hand-off (at most 3 at once: they meet in `ops.def`, `shade_surface.h` and `budget.toml`); gpu = the work loop needs the GPU (one); blender (one); tools (two); docs (one).
Next batch, written at the slice-0/A review (outlines in docs/PLAN-M1b.md section 2): metal in the lobe stack, compiler flags complete (V11), addon lowerings, EXR image sync, extension mechanism and GRATING, VM volume entry with `medium_eval`, `bake_environment` and world wiring, tree traversal in the kernel, guides, OIDN loading.
