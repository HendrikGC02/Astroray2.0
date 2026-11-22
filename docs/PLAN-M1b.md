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

Counts are planning sizes with the reserve included; they add up to 113, inside the 110-135 range. A slice
closes with `check full` green, `check perf` within tolerance once 140 lands, one bench history line per
bench scene, and the lead looking at the slice's renders. A slice review takes the slice's measurements,
writes the next batch of task files (with a scope pre-flight each) and edits `tasks/README.md`; a planning
agent does it when the lead's context is long. Between slice closes the lead runs `check full` every 10
landings (R6c).

| Slice | Content | Tasks | Exit (beyond `check full`) |
|---|---|---|---|
| 0 Tooling and debts | 136 B-5M hang; 157 gate headroom; 137 canary tool; 138 land submit and history; 139 F12 gate on both devices; 140 `check perf` and 160 its idle and partition shares; 141 lock and `test.ps1`; 142 recurring-class lints; 143 reference sources; 144 M-1 probe spills; 155 documentation truth; 159 addon debts. Reserve candidates: lead helpers `next.ps1`, `task.ps1`, `sweep.ps1` (R10), B-31 if the owner schedules it, CPU `--tier=full` and the unchecked budget rows | 13 written + 5 reserve = 18 | B-5M rows measured; gate at or under 150 s; a landing needs no hand step; M-1 probe within D10 or its ladder step chosen |
| A Materials | Written: BLACKBODY and CAUCHY_ABBE (145), table-driven folding (146), opacity entry (147), Mix selection and the three-segment layout (148), F82 metal functions (149), half and float textures (150), op schema and coverage matrix (151), alpha pass-through (156), TABLE (158). To write: metal in the lobe stack with TransportMode checked in every lobe; `MAY_BE_DELTA` and emission estimate complete with V11's generated graphs and the NaN-input rule; addon lowerings (metallic, alpha, Emission, Blackbody, Mix Shader, colour-to-float, `use_nodes`); node-value tests; one report entry per unsupported node (V10); EXR image sync; extension mechanism (`ext_fields.def`, ExtParams, EXTENSION routing, `shade_surface_ext`); GRATING kind and its table builder; `shade_light_path_ext` gate rows over the stub; grating card and alpha-foliage scenes; B-19 on the alpha-card scene | 9 written + 15 to write + 6 reserve = 30 | 0005 V6 and V10 for every implemented op, V7; 0004 D3 alpha cards; V8 order angles on camera paths; B-19 on the alpha-card scene |
| B World, lights, guides, denoise | Written: environment table functions (153), light-tree builder (154). To write: `bake_environment`; world program with Environment Texture; environment NEE and MIS; HDRI asset, S3 and shader-ball row under it, the A9 one-bright-texel scene; tree upload and traversal in the kernel; tree against the alias oracle (D10 row) and B-23 again; first-hit guides, guide tint, denoising-surface bit; DLSS hit distance; OIDN asset loading with 0001 V5; `film_prepare_denoise` and the final denoise; F12 denoise settings and passes; forced-failure path; Diagnostics panel; heat maps in the report; spectral blackbody lamp; orthographic warning if 159 left one | 2 written + 22 to write + 6 reserve = 30 | Analytic A9; 0004 D7/D11 and D10 rows; 0008 V9-V11 (final); 0001 V5; 0003 check 15 second half |
| C Media core | Written: MediumSample and the HG lobe (152). To write: constant-block layout for a medium and the VM volume entry with `medium_eval` (written into 0006); interval-rule column ownership in `ops.def`; `shade_volume` stage skeleton with launch and budget rows; delta tracking on the hero lane with r_u/r_l; ratio tracking in `shade_shadow`; per-launch cap with requeue; medium state routing in `route_hit`; false interfaces at the top of `shade_surface`; `interior_ior` and `medium_priority` in the constant block; CPU stack fixture; a test-medium edit in the ABI and scene builder; slab scene (V1); capped against uncapped (V6, 0003 check 26); double CPU build; hero-lane variance measurement | 1 written + 15 to write + 4 reserve = 20 | 0006 V1, V6, stack fixture; 0003 check 26 |
| D Close | M-1 ratified with traversal, guides and GRATING in the capped build; B-14 and V7a re-gated with every M1b flag on; B-19, B-20 and B-05 re-measured; gallery scenes and report for M1b; `cuda none` in sidecars; owner look; a short retrospective from `out/land/history.jsonl`; M2's planning session | 12 + 3 reserve = 15 | ROADMAP M1b exit criteria |

### The spine: `shade_surface`

ROADMAP names M1b's main risk: tracking, traversal, guides and the extension routing together push
`shade_surface` over its allowances. Slices A, B and C are independent except here, so the edits to
`src/kernel/stages/shade_surface.h` are one ordered queue across slices, one open task at a time:

144 (probe attribution) -> 148 (Mix selection) -> 156 (alpha) -> 158 (TABLE resolve) -> metal in the stack ->
guides -> light-tree traversal -> false interfaces -> extension routing.

The risk is not left to slice D. Every one of these landings is gated on the `k_shade_surface` row in
`src/kernel/budget.toml` by `check gate` (registers, stack, spills, compile time), states the row before and
after in its PR, and re-runs the M-1 probe numbers. Decision path for that row, fixed now:
- Registers at 128 or stack over 512 B: stop; 0002 D10's ladder, starting from 144's attribution.
- Spills appear (the row is 0 / 0 today): the lead may raise the row by ruling up to 0002 D10's allowance
  for VM kernels (128 / 192 B), with the measured numbers in the ruling. Past that: the ladder.
