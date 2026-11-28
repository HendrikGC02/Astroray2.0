# Plan for M2 "Lobes, shadows and media"

Reader: the lead session that dispatches M2, and the owner. Scope and exit criteria are ROADMAP's M2 section;
this page says in what order, in which lanes, and how the kernel-split question is settled before the lobes
reach `shade_surface`. Evidence: `docs/history/M1b-task-ledger.md` (M1b's gate ledger, slice D lines for 213, 215, 188),
`bench/history/2026.jsonl`, `docs/BACKLOG.md`, and task 217's retrospective once it lands (this page was
written before it; where 217's tables disagree with a number here, 217 wins and the slice review corrects
this page). Task files are in `tasks/`; their order is `tasks/README.md`. The process of
`docs/PLAN-M1b.md` sections 4 and 5 (lanes, measurement tasks) and `docs/LEAD.md` holds unless changed below.

## 1. What M1b taught, with numbers, and what this plan does about it

| Measured in M1b | Consequence here |
|---|---|
| 96 task files landed (ids 136-237), 4 open on which M1b's exit rests (214, 216, 217, 236), 10 debts written beside them (238-247, run as M2's slice 0): about 110 against the plan's 110-135 and ROADMAP's estimate of 22 (5.0x) | M2 (estimate 25) is planned at 105-125 task files in six slices. Only the first batch is written; each later batch is written at the slice review before it. |
| The spine worked: twelve ordered edits of `shade_surface.h`, each gated on its budget row, and the risk showed at the edit that caused it (ruling 93, ruling 103, owner decisions 40, 41, 43, 44) | Kept: one queue, one open task at a time, row before and after in every PR. New: the queue does not start on the layered lobes until task 248 has measured the arrangement (section 3). |
| `k_shade_surface` ends M1b at 128 registers, stack 416 B, spills 4 / 16 B, compile 7.33 s quiet (7.45 s recorded) of 8.0 s. Left: 96 B of stack to 0002 D10's 512 B, nothing to ruling 93's 16 B of spill loads, about 0.6 s of compile time. B-05 for this TU was raised three times (5.0 to 7.0, 7.5, 8.0 s) | Raising rows is spent as a method. Coat, fuzz, thin film, transmission and anisotropy cannot all enter the fused kernel on what is left; the arrangement is decided by measurement first (section 3), and the owner is asked for M2's B-05 and spill lines once, with numbers (section 7). |
| The full-scope proxy `ar_probe_m1` (spike A's stand-in lobes, about 100 opcodes) reads 128 / 528 B / 198 / 344 B capped, 203 uncapped: over D10's VM limits (512 B, 128 / 192 B) by 16 B of stack, 70 B of stores and 152 B of loads; 173's environment NEE alone took its spill loads from 332 to 548 B at the time | The forecast is "does not fit as built". The probe's stand-ins are replaced by the real lobes as they land, so the proxy converges on the real kernel instead of being re-attributed (BACKLOG row from 213). |
| B-20 is not met: 12.36 / 12.94 ms per 1080p sample against 9.5 / 12.5 ms (1.30x and 1.04x of the limits' Cycles basis), and the pass grew 1.057x / 1.054x across M1b on scenes that use none of M1b's new features | Any arrangement that slows the common path is paid on every scene. Spike A's split pair was 22 % slower at kernel level; if the whole pass slowed by that much, B-1M would read about 15 ms (+2.7 ms). The decision table in section 3 therefore ranks by measured pass time, not by registers. Cumulative pass time is reported at every slice close (`perf.ps1 -Cumulative`, task 215). |
| B-19: idle 11.9 % mean on B-1M (11.0 % at the M1a close), pq 10.9 % on b_foliage because each alpha layer costs two scheduler steps (132 steps per pass); both accepted red (owner decisions 37, 44), pq "revisited with M2's transparent-shadow work" | Slice B ends with a B-19 re-measurement on b_foliage with `ah_record` on; no remedy is planned before that number. |
| Gate: quiet main 137-147 s against decision 38's 170 s hold line; one landing (193) added 20.5 s with a capped-equals-uncapped pair; fixtures measure about 3x their nominal seconds | Every task declares gate seconds; a lobe's furnace and chi-squared cases go to the `full` family with one small gate case each (owner decision 42: a gate case over 1 s says why `fast` cannot carry it). First batch declares +18.4 s (section 4). |
| A second VM TU (`shade_surface_ext`, the same body) adds 3 to 5.5 s of compile timing to every shared-body landing, off the B-09 clock (rulings 97, 100, 112) | Each further `ShadeKind` identity adds the same again; it is counted in section 3's table as build time, and it is why "one more identity" is not free even when its render cost is zero. |
| Scope lines held after the pre-flight; the misses were suites that only hosted ci runs (208) and stale Budget lines (LEAD lesson 14) | Every task here names its kernel rows as of 213 and the suites its landing must run; 245 brings the headless suite to the landing. |
| Waiting, not work, dominated task time (owner decision 42: lock and background waits 18 %, lead or owner answers 22 %) | Owner questions are collected in section 7 with a default each; no first-batch task waits on one except 251 (downloads) and the stack tasks that follow 248. |

## 2. Slices

Counts are planning sizes with a fifth held for follow-ups; they add up to 118. A slice closes as in M1b:
`check full` green, `check perf` within tolerance with the cumulative line, one bench history line per bench
scene, the lead looking at the slice's renders, then the slice review writes the next batch.

| Slice | Content | Tasks | Exit (beyond `check full`) |
|---|---|---|---|
| 0 Forecast, harness, carried debts | Written: the debts 238-247 (M1b's exit does not wait for them; M2 builds on 239 divide and clamp, 242 limit counter, 243 sampler key, 247 lobe guards); 248 M-1 forecast and arrangement A/B; 249 lobe check harness; 250 E-table tool with registry and regeneration check; 251 references (single-file kind, fuzz fit, walk oracle source); 252 null calibration 17(v). To write after the owner answers: oracle availability (Mitsuba 3, pbrt-v4) for O1-O3; asset fetches for the M2 gallery | 15 written + 3 to write + 3 reserve = 21 | 248's decision table filled and ruled; 17(v) calibrated or declared non-gating; the references in the cache |
| A Lobes | Written (library, off the spine): 253 anisotropic GGX; 254 dielectric reflection and transmission; 255 E_glass and E_avg tables; 256 rough-surface walk oracle; 257 coat functions; 258 fuzz; 259 thin-film Airy; 260 IOR range and the E-table clamp counter; 265 A1 lossless-configuration rows. Written (spine): 261 per-type bounce limits; 262 dielectric energy compensation. Written (compiler): 263 Add Shader; 264 Principled's M2 sockets reported by name. To write after 248: the arrangement task; then the stack in OpenPBR's layering order, one spine task each: anisotropy and tangent frame, transmission with the interior medium push, coat with darkening, fuzz, thin film on dielectric and metal, thin wall, translucent base and `transmission_depth` lowering, per-point dispersion with secondary termination; the probe's stand-ins removed lobe by lobe; addon lowerings per lobe; V9 "verify" rows; V11 on generated graphs with every lobe | 13 written + 18 to write + 6 reserve = 37 | 0005 V1-V5 per lobe and per configuration; V9; V11; A1 all lossless configurations; `k_shade_surface` (or its ruled arrangement) within its rows at every landing |
| B Shadows and bounce limits | `record_shadow_hit` shared by an Embree occlusion filter and OptiX `ah_record` (K = 4, tie order, overflow flag); the shadow item's hit side buffer; `shade_shadow` evaluating recorded hits through the VM `opacity` entry with re-queue beyond K; the "treated as opaque" report entry removed; coloured alpha shadows; 0002 D11's test (n = 1, K, 3K+1); 0004 D3 transparent limits; foliage K measurement; B-19 on b_foliage again | 12 + 3 reserve = 15 | 0002 D11 transparent-shadow test; 0004 D3; B-19 on b_foliage stated |
| C Homogeneous media | Distance-only volume NEE (0006 D10) and D14; shadow rays through media with recorded hits (D16-D19); the camera's initial stack (D20); world volume and homogeneous Absorption, Scatter (HG) and Principled Volume constants with their lowerings and `ar_edit_set_graph` target VOLUME (D21; BACKLOG rows from 179); nested dielectrics on the real dielectric lobe; false interfaces in `shade_surface_ext` (BACKLOG row from 219); volume guides (BACKLOG row from 174); B-17 with real majorants | 16 + 4 reserve = 20 | 0006 V2-V5, V7 (HG), V8, V9 (homogeneous); 0003 check 9; A6, A7 |
| D Viewport: controller and denoiser | `render_scale` in the session (today refused, session.cpp line 345); the controller in the addon (0007 d34, d49) with B-33; viewport OIDN with quality stepping (0008 d18) and B-26 at 1080p and 4K; OIDN cancel (BACKLOG row from 197); guide debts (rows from 174 and 196); 0007 V11, V13 soak, V14 fail path; 0008 V11 viewport. Independent of slices A-C: its engine half may be pulled forward whenever the gpu or blender lane is idle | 12 + 3 reserve = 15 | 0007 V11, V13, V14; 0008 V11 (viewport); B-26; B-33 |
| E Close | M-1 ratified on the real lobes with every film flag on; B-14 (with the controller, rule 2 of ROADMAP), V7a, B-19, B-20, B-05 re-measured; O1-O3 with cross-oracle agreement or single witness; A2, A10; D4 edit round trip; gallery (shader-ball sweep, thin-film set, glass of water, material test ball, foliage shadows, fog box, world fog) and report; retrospective; M3's planning session | 8 + 2 reserve = 10 | ROADMAP M2 exit criteria |

### The spine in M2

Queue, one open task at a time on every header the `shade_surface` TU includes (`shade_surface.h`,
`stack.h`, `ggx.h`, `metal.h`, `ops.def` and the interpreter), by lead ruling: 239 -> 242 -> 247 -> 253 ->
260 -> 263 -> 261 -> 262 -> the arrangement task from 248 -> anisotropy -> transmission -> coat -> fuzz ->
thin film -> thin wall -> translucent base -> dispersion. 250, 255 and 258 edit `tables.h`, which that TU
also includes: they land one at a time and each states the TU's compile seconds. Of the library tasks only
253 edits headers in the TU (`ggx.h`, `metal.h` and two parameter functions of `stack.h`); 254 and 256-259
add new headers with their own tests, which no stage includes until their spine task.

Decision path for the `k_shade_surface` row, fixed now (it replaces PLAN-M1b's, whose limits were "for M1b"):
- Until the owner answers section 7's questions 1 and 2, the M1b lines stand: spills 16 / 16 B (ruling 93),
  compile 8.0 s (owner decision 43), stack 512 B (0002 D10). 261 and 262 are sized to fit them and stop if
  they do not.
- A breach follows 0002 D10's ladder in order; 248 has already measured steps 3 to 5's candidates, so the
  lead rules from its table instead of commissioning a new measurement.
- Spills between ruling 93's line and D10's ceiling (128 / 192 B): the lead may raise the row by ruling with
  the measured numbers, as PLAN-M1b allowed. Past the ceiling: the arrangement changes, never the ceiling.
- Compile time: no further raise without the owner (question 2).

## 3. The kernel-split decision: what M-1 and B-20 feed

The question: where do OpenPBR's nine lobes run? 0002 D8-D10 give the choices and their rules; M1b gave the
numbers in section 1. The candidates, with what is known today:

| Candidate (0002 D10 ladder step) | Known | Unknown, measured by 248 |
|---|---|---|
| F: fused as today, rows raised to D10's ceiling | proxy over the ceiling (528 B; 198 / 344 B); compile already at 7.33 of 8.0 s | nothing: this is the baseline that fails |
| L: fused, local fixes (step 2): lobe state recomputed instead of kept live across NEE's eval and the continuation | ruling 89 attributed every spilled byte of the proxy to `BsdfCtx` kept live; the production `stack_select` design already recomputes | the proxy's rows with its stand-ins patched to recompute as `stack_select` does (248, alone and with the best cap): this removes ruling 89's bias from the forecast; whether the real layered stack keeps the property is answered lobe by lobe on the spine |
| C: fused, cap re-tuned (step 3) to 168, 96 or 64 registers | spike A: the 64-register cap with 472 B of spills still beat uncapped; (256, 2) was 56 % faster than uncapped | pass time of today's kernel at each cap on B-1M, B-5M, b_tx, shader_balls; the proxy's spills at each cap |
| O: fused, the lobe stack's eval and sample out of line, record and register file by pointer | spike A: `__noinline__` by value is harmful, by pointer recovers most of it; task 208 used `AR_NOINLINE` once as a row remedy (40 -> 36 B). 0002 D1 and D10's "Never" list forbid `__noinline__`: this candidate needs the owner (section 7, question 1) | proxy rows and compile time with the stand-in stack out of line; pass time of today's kernel with `stack_eval` and `stack_sample` out of line |
| R: a rare lobe set in its own identity (step 4), for example transmission and translucency as a `ShadeKind` | 206 built the mechanism (`SurfaceKind<K>`, `shade_surface_ext` 124 / 400 B / 0 / 0, compile 5.41 s); costs one more VM TU of compile timing and one more sort and partition segment (B-19 pq) | needs an amendment to 0005 only (0002 D8 already lets the `ShadeKind` enum grow when 0005 justifies an identity); render cost on a scene without the rare lobe is expected near zero and is measured only if C and O fail |
| S: split `shade_surface` into bsdf and nee stages, or light selection into `sample_light` (step 5) | spike A: 22 % slower than the capped fused kernel; B-20 already unmet | measured on today's kernel only if C, O and R are all inadmissible |

Task 248 (slice 0, first to dispatch) measures L on the proxy and C and O on the proxy and on today's production
kernel (O in uncommitted patches only: ruling 114), records
each variant's registers, stack, spills, compile seconds and pass time, and fills this table, written here
before measuring (0002 D9: a remedy for a budget breach is "the fastest of the measured alternatives,
including re-tuning the cap"):

| Outcome | What follows |
|---|---|
| A variant puts the proxy inside D10's ceiling, keeps the production `k_shade_surface` inside D10 under the same variant, and holds today's kernel's pass time within B-38's 1.03 on all four scenes | That variant is the arrangement. The lead writes the arrangement task (one spine landing that applies it to the production kernel before any layered lobe); if the variant is O, only after the owner's answer to question 1. |
| Several variants do | The fastest on B-1M, then on shader_balls (0002 D9). |
| A variant fits the proxy but costs more than 1.03 on some scene | Not adopted by the lead. The table goes to the owner with candidate R's design sketch as the alternative; the library tasks (253-259) and slices B and D continue meanwhile. |
| No variant fits the proxy | Candidate R is specified (a 0005 amendment, drafted by the planning agent at the slice-0 review) and measured as the next task; S only after R. The spine holds at 262. |

Whatever the outcome, B-20's line is reported with it: the arrangement's pass time on B-1M and B-5M against
9.5 and 12.5 ms. Whether B-20 gates M2's close is question 3.

## 4. Order, dependencies and gate seconds of the first batch

```
debts:      245 -> 246     238, 243, 244 (independent)     blender lane, serial: 216, 240, 241, 264
slice 0:    248 forecast (first; quiet machine)    249 harness and agreement TU -> 253, 254, 257, 258, 259
            250 E-table tool -> 255      251 references (owner OK) -> 258 fuzz; 256's second witness
            252 null calibration (CPU capture, off-hours, after 248)
slice A:    254 dielectric -> 255 E_glass -> 256 walk oracle -> (stack: transmission, after 248)
            shade_surface TU queue: 239 -> 242 -> 247 -> 253 -> 260 -> 263 -> 261 -> 262 -> (arrangement task)
            tables.h, one at a time: 250, 255, 258        265 A1 rows after 249 (no dependency on 262)
```
Gate seconds: quiet main reads 137-147 s over the last ten landings (142.7 s was 213's landing gate step,
not a quiet-main median). Open M1b tasks declare +2.9 s (214, 216, 217, 236), the debts +3.5 s, this batch
+18.4 s (261 and 265 carry +3.5 s each for analytic scenes on both devices): 162-172 s on declared seconds,
and about 168-172 s once fixtures read their usual three times nominal, against the 170 s hold line. The
lead therefore writes a decision-38 move list (named long-stable statistical cases to `full`) before 261 and
265 land. Measured seconds replace declared ones as tasks land (owner decision 38); nothing is weakened.
Statistical lobe cases are written for the `full` family from the start, with one deterministic or
low-count gate case per lobe.

## 5. Lanes and models

Lanes are PLAN-M1b's (cpu, kernel, gpu, blender, tools, docs) under owner decision 42: two implementers at
once, at most one of them in gpu or blender, the blender lane serial in the order 216, 240, 241, 264; a landing runs beside them; kernel landings and captures get a
quiet window. The cpu lane carries slice A's library work; the kernel lane carries the spine and anything
that edits a header already in a VM TU (`ggx.h`, `metal.h`, `stack.h`, `ops.def`); the gpu lane has 248's
captures, then slice B's OptiX half and slice D; the blender lane has 240, 241, 216, 264, then lowerings.
Models: Sonnet by default; Opus implements the tasks marked O in `tasks/README.md` (sampling, estimators,
new lobe mathematics, measurement tasks whose table decides a design): 7 of this batch's 18 (owner decision 40 keeps the split), the lobe-heavy
front of the milestone, falling in slices B-D. Opus reviews kernel, physics, sampling, ABI, concurrency and
process diffs and every diff that writes into a record (ruling 87).

A lobe task is finished only with: its source named in the code (`Ref:` paper, section or equation, and the
reference file in the refs cache), V1 (furnace), V2 (chi-squared and the pdf integral), V5 (CPU/CUDA
weight agreement) through 249's harness, TransportMode in its signature (0005 item 8), the delta rule
through `lobe_is_delta`, and 247's result rules (no sample is pdf exactly 0; non-finite or negative is
invalid).

## 6. Measurement tasks

PLAN-M1b section 5 holds: metric and command, quiet machine, a decision table written before measuring, the
task's own bench history line, limits changed listed for the owner, no untried acceptance method.

| Measurement | Where | Decides |
|---|---|---|
| M-1 forecast: proxy and production variants (248) | slice 0, first | the arrangement (section 3) |
| Null calibration of 17(v): CPU against CPU, 8 seeds a side, 20 repeats on three scenes (252) | slice 0 | whether the tile Welch test gates anything in M2 (ROADMAP: at most 2 failing repeats per scene; else it gates nothing and sample counts or m rise) |
| Oracle availability: Mitsuba 3 and pbrt-v4 run O1-O3 with explicit spectra | slice 0, after question 4 | cross-oracle agreement or "single witness recorded" per scene |
| Kernel row after every spine landing | slices A-C | the ladder, at the landing that breaches |
| Foliage K and B-19 pq with `ah_record` | slice B | K (0002 D11, PROVISIONAL 4); whether pq needs its first remedy task |
| B-17 with real majorants; the per-launch cap | slice C | the cap's owner item (4,096 PROVISIONAL, STATUS) |
| B-26 OIDN share at 1080p and 4K; B-33 controller response | slice D | quality stepping levels; the controller's PROVISIONAL scale levels |
| M-1 on the real lobes with every film flag; B-14, V7a, B-19, B-20, B-05 | slice E | the close |

## 7. Owner decisions needed

All nine are answered: 2, 4, 5, 7 and 9 by owner decision 46; 1 (candidate O is allowed if 248's table
ranks it the fastest admissible variant), 3 (B-20 does not gate M2's close), 6 (4,096 until slice C, then
set by rule) and 8 (20 min, enforced, task 272) by owner decision 47. The table is kept for its reasons.
Decision 47 also makes 248's A/B valid on a machine other than the reference machine: it decides by
ratios between variants, recorded with the machine id; resource rows come from sm_120 compile-only builds
(task 267).

| # | Question | Default until answered | Recommendation |
|---|---|---|---|
| 1 | May the lobe stack be called out of line with the record and register file by pointer (candidate O)? The records disagree today: 0002 D1 and D10's "Never" list forbid `__noinline__`, 0005 item 31 allows it for heavy ops, and 208 used `AR_NOINLINE` once; ruling 114 lets 208's use stand and makes any further use wait for the owner's amendment of 0002 D1, which should settle all three | O is measured by 248 in uncommitted patches, not adopted | Decide after 248's numbers: allow it for the lobe stack only if it is the fastest admissible variant, by an amendment to 0002 D1 and D10 |
| 2 | B-05 for `shade_surface` and the spill row in M2 (8.0 s and 16 / 16 B were set "for M1b") | the M1b lines stand; 261 and 262 stop if they do not fit | Set both once, from 248: the compile time of the chosen arrangement at full proxy scope plus 10 %, spills at D10's ceiling |
| 3 | Does B-20 (1.25x Cycles: 9.5 / 12.5 ms) gate M2's close now that the lobe set completes (owner decision 37 deferred it until then)? Today 12.36 / 12.94 ms | reported at every slice close, not gating | Keep 1.25x as the release target; gate regression per landing (B-38 1.03) and ask again at the close with the measured arrangement |
| 4 | Downloads: `tizian/ltc-sheen` (the fuzz fit, Apache-2.0; 0005 item 6 names it), the supplemental code of Heitz et al. 2016 if its licence allows reading, author-hosted PDFs of the cited papers, an open thin-film text (Byrnes 2016, arXiv:1603.02720) for 259's oracle, Mitsuba 3's source (the chi-squared method of 0005 V2 and a second witness for V3), the ASWF Standard Shader Ball and Bitterli's Glass of Water and Material Test Ball (ROADMAP "Owner sees"), Mitsuba 3 and pbrt-v4 as oracle programs (0009) | 251 writes the rows and fetches nothing; 256 and 258 wait | Allow the fit and the assets now; oracle programs after 251 states their size and licences |
| 5 | Task 236's prefilter choice (STATUS: options a, b, c) | (b) as the lead recommends | Needed before slice D chooses the viewport denoise mode; (c), a guide-side remedy, would become a slice D task |
| 6 | B-17 and the per-launch medium cap (STATUS: 4,096 PROVISIONAL or about 1,000) | 4,096 | Decide at slice C's measurement with real majorants |
| 7 | May M3's first slice (texture system, cpu lane) start beside M2 (PLAN-M1b section 7)? | no | Yes once slice A's library tasks are dispatched: they and M3's slice share only the cpu lane |
| 8 | B-10 (`check full` 12 min, enforced by no script; 740-855 s quiet today): 245, 250, 255 and 256 each add a `full` step | unenforced; each task states the new total | Enforce or restate it once, at slice 0's close, from the measured total |
| 9 | Does M1b close when 214, 216, 217 and 236 are done, before the debts 238-247 land? | yes: the exit criteria rest on those four; 238-247 run as M2's slice 0 | yes |

## 8. After M2

ROADMAP's order stands. M3's planning session runs during slice D or E, when the corpus run it shares with
M2 can be scheduled; M-3 to M-5 and M-8 are its measurements, and it inherits whatever arrangement section 3
produced, because every M3 opcode adds an interpreter case to each VM TU.
