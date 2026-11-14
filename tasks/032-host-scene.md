# 032 Host scene: creation, edit transactions, staged arena, workers and generations
Goal: scenes cannot be created or edited -> `ar_scene_create` returns at once with an init thread and a worker pool, edits open, stage, hand items to workers and commit to a generation that becomes active only when all its jobs succeed, and a failed generation is dropped with the previous one kept and reported.
Read first: 0007 d4-d6 (conventions, function list, zero-copy staging), d9 (validation and outcomes), d12 (edit to work map), d23-d25 (threads, pipelined sync, generations), d41 (`ar_edit_set_astro` stored, no effect); 0001 D2.3 (staging), D2.11 (GENERATION_REJECTED); 0002 D15 (init failure row).
Scope: src/host/scene/scene.{h,cpp}, src/host/scene/edit.{h,cpp}, src/host/scene/arena.{h,cpp}, src/host/scene/generation.{h,cpp}, src/capi/scene_api.cpp, src/capi/unimplemented.cpp (remove implemented entries), tests/cpu/test_scene_edits.cpp, tests/abi/test_scene_api.py.
Out of scope: mesh, instance, image, material, light conversion jobs (035, 036, 060, 058, 067); sessions (052, 104); `ar_scene_trim` and spill (later milestones; returns AR_ERR_UNSUPPORTED); the recorder (033).
Reference: none.
Do:
1. `ar_scene_create`: creates the scene on the context's device, starts the init thread (CPU: no work; CUDA: context and module loading through 028; OptiX added by 041) and a worker pool of `worker_threads` or min(8, logical cores / 2), all through task 015's engine-thread helper; returns at once. An init failure is stored and re-raised at the first upload and on the scene's sessions (0002 D15).
2. Staged arena: 64 MiB chunks, pre-faulted by a worker one chunk ahead; pointers stay valid until the item's commit; chunks recycle after their jobs finish.
3. Edit API: `ar_scene_edit_begin` (one open edit per scene, else AR_ERR_INVALID_ARGUMENT), `ar_edit_reset` (first call only, replaces the whole scene), `ar_edit_item_commit` (hands that key's job to the workers now), `ar_edit_remove`, `ar_edit_set_settings`, `ar_edit_set_camera`, `ar_edit_set_astro` (validates finite transform, mass > 0, |spin| < 1, meters_per_rg > 0; stores; no effect until M10), `ar_edit_commit` (O(1) checks: enum ranges, known keys, staged counts filled; returns the pending generation number), `ar_edit_abort`. Setters for other kinds store their descriptors for later tasks' jobs.
4. Generations: a commit's generation becomes ready when every job succeeds; replaced objects are freed only then; a job failure drops the whole generation, frees its new objects, keeps the previous generation active, records `failed_generation`, and raises `GENERATION_REJECTED` (subject = key, detail = failed check, value[0] = generation) in a scene-level report that sessions merge (104).
5. Tests: test_scene_edits.cpp (fast and gate, host classes directly): numbering, a failing test job rejects its generation and keeps the previous, two edits pipeline while workers run, abort discards staged data, astro validation; test_scene_api.py drives create, edit, set_astro, commit, destroy through `_abi.py` on the CPU device.
Acceptance: `pwsh tools/check.ps1 gate` passes; `out/bin/astroray_tests_cpu.exe --tier=fast -tc="scene/*"` passes at least 6 cases; `ar_scene_create` returns within 0009 B-21 (5 ms) in test_scene_api's timing print.
Look at: none.
Canary: tasks/032.canary.patch must turn red: astroray_tests_cpu "scene/failed job keeps previous generation", "scene/one open edit".
Budget: none.
Depends on: 031, 026, 016.
Size: M (about 500 lines); stop and report at 2x.
Progress:
