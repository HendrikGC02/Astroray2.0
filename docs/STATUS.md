# Status

Overwritten by the lead only. Live facts (branches, PRs, last gate) come from `tools/status.ps1`, never from here.

## Current milestone
M1b "First light" (docs/PLAN-M1b.md: slices, lanes, the `shade_surface` spine, gate ledger). First batch
tasks/136-161, order and lanes in tasks/README.md; runbook and handover in docs/LEAD.md; items without a task in
docs/BACKLOG.md. Slice 0 landed: 155, 146, 157, 143, 152, 137, 144, 142, 153, 159, 149, 162, 151, 136, 154, 150, 138, 141, 139, 140, 161 (landings now `pwsh tools/land.ps1 submit`); second batch 163-183 written (ruling 91); open work and next steps in
docs/LEAD.md's handover.
M1a "First pixels" closed (owner decision 37): textured, lamp-lit meshes render progressively in the Blender 5.3
viewport and F12 on CUDA and CPU through the final C ABI; gallery and report from `tools/gallery.py --milestone
M1a` and `tools/report.py`. Accepted at the close: B-19 idle 11.0 % (10 % stays the target), B-20 2.00x / 1.63x
(1.25x is the release target; regression-gated by 140 until M2), B-31 39.8 ms (one slice-0 task).
Process: owner decision 36 confirmed; `process/guards` merged (tool deny rules, allow-list, CLAUDE.md rule 1's
scope clause).

## Owner decisions still open (planning session's recommendation in brackets; docs/PLAN-M1b.md section 6)
- Opus implements 10 of the first 25 tasks (kernel, sampling, concurrency), as in M1a; decision 31 says Sonnet
  implements. [Keep.]
- B-38 decided (owner decision 39: 1.03 PROVISIONAL); 160's share tolerances follow the same rule. B-31 cold 82.7 ms per
  10k against 30: only an ABI change moving the loop out of Python reaches it. [Accept cold as measured; warm is met.]
- Earlier: fold rulings 64-85 into the records and delete superseded records (0009 q4) [yes]; exact JPEG parity
  (ruling 78) [leave].

## Red items
- k_shade_surface compile at ruling 72's 5.0 s limit with task 145 (4.96-5.35 s under load); quiet-window landing,
  else an owner item.
- B-19 idle, B-20, B-31 accepted at the M1a close (decision 37); B-31 instance loop warm S2 28.0 ms per 10k after task 161 (limit 30), cold 82.7 (owner item: under 30 needs the loop out of Python). M-1 probe spills 130/376 B come from spike A's stand-in lobes (ruling 89, task 144): not ratified, slice D re-measures.
- `check perf` (140) gates kernel/device/session landings at B-38 1.03 PROVISIONAL (owner decision 39); F12 on CUDA is gated (139).

## Budget headroom
`check gate` quiet-machine max 132.7 s, median 123.1 s of B-09's 180 s after task 157; landings under load
133-179 s (owner decision 38: measured ledger in tasks/README.md, hold above 170 s measured). `check fast` 6.9-7.2 s
of B-08's 10 s. `check full` about 10-15 min. `k_shade_surface`: 128 registers, stack 384 B, spills 0 / 0, compile 4.6 s of ruling 72's 5.0 s; decision path in docs/PLAN-M1b.md
"The spine". History: bench/history/2026.jsonl. Spikes: docs/spikes/*/RESULTS.md.
