# 091 Measurement: process budgets (locks, tiers, Blender start-up, nvcc RAM, CI, doctest)
Goal: the process budgets are PROVISIONAL and the machine lock is unproven -> measure lock behaviour under concurrent runs, tier durations over five runs, Blender headless start-up per process, nvcc pool peak RAM, hosted `ci` duration and toolset, and doctest's compile cost against Catch2, then reset the PROVISIONAL rows from the results.
Read first: 0009 d7 (nvcc pool depth PROVISIONAL), d8 (locks), Verification rows 8 and 9-13, B-07, B-08..B-12, Risks (hosted runner, Blender start-up), must-exist list (the measurements that reset limits); tools/budgets.toml.
Scope: tools/measure_process.ps1 (one-off; deleted when this task lands), tools/budgets.toml, docs/decisions/0009-build-test-process.md (B-row texts reset from measurement, with "MEASURED" in the basis column), cmake/kernels.cmake (pool depth, only if the result says so).
Out of scope: engine budgets B-13..B-37 (093-096).
Reference: none.
Do:
1. Locks (0009 Verification row 8): start two `check gate` runs together and a `check fast` during a gate: the second gate waits (waited > 0 in its JSON), `fast` finishes without waiting and inside B-08, runs never overlap (timestamps from the JSON summaries), the PC stays up.
2. Tier durations: five consecutive runs each of `fast`, `gate` and (once) `full`; report min, median, max excluding lock wait against B-08, B-09, B-10; configure from scratch (B-07) three times.
3. Blender start-up: 10 headless `Invoke-ArBlender` runs of an empty script; median and max.
4. nvcc pool: peak working set of all nvcc and cicc processes during a clean dev build at depth 8 (sampled every 100 ms); if the peak exceeds half of physical RAM, lower the depth until it does not and record the depth.
5. Hosted CI: duration of the last five `ci` runs and the `cl` versions they logged (from the GitHub API).
6. doctest vs Catch2: compile time of one representative test TU (test_packet.cpp) with doctest and with Catch2 v3 amalgamated fetched to third_party/_cache temporarily (not committed): if Catch2 is not faster by more than 20%, doctest stays (expected).
7. Result (PR body `Result:`): every command and its numbers; on each outcome: a PROVISIONAL row whose measurement has at least 30% headroom gets that measurement plus headroom as its new limit; a row already breached is reported to the lead with the numbers instead (a broken budget leads to a split or a fix, never a silent raise, 0009 Risks).
Acceptance: the PR body holds the result table with commands; budgets.toml and 0009's table change consistently (lint passes); `pwsh tools/check.ps1 gate` passes on the final diff.
Look at: none.
Canary: none: measurement only.
Budget: resets PROVISIONAL process rows.
Depends on: 030, 010, 084.
Size: S (scripts plus a result table); stop and report at 2x.
Progress:
