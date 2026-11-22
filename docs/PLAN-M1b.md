# Plan for M1b "First light"

Reader: the coordinator (lead) session that dispatches M1b, and the owner. Scope and exit criteria are
ROADMAP's M1b section; this page says in what order, in which lanes and with which process. Evidence for the
process choices is `docs/retro/M1a-retrospective.md` (cited as R1-R20, F1-F25). Task files are in `tasks/`;
their order is `tasks/README.md`. Backlog items without a task are in `docs/BACKLOG.md`.

## 1. What M1a taught, and what this plan does about it

| Lesson (retrospective) | Consequence here |
|---|---|
| 25 estimated tasks became 135 (5.4x) | M1b is planned at 110-135 task files, in five slices of 15-30, with a slice review between them (R13). Only the first batch is written; each later batch is written at the slice review before it, from what the slice measured. |
| 22 % of tasks were added mid-milestone, 17 of 24 by measurements | Each slice reserves a fifth of its count for follow-ups. A budget row gets at most two remedy tasks per milestone, then goes to the owner with numbers (R8). |
| Landing mechanics were the largest avoidable cost | Slice 0 is tooling: canary tool (137), land hardening (138), lock visibility (141). They run beside the first product tasks, in their own lane. |
| Scope lines were wrong in 11 of 13 consecutive tasks | Every task in this batch had a scope pre-flight (a grep for call sites, registration lists, budget rows and tests the change forces). Scope lines name those. Tasks of size M or L start with the implementer's file list (implementer definition). |
| Three defect classes had no gate | 139 (F12 on both devices, textured), 140 (`check perf`), 138 (Blender suites when `blender/` changes). |
| Session 2 was four times slower than session 1 because everything queued on one machine lock | Lanes (section 4). Product work is ordered so the CPU lane is never empty. |
| Opus review found a real defect in 40-48 % of diffs for 2 % of tokens | Kept. The reviewer now reads the canary patch and main sources (reviewer definition). |
| B-08 failed 60+ times from rebuilds | Fixed: the build step is outside B-08's window (owner decision 36). "Rerun once" is gone. |

## 2. Slices

Counts are planning sizes with the reserve included. A slice closes with `check full` green, `check perf`
within tolerance once 140 lands, one bench history line per bench scene, and the lead looking at the slice's
renders. A slice review takes the measurements of the slice, writes the next batch of task files and edits
`tasks/README.md`; it is the lead's work, with a planning agent if the lead's context is long.

| Slice | Content | Tasks | Exit (beyond `check full`) |
|---|---|---|---|
| 0 Tooling and debts | 136 B-5M hang; 137 canary tool; 138 land hardening and history; 139 F12 gate on both devices; 140 `check perf`; 141 lock and `test.ps1`; 142 recurring-class lints; 143 reference sources; 144 M-1 probe spills; 155 documentation slimming | 10 written + 5 reserve | B-5M rows measured; a landing needs no hand step; M-1 probe within D10 or its ladder step chosen |
| A Materials | SpectrumRef kinds (145), folding (146), entry table (147), Mix selection and the three-segment layout (148), F82 metal (149), half and float textures (150), generated schema and coverage matrix (151), alpha pass-through (156); then: metal in the lobe stack, compiler flags complete, addon lowerings (metallic, alpha, Emission, Blackbody, Mix Shader, colour-to-float), node-value tests, EXR image sync, extension mechanism, GRATING kind and its table builder, light-path extension stub, grating card and alpha-card scenes | 8 written + about 14 to write + 6 reserve | 0005 V6 and V10 for every implemented op, V7; 0004 D3 alpha cards; V8 order angles on camera paths; B-19 on the alpha-card scene |
| B World, lights, guides, denoise | Environment table functions (153), light-tree builder (154); then: `bake_environment`, world program with Environment Texture, environment NEE and MIS, HDRI asset and scenes, tree traversal in the kernel, tree against alias oracle, first-hit guides and guide tint, DLSS hit distance, OIDN asset loading with V5, `film_prepare_denoise` and final denoise, F12 denoise settings, Diagnostics panel, heat maps in the report, spectral blackbody lamp | 2 written + about 18 to write + 5 reserve | 0004 D7/D11 and D10 rows; 0008 V9-V11 (final); 0001 V5; 0003 check 15 second half |
| C Media core | MediumSample and the constant medium (152); then: interval-rule ownership in `ops.def`, delta tracking in `shade_volume`, ratio tracking in `shade_shadow`, per-launch cap with requeue, medium stack and false interfaces in `shade_surface`, `interior_ior` and `medium_priority`, CPU stack fixture, slab scene, double CPU build | 1 written + about 10 to write + 3 reserve | 0006 V1, V6, stack fixture; 0003 check 26 |
| D Close | M-1 ratified with traversal, guides and GRATING in the capped build; B-14 and V7a re-gated with every M1b flag on; B-19 and B-20 re-measured; gallery and report for M1b; owner look; a short retrospective from `out/land/history.jsonl` | about 10 + 3 reserve | ROADMAP M1b exit criteria |

