# 134 B-19 after 133: the GPU-idle drift and a partition range that follows live slots
Goal: after 133 (ruling 84) B-19 still fails both rows: `queue_build` + sort 12.0 % (B-1M) / 11.8 % (B-5M) against 10 %, and GPU idle 13.9 % / 12.1 % although 123 measured 9.2 % / 9.4 % with the same method (already 13.2 % / 10.1 % on main before 133, so a regression landed between 123 and 133) -> find and remove the idle regression, and shrink the partition ranges to what is live, so both rows meet 10 % on B-1M and B-5M for the M1a close.
Read first: 0002 D5, D6, D7 (and its (b) line as 133 wrote it); RULINGS 79, 80, 84; 0009 B-19, B-17; 123's PR body (`gh pr view 94`: method) and 133's PR body (step tables, what remains); src/host/scheduler.{h,cpp}, src/kernel/sched/step_table.h, src/kernel/stages/queue_build.h.
Scope: src/kernel/sched/ and src/kernel/stages/queue_build.h (one source for CPU and GPU), src/host/scheduler.* and state_alloc.*, src/host/ files a bisected idle regression points to (name them in the PR), src/kernel/budget.toml, tests/cpu and tests/gpu (scheduler and queue_build tests), docs/decisions/0002-kernel-architecture.md (D6/D7 outcome lines), tools/budgets.toml (B-19 only from measurement).
Out of scope: the material sort's rule or key (ruling 80); the D10 ladder; Blender-side code; changing 123's measurement method.
Reference: 0002 D6, D7; Cycles `compact_main_paths` (intern/cycles/integrator/path_trace_work_gpu.cpp, rule only, no code copied); Merrill and Garland 2016 for any scan reused.
Do:
1. Idle first: measure GPU idle on B-1M at 8af25af (123) and on main with 123's method, nothing else on the GPU (`Get-Process blender` empty before each capture). If it reproduces, bisect the src/ commits between them (126, 130, 124, 125, 129, 128 touch the host) to the one that moves idle, and say what in it adds the gap. Fix it if the fix lies in Scope and keeps that commit's purpose; otherwise stop and report the commit and the mechanism.
2. Range, measure before building: from a traced run, per step and pool, the largest live slot index + 1 against the current range, and the estimated saving of (i) the shadow range falling back to its largest live slot + 1, (ii) the same for the main pool, (iii) main compaction with D7 (b)'s quarter rule.
3. Build the smallest of these that brings the partition row under 10 % on both scenes, with a sentence on why. The lead pre-approves (i) and (ii) as an amendment of ruling 84 (the high-water mark may fall to the largest live slot + 1 when a step's partition finds the slots above it all terminated; free and fresh slots keep index order) and (iii) as D7 (b) as written; the lead writes the ruling text from your PR. Results stay deterministic; film, counters and the invalid total are bit-identical with the change forced off, on CPU, as 133's tests do.
4. Re-measure B-19 both rows and B-17 on B-1M and B-5M; table 123 -> 133 -> after. Both rows met -> record in 0002 D7 and reset B-19 from measurement if PROVISIONAL; still failing -> record what remains and why (owner decision at the M1a close).
Acceptance: `pwsh tools/check.ps1 gate` passes; the PR body holds the idle bisect table, the step table of Do 2, and the before -> after B-19 and B-17 tables with commands.
Look at: none.
Canary: tasks/134.canary.patch must turn red: astroray_tests_cpu "queue_build/live_end is the end of the last tile with a live slot", astroray_tests_cpu "scheduler/live range matches kept high-water marks", astroray_tests_cpu "scheduler/shadow range matches the full shadow range", astroray_tests_cpu "scheduler/shadow range under a starved shadow pool".
Budget: under 1 s of gate time for the new tests; budget.toml rows for any new kernel.
Depends on: 133.
Size: M (about 250 lines); stop and report at 2x, after Do 1 if the regression is outside Scope, or after Do 2 if no option can meet the row.
Progress:
- 2026-11-23: started; branch task/134-b19-idle-and-live-range on 13edb94.
- Do 1: no regressing commit (8af25af, 81553dd, 1a7e364, 8b86bfd, 13edb94 within run-to-run noise of 0.3 ms); idle share rose because 133 shortened the pass.
- Do 2: shadow largest live + 1 falls fast, main stays near 1.6-1.85 M; model (i) 0.50 ms, (ii) 0.12, (iii) 0.71 (new kernel, out of Scope).
- Do 3: (i)+(ii) built; live_end tile-granular from queue_build_scan (exact per-slot variants broke count/scatter register budgets).
- Do 4: B-1M queue_build 12.0 -> 9.4 %, idle 11.0 %; B-5M not measured (bench hangs before the first pass, at main too).
