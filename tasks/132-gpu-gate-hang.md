# 132 astroray_tests_gpu --tier=gate hung once after "agree cancel (ii)"
Goal: during 131's first `check full`, the gate's `gpu gate tests` step hung for 28 minutes at 0 % CPU and 0 % GPU (11 threads) right after printing "agree cancel (ii) nofma vs cpu float ... pass", i.e. inside agree/fma detector's second run_gpu (the production agree_pure module, ar_agree_cancel); the same step had passed 20 minutes earlier and the diff under test was Python only -> reproduce, attribute and fix the hang, or show with numbers that it does not reproduce and add the instrumentation that will catch it next time.
Read first: tests/gpu/test_agree_pure.cpp (agree/fma detector, run_gpu, prod_module, nofma_module), src/device/cuda/module_loader.cpp (read_file with full sharing since 108, load_image), src/device/cuda/cuda_device.cpp (module loading, dev kernel reload poll), 108's PR body (`gh pr view 89`: a truncated cubin once hung cuModuleLoadData in the driver), 0002 D15 (failure containment, watchdogs).
Scope: tests/gpu/ (the agreement harness: timeouts, per-case timestamps), src/device/cuda/ (only if the cause is there), tools/check.ps1 (a per-step timeout for the GPU test executables if none exists, failing loudly with the last case printed); tests/gpu/test_cuda_session_trace.cpp (Do 4).
Out of scope: changing agreement thresholds or the cases themselves.
Reference: CUDA Driver API (cuModuleLoadData, synchronisation); 0002 D15.
Do:
1. Reproduce: run `astroray_tests_gpu --tier=gate` in a loop (at least 100 iterations, under the machine lock, while another worktree builds, as when it hung) and `--test-case=agree/fma detector` alone 500 times; record any hang with the last case and the loaded module paths and sizes.
2. If it reproduces, attribute it (a module file read while being written by a build in the same out/bin/kernels, a driver hang in cuModuleLoadData, a lost synchronisation) and fix the cause; if it does not, report the numbers.
3. Either way: the GPU test steps in check.ps1 get a timeout (derived from the step's measured time with a wide factor, stated) that fails loudly naming the last case started, so a hang costs minutes, not half an hour.
4. `cuda_session_trace/no cache dir disables the optix cache` compares listings of the user's shared default OptiX cache, which any other OptiX process (Cycles in Blender, other worktrees) writes, so it failed every run while a stale -wal sat there: make it check the same property without reading the shared database (for example the default location pointed at a private directory for that case, if OptiX honours it there; say what OptiX documents).
Acceptance: `pwsh tools/check.ps1 gate` passes; the PR body holds the reproduction numbers and the outcome.
Look at: none.
Canary: tasks/132.canary.patch must turn red: astroray_tests_gpu "device_failures/cubin layout check refuses truncated images", "device_failures/corrupt module image", "dev_reload/corrupted module keeps previous", "cuda_session_trace/no cache dir disables the optix cache"
Budget: none added.
Depends on: none.
Size: S; stop and report at 2x.
Progress:
- started: branch task/132-gpu-gate-hang from origin/main 4c3f048.
- reproduced: gate x36 (CPU gate tests alongside), hang at run 36 in dev_reload/corrupted module keeps previous (half cubin into cuModuleLoadData); fma detector alone x500, no hang. The original "after agree cancel (ii)" was stdout buffering: report() was the last fflush.
- fix: ELF-64 layout check before every cuModuleLoadData (gABI ch. 4); dev_reload and device_failures assert the new detail; per-case flushed start lines; check.ps1 GPU steps time-limited (gate 150 s, full 60 s, 10x measured).
- Do 4: the default-cache case points OPTIX_CACHE_PATH at a private directory and requires OptiX to report it. After the fix: gate x100, no hang (12.6-24.1 s).