- Compile time passes ruling 72's 5.0 s: the owner decided that number; back to the owner with the ladder's
  split measured (spike A: about 22 % slower at render time).

Everything that does not touch the spine (lobe functions, textures, schema, tables, tree builder, OIDN
loading, media structs) runs in the cpu lane beside it.

### Gate seconds

The gate stands at 163.5 s of B-09's 180 s. The first batch's Budget lines add about 50 s; 157 is expected
to give back 28 s or more. `tasks/README.md` keeps the ledger: measured gate seconds after each landing (from
138's history line) plus the declared seconds of open tasks. A task adds its declared seconds without asking.
When the projection passes 170 s the lead first moves newly added statistical cases into concurrent groups
(157's method); when no grouping is left, the question in section 6 goes to the owner. No test is dropped or
weakened to make room, and B-09 is not raised without the owner.

## 3. Order and dependencies of the first batch

```
157 gate headroom --> 139 F12 gate           137 canary tool --> 138 land submit --> 140 check perf --> 160 shares
136 B-5M hang --> 140              137 --> 141 lock, test.ps1        143 references, 142 lints, 155 documents, 159 addon debts
146 folding --> 145 BLACKBODY, CAUCHY_ABBE --> 158 TABLE          146 --> 151 schema (both edit tools/opsgen.py)
144 M-1 spills --> 150 half/float textures        146 --> 147 opacity entry --> 148 Mix selection --> 156 alpha --> 158
143 --> 149 F82 metal, 153 environment table, 154 light-tree builder      152 MediumSample (independent)
```
The table, the dispatch order and the landing order where files collide are in `tasks/README.md`. Until 138
lands, landings use `tools/lead/land-pr.sh` as in M1a. R2's one-item-per-line canary format was not adopted:
137 keeps the format and adds a lint of its shape, so open task files and the land parser stay as they are.

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

Models: Sonnet implements by default. Opus implements the tasks marked O in `tasks/README.md` (sampling and
estimators, concurrency, lifetime and teardown), as in M1a, where 31 of 135 tasks went to Opus; in this batch
it is 10 of 25 because the batch is the kernel-heavy front of the milestone. Whether that share is what owner
decision 31 meant is in section 6. Opus reviews kernel, physics, sampling, ABI, concurrency and process diffs
(O31) and every diff that writes into a record (ruling 87); other diffs land on green checks and their canary.
On `PASS WITH FIXES` the implementer fixes, the lead reads the fix diff and posts `REVIEW PASS`; a fix that
changes control flow in concurrency, lifetime or teardown code goes back to the reviewer. Haiku `digest`
reads long logs. `tools/lead/deepseek.sh` may draft history lines, backlog rows and digests; its text is
checked like any other. The lead hands over to a fresh session every 25-30 landings or at the first
compaction (R11), from STATUS, LEAD.md's handover and `tools/status.ps1`.

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

Answered (owner decisions 36, 37): B-19, B-20, B-31 and starting M1b as recommended (M1a closed); decision 36
confirmed; `process/guards` merged. The B-31 task is still to be written in slice 0. The other rows stay open.

| Decision | Default until answered | If the owner says "fix it" | Recommendation |
|---|---|---|---|
| B-19 GPU idle 11.0 % against 10 % on B-1M | row stays red in STATUS; M1a stays "built, not closed" | one remedy task in the gpu lane after 140, at most two (section 1), then back to the owner | Accept at M1a's close as measured and keep the 10 % limit as the target for slice D's re-measurement: the cause is the step-table round trip, B-14 (what the user sees) is met, and M1b changes the step count anyway |
| B-19 on B-5M | measured by 136 | as above | wait for 136's number |
| B-20 2.00x / 1.63x Cycles against 1.25x | reported at every slice close by `check perf`; no remedy task | a measurement-first task in slice D with a decision table; spike C only if B-14 fails | Treat 1.25x as the release target and gate regression instead (140) until the lobe set is complete in M2: the comparison is against a renderer with every feature on a kernel that has two lobes |
| B-31 instance loop 39.8 ms per 10k against 30 | red in STATUS | one task in the blender lane | Schedule one task in slice 0's reserve: V7a is met, but the loop grows with scene size and M3 adds corpus scenes |
| Starting M1b while M1a is "built, not closed" (ROADMAP: M1b depends on M1a) | slice 0 and the first batch proceed: none of it depends on the open rows, and the owner's instruction for this planning session asks for the next steps to move | the owner may hold product tasks until the close | go ahead; close M1a formally when the B-19, B-20 and B-31 answers are in |
| Opus as implementer for 10 of the first 25 tasks (owner decision 31: Sonnet implements, Opus reviews and thinks) | as M1a did for physics and shared contracts | all-Sonnet with Opus review; expect more "chose instead of asked" findings | keep the split; it falls as the work moves from kernel to breadth |
| Gate time when the projection passes 170 s of B-09's 180 s (section 2) | grouping first; landings are held, never weakened | raise B-09, or move named long-stable statistical cases to `full` with `check full` every 10 landings | move cases to `full`; keep 180 s |
| `k_shade_surface` compile time past ruling 72's 5.0 s | that landing stops | a new limit, or the D10 split with its measured render cost | decide on the numbers when it happens |
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
