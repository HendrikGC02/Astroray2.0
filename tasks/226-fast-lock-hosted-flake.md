# 226 tests/tools/fast_lock.ps1 fails on the hosted runner: find the timing-sensitive check and make it deterministic
Goal: hosted `ci` failed `tools tests: FAIL fast_lock` on two of three runs of 186's landing (2026-11-25, runs 45540806790 and 45541629988), while the same file passes locally and on main's ci after 212; 186 touches no tool, and ci prints only the step summary, so the failing check is unknown -> the failing check is named (ci prints every FAIL line of a failing tools fixture), its cause is found by running the fixture on a 2-core affinity mask locally, and the check asserts on lock state or ordering instead of wall time, so it passes on 2-4 cores without a skip or a longer sleep that hides an ordering bug. [lane tools; Sonnet; Opus review (concurrency)]
Read first: tests/tools/fast_lock.ps1 (cases added by 204 and 224: window modes, -Landing, B-09 accounting, the judging waits and stand-in sleeps); tools/lib/lock.ps1; tools/check.ps1 (the 'tools tests' step, tag cpu, and how ci prints step output); tests/tools/run_fast.ps1; docs/decisions/0009 d8, d11.
Scope: tests/tools/fast_lock.ps1, tests/tools/run_fast.ps1 (print a failing fixture's FAIL lines in its summary so hosted logs name the check), tools/check.ps1 only if ci needs one line to surface those lines, tasks/226.canary.patch.
Out of scope: lock.ps1's semantics (if the fixture shows a real lock bug, stop and report), any limit, skipping or moving the fixture out of ci, raising timeouts as the fix.
Reference: none (process).
Do:
1. Surface the FAIL lines in ci output; reproduce on 2 cores (`Start-Process -Affinity` or `[System.Diagnostics.Process]::GetCurrentProcess().ProcessorAffinity = 3` for the fixture's process tree), 20 runs, and record which check fails and how often.
2. Make that check assert on state (marker contents, lock owner file, event ordering) rather than elapsed seconds; where a duration must be judged, judge it relative to a measured stand-in, never an absolute.
3. 20 runs on 2 cores green; a hosted ci run green.
Acceptance: `pwsh -NoProfile -File tests/tools/fast_lock.ps1` 20 times on 2 cores, all PASS (paste the count); `pwsh tools/check.ps1 fast` and `pwsh tools/check.ps1 gate` pass; the landing's hosted ci passes.
Look at: none.
Canary: tasks/226.canary.patch must turn red: `pwsh -NoProfile -File tests/tools/fast_lock.ps1` (a hunk that breaks the ordering the fixed check now asserts).
Budget: gate +0 s (tools tests step unchanged or faster).
Depends on: none. Collisions: tests/tools/run_fast.ps1 with 210; this lands first.
Size: S (about 60 lines); stop and report at 2x.
Progress:

226: 2-core runs before: fast_lock 19/20 red; timeout case 18, unknown-lines case 8, tree-waiting-line 1 (after first fix). Holders 3 s / 10 s -> release file with a 60 s cap; tree poll 10 -> 30 s; unknown case keeps 0.5/1.0 s limits and is judged by state (waited >= limit, <= waiter lifetime, 1..3 lines). run_fast prints FAIL lines.
