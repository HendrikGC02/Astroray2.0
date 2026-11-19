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
