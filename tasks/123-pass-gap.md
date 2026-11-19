# 123 GPU idle between passes (session host work after film_resolve)
Goal: after 120 the largest single GPU idle on B-1M is between passes, about 1.7 ms from k_film_resolve to the next pass's k_film_build_active (19.8 % idle per pass in total), host work in the session -> find what the session does there, move what can be stream-ordered or overlapped off the critical path, and re-measure B-19's idle share.
Read first: 0002 D7 (remedy (a) as adopted by 120), 0007 d12 (epochs, pass boundaries), d29 (display ring); 120's PR body (gaps.py method, out/evidence/120/); src/host/session/session.cpp (pass loop, begin_epoch, latch, resolve, progress), src/host/scheduler.cpp (pass start).
Scope: src/host/session/session.{h,cpp}, src/host/scheduler.{h,cpp} (pass start only), src/device/cuda/cuda_device.{h,cpp} (if a transfer at the boundary must become async), tests/cpu/test_session*.cpp or tests/gpu/ (a test for any reordering that could change results), docs/decisions/0002-kernel-architecture.md (D7 note).
Out of scope: the display ring's pinned slots and copy stream (0007 d29; 096 depends on them; a separate task if the gap is the display download), main-pool compaction (remedy (b)).
Reference: CUDA C++ Programming Guide, streams and events (asynchronous execution).
Do:
1. Attribute the gap: nsys with NVTX ranges (or host timestamps) around each session step between resolve and the next pass; table of host work per pass with ms.
2. Remove or overlap what is not needed on the critical path (waits that order nothing, per-pass host uploads that can be enqueued, progress bookkeeping under a lock the GPU waits on); film and counters bit-identical (determinism check, 107).
3. Re-measure b1m and b5m (120's method): idle %, pass ms, B-19 share; PR body Result: with commands, and say what the remaining idle is.
Acceptance: `pwsh tools/check.ps1 gate` passes; the PR body holds the before/after table.
Look at: none.
Canary: tasks/123.canary.patch must turn red: astroray_tests_gpu "cuda_transfers/memset is stream-ordered without a wait"
Budget: no gate time added.
Depends on: 120.
Size: S (about 150 lines); stop and report at 2x.
Progress:
- started
- Gap attributed (nsys --trace=cuda + host timestamps, B-1M): 1.75 ms = invalid-plane readback: fresh 8 MiB vector 0.75, pageable download 0.6, host sum 0.3.
- Kept the vector between passes, dropped the second bind_globals, CUDA memset stream-ordered (test + canary): gap 0.80 ms; idle B-1M 20.0 -> 13.6 %, B-5M 17.3 -> 11.7 %.
- Remaining gap needs a device-side invalid total (kernel) or pinned memory through the Device seam: out of scope.
