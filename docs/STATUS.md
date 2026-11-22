# Status

Overwritten by the lead only. Live facts (branches, PRs, last gate) come from `tools/status.ps1`, never from here.

## Current milestone
M1a "First pixels" is built, not closed: textured, lamp-lit meshes render progressively in the Blender 5.3
viewport and F12 on CUDA and CPU through the final C ABI; gallery and report from `tools/gallery.py --milestone M1a`
and `tools/report.py`. The close waits on the owner decisions below (ROADMAP: a milestone that breaks a gate does
not close). M1b "First light" is planned and ready to dispatch: docs/PLAN-M1b.md (slices, lanes, the
`shade_surface` spine, gate ledger), tasks/136-160 with order in tasks/README.md, runbook and handover in
docs/LEAD.md, items without a task in docs/BACKLOG.md. Next: dispatch the first batch.

## Owner decisions (planning session's recommendation in brackets; details in docs/PLAN-M1b.md section 6)
M1a close:
- B-19 GPU idle 11.0 % against 10 % on B-1M; the partition row is met at 9.4 % (ruling 85); the cause is the
  step-table round trip, no regressing commit. B-5M is not measured until task 136. [Accept for the M1a close,
  keep 10 % as the target for M1b's re-measurement.]
- B-20: 2.00x / 1.63x Cycles per 1080p sample against 1.25x. [Keep 1.25x as the release target; until M2's lobe
  set is complete gate on regression (task 140) instead. Spike C stays unrun: B-14 passes.]
- B-31: instance loop 39.8 / 88.5 ms per 10k warm / cold against 30; V7a is met (worst S1 1.17x). [Schedule one
  task in M1b's slice 0.]
M1b start and process:
- Starting M1b's first batch before M1a is formally closed. [Go ahead: nothing in the batch depends on the open rows.]
- Owner decision 36 records what the planning session applied on the relayed instruction: B-08 times lint and
  tests only; lanes instead of a global implementer limit; reviewer reads the canary patch, three verdicts;
  DeepSeek wrapper. [Confirm.]
- Branch `process/guards`, not merged: deny rules for direct cmake, ninja, nvcc and blender, an allow-list for
  `pwsh tools/*` and read-only git and gh, and a standing scope clause in CLAUDE.md rule 1. Permission settings
  and CLAUDE.md rules were left for the owner's own word. [Merge.]
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
- B-19 idle, B-20, B-31: open rows above. M-1 probe spills 130/376 B against 128/192: task 144 (0002 D10 ladder).
- No regression check on performance and no F12 gate on CUDA until tasks 140, 160 and 139 land.

## Budget headroom
`check gate` 163.5 s of B-09's 180 s (kernel part 33.8 s of 120 s); ledger in tasks/README.md. `check fast`
5.7 s of B-08's 10 s with the build step outside the window. `check full` about 10-15 min. `k_shade_surface`:
128 registers, stack 384 B, spills 0 / 0, compile 4.6 s of ruling 72's 5.0 s; decision path in docs/PLAN-M1b.md
"The spine". History: bench/history/2026.jsonl. Spikes: docs/spikes/*/RESULTS.md.
