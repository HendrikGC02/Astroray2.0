# Status

Overwritten by the lead only. Live facts (branches, PRs, last gate) come from `tools/status.ps1`, never from here.

## Current milestone
M2 "Lobes, shadows and media" (docs/PLAN-M2.md: six slices, the `shade_surface` spine, the kernel-split decision of
task 248). Slice 0 is open: order and batches in tasks/README.md, runbook and handover in docs/LEAD.md, items
without a task in docs/BACKLOG.md. Landed in M2 so far: 238, 239, 242, 267. Written and not dispatched: 240, 241,
243-266 (scope-checked and critiqued by the eighth lead) and 267-272 (ninth lead: the second machine and the
M1b retrospective's open recommendations; pre-flighted, no Opus critique yet).
M1b "First light" closed 2026-11-28 (owner decision 47): the owner looked at the gallery and report (25 images;
grating orders, OIDN before and after, metal edge colour) and approved all of it. Accepted as measured: B-19 idle
11.9 % on B-1M and pq 10.9 % on b_foliage, B-20 12.36 / 12.94 ms against 9.5 / 12.5 ms, b_tx under the final denoise
reported, 0001 V5 deferred to task 269. Numbers and lessons: docs/retro/M1b-retrospective.md; batches and gate
ledger: docs/history/M1b-task-ledger.md. M1a "First pixels" closed by owner decision 37.

## Machines (owner decision 47)
The reference machine is the desktop, `rtx5070ti-8c16t`: every absolute budget row, kernel resource row and bench
baseline was measured there. Development continues on a laptop with an RTX 3000-class GPU from a fresh clone
(docs/SETUP.md). 267 landed: `build.ps1` builds the kernels for the installed GPU and gates the sm_120 rows compile-only. Until 268
lands a red timing budget on the laptop is expected and reported, never fixed by editing a limit. Ruling 117: the
desktop read 6 % slow on 2026-11-29 (cause not found); its B-38 baseline was re-recorded, re-measure after a restart.

## Owner decisions still open
None blocks work. Decision 47 answered PLAN-M2 questions 1, 3, 6 and 8, the coincident-hit tie rule, B-10, B-11
and the PROVISIONAL rulings 94, 100, 109, 110, 112, 114 under the owner's delegation; the owner may correct any.
- The laptop, as the owner states it: RTX 3000 with 8 GB (with a Core Ultra 7 165H that is the RTX 3000 Ada, sm_89: confirm
  with `nvidia-smi` on first start), 64 GB RAM, 16 cores / 22 threads. The desktop cannot be reached while the owner travels: absolute rows close as
  "reference rows pending" and are re-measured there first when it is back (decision 47 e).
- After task 266 re-measures every prefilter mode with the assets present, the owner may revisit decision 46's
  option (b) for the final denoise.

## Red items (reference machine)
- B-20 not met (12.36 / 12.94 ms; 1.25x Cycles is the release target, not a gate for M2: decision 47); cumulative
  pass time +5.7 % / +5.4 % across M1b on scenes that use no M1b feature, reported at every slice close.
- B-19 idle 11.9 % on B-1M and pq 10.9 % on b_foliage, accepted (decisions 37, 44); pq is re-measured at slice B's
  close with `ah_record` on.
- The full-scope proxy `ar_probe_m1` is over 0002 D10 (128 / 528 B / 198 / 344 B): the layered lobes do not fit
  the fused kernel as built. Task 248 measures the arrangements; the spine holds at 262 until its table is ruled.
- B-10: 20 min enforced by task 272 (740-855 s quiet today). Below sm_120 the kernels compile far over 0002 D10 (267: sm_89
  `k_shade_surface` 1,152 B stack, 76 / 124 B spills): laptop pass times are indicative only, 248 is confirmed on the desktop.
- Silent wrong result, unfixed (rule 5): a missing image renders magenta with only a warning, so comparisons pass when
  render and reference miss the same asset (BACKLOG; 245 and 246 remove the worktree cause; the loud failure has no task: 271).

## Budget headroom (reference machine, at the M1b close)
`check gate` quiet-main median 127.2 s, landings 133-149 s, hold line 170 s, limit 180 s (B-09). `check fast` 5.4-7.2 s
of 10 s (B-08). `check full` 740-855 s. Hosted `ci` 335-379 s of 450 s (B-11). `k_shade_surface` 128 registers, stack
416 B of 512, spills 4 / 16 B of 16 / 16 (ruling 93, carried into M2 by decision 46), compile 7.37 s quiet of 8.0 s:
about 0.6 s and no spill headroom left, which is why 248 runs first. `k_shade_surface_ext` 124 / 400 B / 0 / 0,
5.41 s. Declared by the open batches: +21.9 s of gate (debts 3.5, M2 first batch 18.4) plus 267-272's own. History: bench/history/2026.jsonl; landings: docs/retro/land-history-rtx5070ti-8c16t.jsonl. Spikes: docs/spikes/*/RESULTS.md.
