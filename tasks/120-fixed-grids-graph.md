# 120 Fixed grids and CUDA graph capture (0002 D7 remedy (a)), and the B-19/B-20 re-measurement
Goal: after 118 and 119, the remaining wavefront overhead on B-1M is GPU idle between launches (093: 11.4 % of the pass between launches, 3.5 % between passes) and the per-step host round trip -> steps launch with fixed grids from a captured CUDA graph, idle is re-measured, and B-19, B-17 and B-20 are reported with the outcome rule of 093 Do 5.
Read first: 0002 D4 (device-read counts, grid-stride loops), D7 (step, read_step_table round trip, remedies, ruling 79), Risks 2; 0009 B-14, B-17, B-19, B-20; 093's, 118's and 119's PR bodies.
Scope: src/host/scheduler.{h,cpp}, src/device/cuda/ (graph capture and replay, launch helpers), src/host/device.h (seam method if needed), tests/gpu/ (a new scheduler graph-replay test file), docs/decisions/0002-kernel-architecture.md (D7 remedy (a) adopted), tools/budgets.toml (B-19/B-20 only if measured with headroom).
Out of scope: main-pool compaction (remedy (b): written by the lead only if B-19 still fails after this); megakernel (spike C).
Reference: CUDA C++ Programming Guide, "CUDA Graphs" (stream capture, graph update, conditional or fixed-length bodies); Cycles integrator/path_trace_work_gpu.cpp for the step structure.
Do:
1. Launch every stage with a fixed grid sized from the pool capacity (D4 makes this host-only: kernels read their counts from the step table), and capture a step's launch sequence as a CUDA graph replayed per step; keep the refill and the pending-generation check between steps (cancellation latency stays one step); CPU unchanged.
2. Results unchanged: the GPU film and counters bit-identical to before on the gate's agreement and integrator checks (determinism check, 107).
3. Re-measure b1m and b5m at 1080p, 1 spp, 64 passes (093's method): idle %, partition + sort %, longest launch, pass ms, kernel time per sample vs Cycles' 7.37 / 10.39 ms (093's re-measure); state the outcome: B-19 passes -> record; fails -> say so (the lead decides on remedy (b) and spike C, 093 Do 5); B-20 with the per-kernel breakdown.
Acceptance: `pwsh tools/check.ps1 gate` passes; the PR body holds the before/after table and the outcome.
Look at: out/evidence/120/timeline_b1m.png.
Canary: tasks/120.canary.patch must turn red: astroray_tests_gpu with the new graph-replay case (the patch replays a stale graph after a step-table change).
Budget: no gate time added.
Depends on: 119.
Size: M (about 300 lines); stop and report at 2x.
Progress:
