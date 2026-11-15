# 026 Device interface, CpuDevice, thread pool and the VRAM ledger
Goal: there is no vendor seam and no allocation accounting -> the host `Device` interface exists with a working `CpuDevice` (memory, chunked stage launch on a per-session thread pool), and every allocation goes through a ledger with 0007's categories and per-scene and per-session totals.
Read first: 0001 D5.2 (Device seam), 0002 D4 (CPU stage loop), D7 (two-method device interface), D13 (accounting), 0007 d14 (ledger, categories, reserve rule, budget table), R19.
Scope: src/host/device.h, src/host/ledger.{h,cpp}, src/host/thread_pool.{h,cpp}, src/device/cpu/cpu_device.{h,cpp}, src/kernel_cpu/stage_cpu.h, tests/cpu/test_ledger.cpp, tests/cpu/test_cpu_device.cpp.
Out of scope: CudaDevice and the reserve rule's CUDA side (028); traversal (037); trim and spill (later milestones: the ledger fails only).
Reference: pbrt-v4 `src/pbrt/util/parallel.h` `ParallelFor` (Apache-2.0) as the chunked-loop pattern.
Do:
1. device.h: `class Device` (host C++, no CUDA types) with `allocate(LedgerCategory, bytes) -> DeviceBuffer`, `free`, `upload`, `download`, `memset`, `launch(Stage or KernelId, uint32_t count)`, `read_step_table(StepTable&)`, `synchronize()`, `kind()`. Dispatch is per launch, never per item. A `KernelId` enum lists the per-pass kernels of 0002 D8 (film and scheduler kernels) next to `Stage`.
2. ledger: categories = every row of 0007 d14's budget table plus scene categories geometry, acceleration, textures, volumes, tables; records (scene id, session id, category, bytes) per allocation; totals per scene, session and category; `may_allocate(need)` is a hook the device implements (CPU: always allowed, host memory; CUDA in 028); a refused allocation throws a typed error the capi layer turns into AR_ERR_OUT_OF_MEMORY and a `SESSION_FAILED` entry "needs <X> GiB for <category>, <Y> available".
3. thread_pool: owned by a session or scene (no process-global pool), N workers started through task 015's engine-thread helper, `parallel_for(count, chunk = 1024, fn)`.
4. CpuDevice: memory with `_aligned_malloc` (256 B), stage launch looks up a per-device table of CPU stage entry points (host function pointers are allowed in host code) registered by `AR_CPU_STAGE(name)` (stage_cpu.h) and runs `parallel_for` over the step table's segment.
5. Tests (fast): ledger totals after allocate/free sequences per scene and session; refused allocation produces the message text; `parallel_for` visits every index exactly once with 1 and 8 threads; a registered dummy test stage (test-only, in the test file) receives the right segment.
Acceptance: `pwsh tools/check.ps1 fast` passes; `out/bin/astroray_tests_cpu.exe --tier=fast -tc="ledger/*,cpu_device/*"` passes at least 5 cases.
Look at: none.
Canary: tasks/026.canary.patch must turn red: astroray_tests_cpu "ledger/totals per category", "cpu_device/parallel_for covers every item".
Budget: none.
Depends on: 025, 015.
Size: M (about 400 lines); stop and report at 2x.
Progress:
- Device::allocate/free are non-virtual (ledger applied in the base; devices supply may_allocate, alloc_memory, free_memory); allocate takes an Owner {scene, session}.
- Added Device::set_kernel_globals(buffer) (launch needs the one KernelGlobals pointer, 0002 D4); the step table is read from it.
- Refusal text "needs %.2f GiB for <category>, %.2f GiB available"; the capi mapping to AR_ERR_OUT_OF_MEMORY and SESSION_FAILED is outside Scope and not done.
- ThreadPool throws on a poisoned context instead of waiting for a dead worker; AR_CPU_STAGE has no static registry, CpuDevice::register_stage fills a per-device table.