Slices A, B and C do not wait for each other. They share three files that need care: `shade_surface.h`
(alpha pass-through in A, guides in B, false interfaces in C), `ops.def` (append only) and
`src/kernel/budget.toml`. Rule: at most one open task edits `shade_surface.h` at a time, in the order 156
(alpha), guides, false interfaces, extension routing; each re-runs the M-1 numbers and states the register
and spill delta in its Budget line.

## 3. Order and dependencies of the first batch

```
157 gate headroom --> 139 F12 gate           137 canary tool --> 138 land submit --> 140 check perf <-- 136 B-5M hang
137 --> 141 lock, test.ps1        142 lints, 143 references, 155 documents (independent)
144 M-1 spills (GPU lane, first; lands before the other probe_m1.cu edits)
146 folding --> 145 BLACKBODY, CAUCHY_ABBE --> 158 TABLE
146 --> 147 opacity entry --> 148 Mix selection --> 156 alpha pass-through
143 --> 149 F82 metal, 153 environment table, 154 light-tree builder      150 half/float textures, 151 schema, 152 MediumSample (independent)
```
The table, the dispatch order and the landing order where files collide are in `tasks/README.md`. 157 is first
in the tools lane because the gate stands at 163.5 s of B-09's 180 s and every task below adds seconds; each
task's Budget line states its seconds and says "stop and ask above". Until 138 lands, landings use
`tools/lead/land-pr.sh` as in M1a. Tasks found while drafting that are not written yet: the VM volume entry
with `medium_eval` (after 147; needs a constant-block layout for a medium written into 0006), the idle and
partition share analysis script for `check perf` (after 140; 134's script was not committed), one Blender
start per windowed test file for canaries (after 137).

## 4. Lanes

| Lane | What runs there | Limit |
|---|---|---|
| cpu | kernel library functions, host code, compiler, CPU tests: no GPU or Blender in acceptance | as many as the lead can rule for; 5-8 worked in M1a |
| kernel | kernel headers, VM ops, stages: the work loop is CPU tests, with one `check gate` before hand-off | three: they meet in `ops.def`, `shade_surface.h`, `probe_m1.cu` and `budget.toml` |
| gpu | tasks whose work loop needs the GPU: captures, kernelgate tuning, bench runs | one |
| blender | addon, sync, Blender tests, gallery | one |
| tools | `tools/`, `.github/`, lint; two tasks that edit the same script land in the order `tasks/README.md` gives and the later one rebases | two |
| docs | records, ROADMAP notes, BACKLOG | one, any time |

Every landing still takes the machine lock for the gate, so landings are serial: land the oldest green
branch first, and do not commit to main while a landing runs. A measurement capture holds the machine lock
for its whole run and runs when the gpu and blender lanes are idle (R8).

Models: Sonnet implements by default. Opus implements tasks marked O in `tasks/README.md`: sampling and
estimators, concurrency, lifetime and teardown, record edits. Opus reviews kernel, physics, sampling, ABI,
concurrency and process diffs (O31); other diffs land on green checks and their canary. Re-review only a fix
that changes control flow in concurrency, lifetime or teardown code. Haiku `digest` reads long logs.
`tools/lead/deepseek.sh` may draft history lines, backlog rows and digests; its text is checked like any other.

## 5. Measurement tasks

A measurement task states, before measuring: the metric's definition and the command that prints it; that
the capture holds the machine lock with no other GPU process; a decision table (within the limit: nothing;
missed by less than X: the named remedy; missed by more: stop and ask); and that it appends its own
`bench/history` line. Analysis scripts are committed under `tools/`. A limit it proposes to change is listed
in the PR body under "Limits changed" with the old and new value, and is the owner's to accept. An
acceptance method that has never been tried once is not written into a task.

Measured in M1b (ROADMAP "Measurements inside" plus M1a's open rows): M-1 probe spills (144, first); B-5M
rows of B-14 and B-19 (136); perf baseline and its noise (140); B-19 on the alpha-card scene (slice A);
guide register cost and OIDN on sm_120 (slice B); B-14 and V7a with environment sampling and tree traversal
on (slice B); hero-lane variance over the slab (slice C); B-05 for `shade_surface` (ruling 72) and B-20 at
M1b scope (slice D).

## 6. Where the M1a owner decisions plug in

No task in slices 0, A, B or C depends on these decisions, so M1b starts without them. Each has a default
that holds until the owner answers, and a place where the answer lands.

| Decision | Default until answered | If the owner says "fix it" | Recommendation |
|---|---|---|---|
| B-19 GPU idle 11.0 % against 10 % on B-1M | row stays red in STATUS; M1a stays "built, not closed" | one remedy task in the gpu lane after 140, at most two (section 1), then back to the owner | Accept at M1a's close as measured and keep the 10 % limit as the target for slice D's re-measurement: the cause is the step-table round trip, B-14 (what the user sees) is met, and M1b changes the step count anyway |
| B-19 on B-5M | measured by 136 | as above | wait for 136's number |
| B-20 2.00x / 1.63x Cycles against 1.25x | reported at every slice close by `check perf`; no remedy task | a measurement-first task in slice D with a decision table; spike C only if B-14 fails | Treat 1.25x as the release target and gate regression instead (140) until the lobe set is complete in M2: the comparison is against a renderer with every feature on a kernel that has two lobes |
| B-31 instance loop 39.8 ms per 10k against 30 | red in STATUS | one task in the blender lane | Schedule one task in slice 0's reserve: V7a is met, but the loop grows with scene size and M3 adds corpus scenes |
| M-1 probe spills 130/376 B against 128/192 | 144 applies 0002 D10's ladder | n/a (ROADMAP already orders it) | none needed |
| Spike C not run | stands: B-14 passes on B-1M | owner may order it | keep |
| Process: `process/guards` branch (tool deny rules, allow-list, standing scope clause in CLAUDE.md) | not merged; Scope lines in this batch name forced files instead | merge the branch | merge it |
| `check perf` tolerances (R6) | 140 sets them at three times the measured run-to-run spread, marked PROVISIONAL | owner sets other values | accept 140's values after reading its numbers |
| Deleting superseded records (0009 q4); folding rulings 64-85 into records (R12 a) | not done; 155 moves only review tails and planning documents to `docs/history/` | 155's follow-up does it | yes to both |
| Exact JPEG parity (ruling 78) | 2-code tolerance stands | a libjpeg-turbo task, needs a 0001 D9 change | leave |

## 7. After M1b

ROADMAP's order of milestones stands. Its sizes are now read at 4.4x to 5.4x of the estimate (its overview
table says so), and each milestone is planned the same way: a planning session writes the plan and only the
first batch; slices of 15-30 tasks; a slice review writes the next batch. M2's planning session runs during
slice D, when M-1 at M1b scope and B-20 are known, because both decide how M2's lobes are split across
kernels. M3 is independent of M2 except for the corpus run (ROADMAP), so its first slice (texture system,
CPU lane) may start beside M2 if the lead has attention to spare.
