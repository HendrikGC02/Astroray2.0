# 232 Timing checks re-run once when another process loaded the machine
Goal: wall-clock budgets fail landings on other worktrees' load, not on the change (166: tripwire 1.5x under an interactive Blender; 210 and 223: timing cases under load; B-08 and B-12 reds beside compiles; owner decision 42) -> each timing check (B-08 tier, B-12 per test, the B-16 tripwire, B-38 perf) records whether foreign load ran during its timed window. A failure with load seen re-runs that step once inside the lock or fast window and prints `RERUN <check>: load <processes>` with both results. The check is red when the re-run fails or when no load was seen. No limit changes. [lane tools; Sonnet; Opus review (process)]
Read first: owner decision 42; 0009 d8, d10, d11, d13 (no warning band: this task changes the method, not a limit) and rows B-08, B-12, B-16, B-38; tools/check.ps1 (the B-08 and B-12 code near `$perTest` and `B-12:`, the tripwire step `integrator cuda`); tools/integrator_checks.py (tripwire); tools/perf.ps1 (its foreign-GPU-user refusal, the model for load detection); tools/lib/lock.ps1 (the fast window).
Scope: tools/check.ps1 (load sampling around timed steps, the one re-run), tools/integrator_checks.py (tripwire re-run hook if the step cannot be re-run from check.ps1), tools/perf.ps1 (re-run on load instead of only refusing beforehand), tools/lib/ (one shared load-sampling helper), tests/tools/fast_timing_rerun.ps1 (new), docs/decisions/0009-build-test-process.md d13 (one sentence: the single re-run under foreign load), tasks/232.canary.patch.
Out of scope: the limits themselves; which tests sit in which tier (234); hosted ci (233).
Reference: 0009 d13 and d18 (the tripwire's 1.25x is "far outside the measured noise", so a red under load is the load); `Get-Process` and the GPU process list perf.ps1 already reads.
Do:
1. Load = any `cl`, `link`, `nvcc`, `ninja`, `blender` or GPU compute process not in this run's own process tree, sampled at the start, during (every 2 s) and at the end of a timed window. Print what was seen.
2. On a timing failure with load seen: re-run that step once (not the tier) and report both numbers; the second result decides. Without load, the first result decides.
3. A fixture with a fake timed step and an injected foreign process: red with no load, red when the re-run also fails, green with `RERUN` printed when the re-run passes.
Acceptance: `pwsh tools/check.ps1 fast` passes; `pwsh tests/tools/fast_timing_rerun.ps1` prints PASS; `pwsh tools/check.ps1 gate` passes.
Look at: none
Canary: tasks/232.canary.patch must turn red: `pwsh -NoProfile -File tests/tools/fast_timing_rerun.ps1` (the patch lets the first result decide even with load seen, and treats a passing re-run without load as green).
Budget: fast +1 s at most; gate +0 (a re-run only happens after a failure).
Depends on: none. Lands after 231 and 233 when they are ready first (tools/check.ps1); the later one rebases.
Size: S (about 140 lines without tests); stop and report at 2x.
Progress:
- Files: tools/lib/load.ps1 (new), tools/check.ps1, tools/perf.ps1, tests/tools/fast_timing_rerun.ps1, docs/decisions/0009 d13, tasks/232.canary.patch.
- B-08/B-12: fast tests step re-runs once (first attempt leaves the tier clock). B-16: gate re-runs integrator_checks.py --check tripwire only when tripwire alone failed. B-38: perf.ps1 re-runs the bench in the same lock hold. B-05 compile timing not covered (not in the task).
- check fast 9.1 s, gate 166.7 s PASS, fixture PASS, canary 232 PASS.
