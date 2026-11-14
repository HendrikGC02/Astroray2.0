# 045 Host scheduler loop, light-path routing test and work counters
Goal: stages cannot be driven -> one host loop written against the two-method device interface runs steps (partition, step-table read, sort, refill, launches in Stage order) until a pass drains, refills by Cycles' quarter rule from a work counter over camera then light samples, checks cancellation between launches, counts work, and a stub light path proves light-path routing.
Read first: 0002 D5 (capacities, POOL_CAPACITY), D6-D7 (step, refill rule, pass end, launch duration), D8 (light paths in the scheduler, stub generate_light test), D12 (determinism); 0009 d18 (work counters B-16); 0004 D29 (per-pass counters); R52.
Scope: src/host/scheduler.{h,cpp}, src/host/counters.{h,cpp}, tests/cpu/test_scheduler.cpp, tests/cpu/stub_stages.{h,cpp} (test-only stage functions), tests/budgets/counters.toml (empty baseline table, filled by 076).
Out of scope: real generate_camera (054) and shading stages; the session thread and epochs (052); CUDA graphs (a D7 remedy, only after task 093 measures).
Reference: Cycles `integrator/path_trace_work_gpu.cpp` (Apache-2.0) for the refill rule.
Do:
1. `Scheduler::run_pass(device, work)`: loop of steps: launch queue_build (3 kernels), `read_step_table` (pinned, one event wait), sort when 044's thresholds hold, refill (`generate_camera` then `generate_light` on the refill count when active < capacity/4 and work remains; work = camera samples followed by light samples, light samples 0 when the scene has no casters), then every stage with a non-zero count once in Stage order. The pass ends when the work counter is exhausted and only the two TERMINATED segments are non-empty.
2. Pool capacity = min(work in the pass, 2^21, the path-pool row the ledger allows); CPU default 2^16; the value is raised as `POOL_CAPACITY` (INFO) in the session report once per session.
3. Between launches, check a cancel flag and a pending-epoch flag (set by 052); on either, stop at the next launch boundary.
4. Counters per pass (u64): rays traced closest and shadow, steps, launches, items per stage, active paths, refills, state and shadow-item bytes moved (sizes from 025), lamp crossings and shadow rays (filled by later stages through counter slots in KernelGlobals); exposed to 104's `ar_session_read_counters` and to the gate's B-16 check (076).
5. Debug flag `AR_DEBUG_PERMUTE` (runtime, test builds): before each launch, randomly permutes item indices within each segment (fixed seed).
6. Tests (fast, CPU device, stub stages registered in the test): a stub generate_camera and a stub generate_light whose paths terminate at their first hit; every light path terminates and the pass ends (0002 D8 verification, scheduler part); pool forced to 2^10 still drains all work; with AR_DEBUG_PERMUTE the final stub state is bit-identical to the unpermuted run and shadow items are equal as a multiset (0002 D6-D7 verification with stubs; repeated with real stages in 076); cancel stops at the next launch.
Acceptance: `pwsh tools/check.ps1 fast` passes with `scheduler/*` cases; the test prints steps per pass and counters for the stub run.
Look at: none.
Canary: tasks/045.canary.patch must turn red: astroray_tests_cpu "scheduler/stub light paths terminate", "scheduler/permute invariance".
Budget: host only.
Depends on: 043, 044, 026.
Size: M (about 450 lines); stop and report at 2x.
Progress:
