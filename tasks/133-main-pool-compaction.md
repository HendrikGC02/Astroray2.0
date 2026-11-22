# 133 D7 remedy (b): compact the main pool, and B-19's partition row
Goal: after remedies (c), the sort scans and (a) (tasks 118-123), B-19's idle row is met (9.2 % / 9.4 %) but `queue_build` + sort is not (18.9 % B-1M, 19.6 % B-5M, against 10 %); ROADMAP's measurement table requires remedy (b) before M1a closes (spike C does not run: B-14 passes on B-1M after 130) -> measure how much of the partition cost scans dead slots, adopt main-pool compaction if the measurement says it pays, and state B-19's outcome for the M1a close.
Read first: 0002 D6 (pools, segments, the free list, determinism), D7 (remedies; (b) as written: compact when live paths drop below a quarter of the high-water mark, Cycles `compact_main_paths`), D8 (kernel list, the partition kernels), Risks 2-3; ruling 79, ruling 80; 0009 B-19, B-17; 123's PR body (`gh pr view 94`: method, tables) and 120's (`gh pr view 91`); src/host/scheduler.{h,cpp}, src/kernel/sched/step_table.h, src/kernel/stages/queue_build.h, src/host/state_alloc.cpp.
Scope: src/kernel/ (a compaction kernel or a change to the partition's range, one source for CPU and GPU), src/host/scheduler.* and state_alloc.* (when to compact, the partition range), src/kernel/budget.toml (rows for a new kernel), tests/cpu and tests/gpu (scheduler tests: compaction keeps every live item exactly once, results and counters identical with and without), docs/decisions/0002-kernel-architecture.md (D7's (b) line: the measured outcome, as 118 and 120 recorded theirs), tools/budgets.toml (B-19 only from measurement).
Out of scope: the light-path pool rules beyond what compaction must preserve; changing kSortMinMaterials or the sort key (ruling 80); the D10 ladder.
Reference: 0002 D7 (b); Cycles `compact_main_paths` (intern/cycles/integrator/path_trace_work_gpu.cpp, read for the rule only, no code copied); Merrill and Garland 2016 for any scan reused.
Do:
1. Measure first (nsys, 123's method, B-1M and B-5M at 1080p 1 spp, 64 passes after 8): per step, the main pool's high-water mark, live items, and queue_build + sort time; from those, the share of partition time spent on slots that are TERMINATED for the rest of the pass, and the most compaction could save. Report this before building anything if the saving cannot bring the row under 10 %.
2. If it pays: the smallest design that removes the dead-slot work (physical compaction of live state rows, or a partition range that shrinks to the live high-water mark if the free-list order allows it; say which and why). Results stay deterministic and identical on CPU and GPU; film, counters and the invalid total are bit-identical with compaction forced on and off on the gate scenes.
3. Re-measure B-19 both rows and B-17 on B-1M and B-5M with the same method; table before -> after.
4. Outcome: row met -> record it in 0002 D7 and reset B-19 from measurement if PROVISIONAL; still failing -> record what remains and why, for the M1a close (owner decision).
Acceptance: `pwsh tools/check.ps1 gate` passes; the PR body holds the step table and the before -> after B-19 and B-17 tables with commands.
Look at: none.
Canary: tasks/133.canary.patch must turn red: the compaction tests (name them in land format).
Budget: under 1 s of gate time for the new tests; budget.toml rows for any new kernel.
Depends on: 123.
Size: M (about 250 lines); stop and report at 2x, or after Do 1 if compaction cannot meet the row.
Progress:
- started on task/133-main-pool-compaction; Do 1 (measurement gate) first.
- Do 1 measured (nsys, 123's method, 1080p 1 spp, 64 after 8, build 79e0213 plus an uncommitted per-step trace): at 1080p the main pool's capacity equals the pass's work (2,073,600 < 2^21), so one refill at step 2 fills it, hw_main stays 2,073,600 and a TERMINATED main slot stays dead for the rest of the pass.
- Every queue_build step costs the same whatever is live (count 40-45, scan 8, scatter 81-87 us); step 1 (shadow pool only, 4,147,200 items) costs 98 us, so the main pool's whole share is 32-36 us per step, 0.61 ms (B-1M) / 0.69 ms (B-5M) per pass.
- Items scanned per pass: main dead 25.8 / 26.8 %, shadow dead 65.2 / 66.5 %, live 9.0 / 6.7 % (B-1M / B-5M). The shadow pool is partitioned over its whole 2x capacity every step.
- Most main-pool compaction could save (free, range = live every step): 0.49 / 0.58 ms; Cycles' quarter rule 0.47 / 0.53 ms; the row needs 1.23 / 1.25 ms. queue_build 17.3 -> 14.6 % (B-1M), 17.4 -> 14.1 % (B-5M) at best; even a main range of zero leaves 13.9 / 13.5 %.
- Stopped after Do 1 as the task says: main-pool compaction cannot bring the row under 10 %; nothing built. Shrinking the shadow pool's partition range to its live items would save about 1.7 ms (proportional estimate) and is outside this task's Scope (step_table.h free-list contract, 0002 D5/D6).
