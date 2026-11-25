# 227 B-08: the fast tier's CPU tests run in shards (remedy one for B-08; 212's method)
Goal: `check fast` on quiet main reads 9.2-9.4 s of B-08's 10 s (tests 7.1-7.2 s serial in one doctest process, lint 1.8 s), and a landing's fast step meets other worktrees' test runs (the fast window holds builds only): 189's landing read tests 8.4 s, fast 10.6 s, and refused; 183 and 186 read 10.0-10.4 s on their branches -> the fast tier's `astroray_tests_cpu --tier=fast` run is split into N doctest processes run in parallel (N = 3 locally, 1 on hosted ci), cut by 212's `cut_ends` from the last run's per-case seconds, with the same checks 212 added for the gate (cases summed against `--list-test-cases`, n build lines, family counts summed, same-name rule without the " f64" suffix, B-12 scan over all shards), so the fast tier reads about 6-7 s quiet. [lane tools; Sonnet; Opus review (concurrency)]
Read first: tools/check.ps1 (the fast tier's tests step and 212's shard helper for `cpu gate tests`), tools/gate_table.py (`cut_ends`, `--families`), tools/lib/testrun.ps1 (`Get-ArSlowTests`, B-12), tests/tools/fast_gate_table.ps1, docs/decisions/0009 d8, d11, the B-08 and B-12 rows; RULINGS 94, 99, 100; docs/BACKLOG.md "check fast B-08 headroom".
Scope: tools/check.ps1 (fast tier tests step only; reuse 212's helper, do not copy it), tools/gate_table.py only if a parameter is needed, tests/tools/fast_gate_table.ps1 (one case for the fast path), docs/decisions/0009 d11 (one sentence, ruling 87), docs/BACKLOG.md (the headroom row rewritten with the measured result), tasks/227.canary.patch.
Out of scope: B-08's 10 s and B-12's 0.41 s (no limit moves), moving or trimming test cases, lint, the gate tier, hosted ci sharding (1 shard there).
Reference: none (process).
Do:
1. The shard run for the fast tier through 212's helper, N = 3 locally, 1 on ci; the summed checks; B-12 over all shards.
2. Measure before and after on a quiet machine: `check fast` 5 runs each (median, max), state what else ran.
3. 0009 d11 sentence; BACKLOG row.
Acceptance: `pwsh tools/check.ps1 fast` passes 5 of 5 quiet (paste the median and max); `pwsh tools/check.ps1 gate` passes; `pwsh -NoProfile -File tests/tools/run_fast.ps1` passes; paste the last lines.
Look at: none.
Canary: tasks/227.canary.patch must turn red: `pwsh -NoProfile -File tests/tools/fast_gate_table.ps1` (the fast-path case: a hunk that drops a shard's cases from the sum).
Budget: fast -2 s or better; gate +0 s.
Depends on: none (212 landed). Collisions: tools/check.ps1 with 226 (run_fast/ci output) and 210 (row c); this lands first if ready first, the later one rebases.
Size: S (about 40 lines); stop and report at 2x.
Progress:
