# 094 Measurement: device start-up and OptiX cold compile (B-18), sync and engine latencies (B-21, B-22, B-23, B-28, B-29, B-31), VRAM accounting
Goal: start-up, sync and per-pass overheads are unmeasured on product code -> measure CUDA context and module readiness warm and cold including the OptiX module and the motion pipeline variant compiled once, main-thread call times, edit-to-new-pass latency, light-structure build time, resolve traffic, statistics overhead, mesh and instance sync on S1 and S2, and the ledger against `cuMemGetInfo`, with outcomes stated.
Read first: 0009 B-18, B-21, B-22, B-23, B-28, B-29, B-31 (rows and bases), 0002 D11 (async creation, disk cache), D13 verification (ledger within 64 MiB), Risks 4; 0007 V3, V22; R47; docs/ROADMAP.md Measurements row "OptiX cold compile".
Scope: tools/measure_startup.ps1 (one-off; deleted when this task lands), tests/blender/gui_main_thread_timing.py, tools/budgets.toml and 0009's table (resets with headroom only).
Out of scope: first frame vs Cycles (095); viewport rate (096).
Reference: OptiX 9.1 Programming Guide (disk cache location and invalidation).
Do:
1. B-18: warm (cache present) and cold (empty cache directory passed in `cache_dir` and the driver's compute cache cleared through `CUDA_CACHE_PATH` pointed at an empty folder) time from `ar_scene_create` to init-thread completion, 10 runs each; include `create_motion_variant()` (041) once to time the motion pipeline's cold compile (R47, 0002 Risk 4). Each run uses a fresh process (fresh CUDA context), and the timed span ends after the first `optixLaunch` completes, since OptiX may compile lazily at pipeline or launch time (041's 48 ms module creation is not the whole cost).
2. B-21 / 0007 V22: tests/blender/gui_main_thread_timing.py instruments every ABI call the addon makes on the main thread (first scene and session create, close, edits, display acquire) on b5m; report max per function against 5 ms, host-data copies excluded.
3. B-22: time from an image-affecting edit commit to the start of the new pass (log timestamps), sync and rebuild excluded.
4. B-28: resolve and sample_add traffic share of pass time at 1080p and 4K (Nsight or per-kernel event timing). B-29: kernel time per 1080p sample with every statistic on vs off.
5. B-31 / 0007 V3: full mesh read, positions-only update, instance loop and a plain move on S1 and S2, cold and warm, in Blender.
6. 0002 D13: after loading b5m, the drop in `cuMemGetInfo` free memory vs the ledger total, within 64 MiB.
7. B-23: light_builder (067) time for 10,000 lamps and for 1 M emissive triangles (a b1m variant with an emissive material), counted inside first frame, against 50 ms per 10k lights.
8. Result (PR body `Result:` with commands and numbers). Outcomes: B-18 cold over 1.5 s -> per the roadmap, split modules or cache compiled pipelines before M1b (lead writes the task) and compile the motion variant lazily on first use; B-21 breach -> the offending call moves to a worker (follow-up task); B-31 breach -> 0007 d35's remedies are considered with 095's numbers; ledger gap over 64 MiB -> find the untracked allocation (follow-up task); B-23 breach -> the light-structure build is parallelised on the scene workers (follow-up task); rows within limits with headroom -> reset from the measurement.
Acceptance: the PR body holds every number and command; budget files change consistently (lint green); `pwsh tools/check.ps1 gate` passes.
Look at: none.
Canary: tasks/094.canary.patch must turn red: tests/blender/gui_main_thread_timing.py "TestMainThreadTiming.test_no_call_exceeds_b21" "TestMainThreadTiming.test_every_call_the_task_names_was_made_on_the_main_thread"
Budget: resets PROVISIONAL rows that pass with headroom.
Depends on: 089, 085.
Size: M (scripts plus result); stop and report at 2x.
Progress: started
- B-18, B-22, B-23, B-28, B-31, D13 measured with tools/measure_startup.ps1 (commands and numbers in the PR Result); the script is deleted in the last commit.
- B-21 case: KNOWN_BREACHES names ar_scene_destroy and ar_context_create (task 124); the case is green and fails if any other call is over 5 ms or a listed one drops under.
- Canary: coverage case and B-21 case, one patch (viewport.py drops ar_display_release; native.py slows ar_session_close to 10 ms).
- Reset: B-18 and B-28 (budgets.toml, 0009 table). Unchanged, numbers reported: B-21, B-22, B-23, B-29, B-31.
