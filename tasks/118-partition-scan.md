# 118 Single-pass partition scan (0002 D7 remedy (c)), measured against B-19
Goal: 093 measured queue_build at 61 % of a B-1M pass (B-19 allows 10 %), and k_queue_build_scan alone at 0.85 ms per launch, 20 launches per pass: it runs one 256-thread block over 16,200-24,300 tile histograms -> the partition becomes one single-pass scan across many blocks, so its cost scales with live items, not with one block's serial loop, and B-19's partition share is re-measured.
Read first: 0002 D6 (block histogram), D7 (step, remedies, ruling 79 on their order), D8 (kernel list), Risks 2; src/kernel/stages/queue_build.h; tools/bench.ps1 and 093's PR body (Result: section, nsys method); 0009 B-17, B-19.
Scope: src/kernel/stages/queue_build.h, src/kernel_gpu/queue_build_*.cu, src/kernel_cpu/queue_build_*.cpp, src/kernel/launch_config.h (rows, kQueueTile), src/kernel/budget.toml (rows), src/host/scheduler.cpp (launch sequence only), tests/cpu/test_queue_build.cpp, tests/gpu/test_queue_build_agree.cpp, docs/decisions/0002-kernel-architecture.md (D7 remedy (c) adopted, D8 kernel list).
Out of scope: the material sort (119); fixed grids and graph capture (120); main-pool compaction (remedy (b), only if 120's re-measurement still fails B-19).
Reference: Merrill and Garland 2016, "Single-pass Parallel Prefix Scan with Decoupled Look-back" (NVIDIA NVR-2016-002), sections 4-5; Blelloch 1990 for the CPU path (one source: the same stage functions compile for both targets, rule 4).
Do:
1. Replace the count/scan/scatter triple's single-block scan with a multi-block single-pass scan (decoupled look-back over tile aggregates, or a fused count+scan+scatter if the record's (c) reading allows), keeping the step table and segment order identical (item-index order, D6); the CPU build runs the same stage functions in tile order.
2. Step table and segments bit-identical CPU vs GPU and vs the current implementation on the existing queue_build tests; a new test with >= 100,000 items across >= 3 segments and empty segments.
3. Re-measure with tools/bench.ps1 and nsys on b1m and b5m at 1080p, 1 spp, 64 passes after burn-in (093's method): partition GPU time as % of pass, idle %, pass ms; put the table and commands in the PR body (Result:).
4. Write back 0002 D7 (remedy (c) adopted, with the measured share) and D8 (kernel names) in the same change.
Acceptance: `pwsh tools/check.ps1 gate` passes; the PR body holds the before/after table; partition share on b1m and b5m reported against B-19.
Look at: out/evidence/118/timeline_b1m.png (one pass, as 093's plot).
Canary: tasks/118.canary.patch must turn red: astroray_tests_cpu with the new queue_build case (the patch drops one tile's aggregate from the look-back), astroray_tests_gpu queue_build agreement case.
Budget: no gate time added; kernel rows recorded at measured values; compile time within B-05.
Depends on: 093.
Size: M (about 300 lines); stop and report at 2x.
Progress:
- started
