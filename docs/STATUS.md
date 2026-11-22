# Status

Overwritten by the lead only. Live facts (branches, PRs, last gate) come from `tools/status.ps1`, never from here.

## Current milestone
M1b "First light" (docs/PLAN-M1b.md: slices, lanes, the `shade_surface` spine, gate ledger). First batch
tasks/136-161, order and lanes in tasks/README.md; runbook and handover in docs/LEAD.md; items without a task in
docs/BACKLOG.md. Dispatched: 157, 137, 146, 152, 144, 136, 159, 155. B-31 is task 161.
M1a "First pixels" closed (owner decision 37): textured, lamp-lit meshes render progressively in the Blender 5.3
viewport and F12 on CUDA and CPU through the final C ABI; gallery and report from `tools/gallery.py --milestone
M1a` and `tools/report.py`. Accepted at the close: B-19 idle 11.0 % (10 % stays the target), B-20 2.00x / 1.63x
(1.25x is the release target; regression-gated by 140 until M2), B-31 39.8 ms (one slice-0 task).
Process: owner decision 36 confirmed; `process/guards` merged (tool deny rules, allow-list, CLAUDE.md rule 1's
scope clause).

## Owner decisions still open (planning session's recommendation in brackets; docs/PLAN-M1b.md section 6)
- Opus implements 10 of the first 25 tasks (kernel, sampling, concurrency), as in M1a; decision 31 says Sonnet
  implements. [Keep.]
- Gate time: 163.5 s of B-09's 180 s; the batch declares +53 s and task 157 should return 28 s or more. If the
  projection passes 170 s after regrouping: [move long-stable statistical cases to `full`, run `full` every 10
  landings, keep 180 s].
- `check perf` tolerances (tasks 140, 160) are set at three times the measured spread as a new PROVISIONAL row
  B-38. [Accept after reading their numbers.]
- Earlier: fold rulings 64-85 into the records and delete superseded records (0009 q4) [yes]; exact JPEG parity
  (ruling 78) [leave].

## Red items
- B-5M bench hangs before its first pass on most runs since 1a7e364 (task 136); it blocks the B-5M rows of B-14
  and B-19 and task 140.
- B-19 idle, B-20, B-31 accepted at the M1a close (decision 37); B-31 is task 161 (blender lane, after 159). M-1 probe spills 130/376 B against 128/192: task 144 (0002 D10 ladder).
- No regression check on performance and no F12 gate on CUDA until tasks 140, 160 and 139 land.

## Budget headroom
`check gate` 163.5 s of B-09's 180 s (kernel part 33.8 s of 120 s); ledger in tasks/README.md. `check fast`
5.7 s of B-08's 10 s with the build step outside the window. `check full` about 10-15 min. `k_shade_surface`:
128 registers, stack 384 B, spills 0 / 0, compile 4.6 s of ruling 72's 5.0 s; decision path in docs/PLAN-M1b.md
"The spine". History: bench/history/2026.jsonl. Spikes: docs/spikes/*/RESULTS.md.
