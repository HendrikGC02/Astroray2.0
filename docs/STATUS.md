# Status

Overwritten by the lead only. Live facts (branches, PRs, last gate) come from `tools/status.ps1`, never from here.

## Current milestone
M1b "First light" (docs/PLAN-M1b.md: slices, lanes, the `shade_surface` spine, gate ledger). First batch
tasks/136-161, order and lanes in tasks/README.md; runbook and handover in docs/LEAD.md; items without a task in
docs/BACKLOG.md. Slice 0 closed (all of 136-144, 155, 157, 159-162 landed; `check full` green 659 s; landings by `pwsh tools/land.ps1 submit`). Also landed: 145-158, 164 pending, 165, 168, 169, 171, 176-181, 185, 199, 200, 202, 203. Second batch 163-183 (ruling 91), third batch 186-203 and 204 written;
spine next 163 (reviewed, lands after 204), then 173; open work and next steps in docs/LEAD.md's handover.
M1a "First pixels" closed (owner decision 37): textured, lamp-lit meshes render progressively in the Blender 5.3
viewport and F12 on CUDA and CPU through the final C ABI; gallery and report from `tools/gallery.py --milestone
M1a` and `tools/report.py`. Accepted at the close: B-19 idle 11.0 % (10 % stays the target), B-20 2.00x / 1.63x
(1.25x is the release target; regression-gated by 140 until M2), B-31 39.8 ms (one slice-0 task).
Process: owner decision 36 confirmed; `process/guards` merged (tool deny rules, allow-list, CLAUDE.md rule 1's
scope clause).

## Owner decisions still open
- Ruling 94 (2026-11-25, PROVISIONAL): B-09 excludes the gate's build steps, as B-08 does since decision 36 (old: wall time incl. incremental build; limit 180 s unchanged). Landing gates rebuild after the canary and read 164-201 s; quiet main without a rebuild 145.5 s. The owner may revert it.
- Ruling 93 (2026-11-24): k_shade_surface spills up to 16 / 16 B for M1b (old 0 / 0), a lead ruling on a D10 row.
Earlier: Owner decision 40 (2026-11-24) accepted B-31 cold as measured and left the rest to the lead: B-05 for
`shade_surface` is 7.0 s for M1b (145 measured 5.16 s quiet), Opus for O tasks kept, rulings 64-90 fold into the
records (task 185) and superseding ADRs delete what they supersede, JPEG parity left.

## Red items
- B-19 idle, B-20, B-31 accepted at the M1a close (decision 37); B-31 instance loop warm S2 28.0 ms per 10k after task 161 (limit 30), cold 82.7 accepted as measured (decision 40). M-1 probe spills 130/376 B come from spike A's stand-in lobes (ruling 89, task 144): not ratified, slice D re-measures.
- `check perf` (140) gates kernel/device/session landings at B-38 1.03 PROVISIONAL (owner decision 39); F12 on CUDA is gated (139).
- Landings fail on machine load (B-09 183-201 s beside builds; B-05 shade_surface 7.29 s beside builds, 5.2-6.4 s otherwise): task 204 and ruling 94; until then land in a quiet window. `check full` due after the next landing.
- B-05 shade_surface at 6.0-6.4 s of 7.0 s after 163's branch: the spine (173, 174, 175, 192, ...) may reach 7.0 s; past it is an owner item (decision 40).

## Budget headroom
`check gate` quiet-machine 145.5 s steady after task 176 (132.7 s after 157); landings under load
133-183 s (owner decision 38: measured ledger in tasks/README.md, hold above 170 s measured). `check fast` 6.9-7.2 s
of B-08's 10 s. `check full` about 10-15 min. `k_shade_surface`: 128 registers, stack 400 B, spills 8 / 8 B (ruling 93: up to 16 / 16 for M1b), compile about 5.6 s quiet, 6.0-6.4 s under load on 163's branch of decision 40's 7.0 s (every VM opcode adds an interpreter case to this TU), watched along the spine queue; decision path in docs/PLAN-M1b.md
"The spine". History: bench/history/2026.jsonl. Spikes: docs/spikes/*/RESULTS.md.
