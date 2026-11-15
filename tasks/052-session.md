# 052 Sessions: render thread, pass loop, generation latch and epochs
Goal: scenes cannot be rendered -> `ar_session_create` starts a session whose render thread runs passes (scheduler, resolve, present), latches pending views and finished commits into generations at pass boundaries, starts epochs on image-affecting changes and cancels between launches.
Read first: 0007 d5 (session functions), d23 (threads), d25 (generations coalesced at pass boundaries, interrupt rule), d27 (cancellation), 0004 D25 (epoch reset rules); docs/decisions/0007-appendix-abi.md (`ar_session_desc`).
Scope: src/host/session/session.{h,cpp}, src/host/session/latch.{h,cpp}, src/capi/session_api.cpp, src/capi/unimplemented.cpp, tests/cpu/test_session.cpp, tests/abi/test_session_lifecycle.py.
Out of scope: progress, report polling, display acquire, pass readback, counters and FITS (104); progressive batching (086); resolution controller and OIDN (M2); camera ray generation and the active-pixel list (054, 101: until then a session renders zero work and resolves empty passes).
Reference: none.
Do:
1. `ar_session_create` returns at once in INITIALIZING; allocates film (047), pools (capacity rule of 045) and the display ring (051) through the ledger; starts the render thread through task 015's helper. `start`, `pause`, `resume`, `cancel` (stop at the next launch boundary), `close` (non-blocking: cancel, free session device memory, state CLOSED), `destroy` (blocks only before CLOSED), `wait(timeout_ms)` (the named wait), `set_view` and `set_display_mode` (record pending state). Every non-query entry point calls `AR_RECORD` (033).
2. Pass loop: per pass the scheduler (045) runs to drain, then film_resolve and statistics (049), then film_present (Beauty) or film_heatmap (display mode), then publish to the ring with the current generation.
3. Latch (0007 d25): at each pass boundary, all pending views and every commit whose jobs finished become one new generation; an image-affecting change starts a new epoch (film, statistics and sample indices reset; report FILM entries cleared); a change that starts an epoch interrupts the pass in flight at the next launch boundary only if the current generation has already presented a frame, and the interrupted pass is not resolved; writes carry the epoch and stale ones are dropped.
4. Tests: test_session.cpp (fast and gate, host classes): pending views coalesce into one generation per pass; an epoch change resets the film; cancel stops at the next launch; two sessions on one scene run concurrently; test_session_lifecycle.py: create, start, wait, pause, resume, close, destroy on the CPU device through `_abi.py`; every call from the test's main thread returns within B-21 (printed).
5. From 047: before every film resolve launch call `Film::check_resolve_fits(k)` (src/host/film/film_layout.h), which throws when active pixels x k exceed the slot capacity, and register `KernelId::FILM_RESOLVE` -> `cpu_film_resolve` on the CPU device (src/device/cpu/cpu_device.cpp joins this task's Scope for that line).
Acceptance: `pwsh tools/check.ps1 gate` passes with `session/*` cases; `<python> -m unittest discover -s tests/abi -p "test_session_lifecycle.py"` passes and prints each call's time and the final state CLOSED.
Look at: none.
Canary: tasks/052.canary.patch must turn red: astroray_tests_cpu "session/epoch change resets film", "session/views coalesce per pass".
Budget: host only.
Depends on: 045, 047, 051, 032, 033.
Size: M (about 350 lines); stop and report at 2x.
Progress:
