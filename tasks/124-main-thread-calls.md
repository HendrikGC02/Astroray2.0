# 124 Main-thread breaches of B-21 (scene destroy, context create) and the addon's OptiX disk cache
Goal: 094 measured two engine calls on Blender's main thread over B-21's 5 ms on b5m: `ar_scene_destroy` 111-115 ms (on close) and `ar_context_create` 7.2-7.7 ms (first create), and found the addon passes `cache_dir = None` (render_final.py) so every Blender start pays the cold OptiX module (~85 ms) -> both calls return within 5 ms on the main thread with the work moved to a worker as 0007 D23 prescribes, and the OptiX disk cache is on in Blender.
Read first: 0007 D23 (main-thread rule), d45 (OptiX cache directory), 0001 D2.5 (handles, destruction, faults); 0009 B-21, B-18; 094's PR body (B-21 table, B-18 cold and warm); tests/blender/gui_main_thread_timing.py (KNOWN_BREACHES, task 124).
Scope: src/capi/ (context and scene create and destroy entry points), src/host/scene/ and the context code under src/host/ (deferred destruction on a worker, asynchronous context init), blender/astroray/render_final.py and blender/astroray/viewport.py (cache_dir, where the addon creates and frees), tests/blender/gui_main_thread_timing.py (remove the two KNOWN_BREACHES entries), tests/abi/ or tests/cpu/ (destruction ordering and use-after-destroy tests).
Out of scope: other B-21 calls (none over the limit); B-31 sync remedies (a separate task after 095).
Reference: 0007 D23; CUDA Driver API (context destruction is synchronous, which is why it leaves the main thread).
Do:
1. `ar_scene_destroy` hands the scene to a worker that frees it; the handle is invalid at return, later calls on it fail loudly with AR_INVALID_HANDLE, and `ar_context_destroy` or process exit waits for pending frees (no leak, no use after free; fault containment unchanged).
2. `ar_context_create` returns after the cheap part; device readiness is reported as today (asynchronous init, 0001); whatever it waits on now (DLL load, driver probe, log setup) is measured and either moved off the main thread or justified with numbers.
3. The addon sets the OptiX cache directory per 0007 d45 (name the path: a per-user cache directory Blender provides or the extension's user directory); measure the second Blender start's OptiX module time (B-18 warm should then apply in Blender).
4. Remove the two entries from KNOWN_BREACHES; gui_main_thread_timing.py passes with B-21 unchanged.
Acceptance: `pwsh tools/check.ps1 gate` passes; `Invoke-ArBlender -Gui -Script tests/blender/gui_main_thread_timing.py` passes with no known breaches; the PR body holds the before and after call times.
Look at: none.
Canary: tasks/124.canary.patch must turn red: the destruction tests you add (name them in land format) and tests/blender/gui_main_thread_timing.py "TestMainThreadTiming.test_no_call_exceeds_b21" (the patch frees the scene synchronously again).
Budget: none added.
Depends on: 094.
Size: M (about 250 lines); stop and report at 2x.
Progress:
- started
