# Status

Overwritten by the lead only. Live facts (branches, PRs, last gate) come from `tools/status.ps1`, never from here.

## Current milestone
M1b "First light" (docs/PLAN-M1b.md: slices, lanes, the `shade_surface` spine, gate ledger). First batch
tasks/136-161, order and lanes in tasks/README.md; runbook and handover in docs/LEAD.md; items without a task in
docs/BACKLOG.md. Slice 0 closed (all of 136-144, 155, 157, 159-162 landed; `check full` green 659 s; landings by `pwsh tools/land.ps1 submit`). Also landed: 145-158, 163, 164, 165, 172, 182, 183, 186, 189, 204, 211, 212, 224-228, 168, 169, 171, 176-181, 185, 199, 200, 202, 203, 166, 173, 174, 187, 195, 210, 221, 222, 223, 229; fourth lead: 175, 233, 231, 232, 196, 234, 192, 197, 191, 198; fifth lead: 205, 190. Second batch 163-183 (ruling 91), third batch 186-203 and 204 written;
spine: 205 landed (owner decision 43), 219 next; open work and next steps in docs/LEAD.md's handover.
M1a "First pixels" closed (owner decision 37): textured, lamp-lit meshes render progressively in the Blender 5.3
viewport and F12 on CUDA and CPU through the final C ABI; gallery and report from `tools/gallery.py --milestone
M1a` and `tools/report.py`. Accepted at the close: B-19 idle 11.0 % (10 % stays the target), B-20 2.00x / 1.63x
(1.25x is the release target; regression-gated by 140 until M2), B-31 39.8 ms (one slice-0 task).
Process: owner decision 36 confirmed; `process/guards` merged (tool deny rules, allow-list, CLAUDE.md rule 1's
scope clause).

## Owner decisions still open
- Answered 2026-11-27 (owner decision 43): B-05 for shade_surface 8.0 s (205 lands; the spine queue 219, 220, 208, 206, 209 follows); D10 tree row 1.8x accepted for full MIS (190 lands); v1 margin = the shifted statistic (190); V11 (201): a remedy task for small textured detail, a different third scene in place of b_tx, and `tools/fetch.ps1` may fetch the Cornell and shader-ball/HDRI assets for a re-run (201 resumes on cornell_area, shader_balls, cornell_point; remedy task 236).
- Coincident-hit tie rule (task 235, closed by the lead with its finding): d2_aov_geometry's CPU/CUDA mean difference (R -0.031, B +0.031, 633 / 365 SE) is the coincident box pair 301/302 that 0009 d17(iv) and the scene's meta.toml declare a traversal tie: Embree reports 302 on every pair pixel, OptiX 301; with the pair's colours equal the two devices agree to the last digit, swapped the difference flips sign. No engine defect. Image-mean CPU/CUDA comparisons on d2 mask the pair (201). Owner, optional: a deterministic tie rule on both backends (any-hit on GPU triangles plus an Embree filter, lowest instance id; a design record and a perf cost), or a loud warning on coincident faces with different materials; otherwise nothing.
- B-10 (`check full` 12 min): full reads 777.5 s after 234 (798.5 s before it on 19a0211c); no script enforces B-10 (check.ps1 prints "budget n/a" for full). Owner: enforce, raise, or move work.
- B-11 (hosted ci 450 s): after 233 the lean tier reads 335-379 s; its basis (1.3 x the old max) no longer derives the limit (1.3 x 379 = 493 s). Limit unchanged; the owner may restate the basis.
- Ruling 101 (2026-11-25): B-38 baseline re-recorded on quiet main (b1m 11.69 -> 12.01 ms, b5m 12.28 -> 12.56 ms; +2.5 % cumulative since 2026-11-23 across M1b landings; limit 1.03 per landing unchanged); 215 reports the cumulative B-20 line.
- k_shade_surface compile: owner decision 41 (2026-11-26) raises B-05 to 7.5 s for M1b; 175's light tree reads 7.31-7.40 s (main 6.76-6.78 s after 173 and 174). The timing still includes about 0.77 s of vcvarsall per compile (BACKLOG). Ruling 103: the spill rows after 175.
- Ruling 100 (2026-11-25, PROVISIONAL): B-09 also excludes the gate's compile-timing step (old: excluding build steps only, ruling 94); landing gates of kernel tasks read 179.8 s with 23 s of real timed compiles. Task 224. Rulings 97-99: shade_surface_ext may take 7.0 s B-05 (old 3.0 s default); medium-stack id for M1b; B-09 remedies 157 and 212.
- Ruling 96 (2026-11-25): CPU/CUDA agreement through the RGB-to-spectrum lift is gated by its parts (pre-lift 1e-6, lift bit-identity, grey radiance 1e-6); colour radiance printed, ungated (172 measured 1.36e-5 against D12's 1e-5, conditioning of the lift).
- Ruling 95 (2026-11-25): k_route_hit regs 28 -> 40 for 182's medium back-face test, a lead ruling on a D10 row.
- Ruling 94 (2026-11-25, PROVISIONAL): B-09 excludes the gate's build steps, as B-08 does since decision 36 (old: wall time incl. incremental build; limit 180 s unchanged). Landing gates rebuild after the canary and read 164-201 s; quiet main without a rebuild 145.5 s. The owner may revert it.
- Ruling 93 (2026-11-24): k_shade_surface spills up to 16 / 16 B for M1b (old 0 / 0), a lead ruling on a D10 row.
Earlier: Owner decision 40 (2026-11-24) accepted B-31 cold as measured and left the rest to the lead: B-05 for
`shade_surface` is 7.0 s for M1b (145 measured 5.16 s quiet), Opus for O tasks kept, rulings 64-90 fold into the
records (task 185) and superseding ADRs delete what they supersede, JPEG parity left.

## Red items
- B-19 idle, B-20, B-31 accepted at the M1a close (decision 37); B-31 instance loop warm S2 28.0 ms per 10k after task 161 (limit 30), cold 82.7 accepted as measured (decision 40). M-1 probe spills 130/376 B come from spike A's stand-in lobes (ruling 89, task 144): not ratified, slice D re-measures.
- `check perf` (140) gates kernel/device/session landings at B-38 1.03 PROVISIONAL (owner decision 39); F12 on CUDA is gated (139).
- 204 landed (B-09 without build steps, landing gate holds the fast window). New: a cold first lint after a rebase failed B-08 in two landings (BACKLOG); 163's landing gate read 179.8 s (quiet main after it 157.3 s).
- B-05 shade_surface at 6.0-6.4 s of 7.0 s after 163's branch: the spine (173, 174, 175, 192, ...) may reach 7.0 s; past it is an owner item (decision 40).

## Budget headroom
`check gate` quiet-main median 127.2 s after task 234 (was 167 s; 145.5 s after 176); landings under load
133-183 s (owner decision 38: measured ledger in tasks/README.md, hold above 170 s measured). `check fast` 6.9-7.2 s
of B-08's 10 s. `check full` about 10-15 min. `k_shade_surface`: 128 registers, stack 400 B, spills 8 / 8 B (ruling 93: up to 16 / 16 for M1b), compile about 5.6 s quiet, 6.0-6.4 s under load on 163's branch of decision 40's 7.0 s (every VM opcode adds an interpreter case to this TU), watched along the spine queue; decision path in docs/PLAN-M1b.md
"The spine". History: bench/history/2026.jsonl. Spikes: docs/spikes/*/RESULTS.md.
