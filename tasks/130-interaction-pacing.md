# 130 Interaction pass pacing: the publish gap is waited before each pass (V12 latency, B-14 headroom)
Goal: 096 found session.cpp sleeps kInteractionPublishGapS (1/60 s) before each interaction pass, so an interaction cycle is about 16.7 ms of waiting plus the pass: B-1M holds 32/s only thanks to that serial wait's arithmetic, A1 (6.3 ms pass) gets 40/s instead of the B-34 cap of 60, and tests/blender/gui_latency.py's view-change latency and orbit-gap cases are red on every scene (b1m 19.9-39.4 ms against a ~33 ms bound); F12 returns 180-430 ms after the engine pauses because ar_session_wait does not wake on PAUSED (100 ms timeout) -> pace publishing, not rendering: passes run back to back and the publish gap only delays presenting a finished frame; ar_session_wait wakes on PAUSED.
Read first: 0007 d49-d50 (interaction, progressive batching, owner decision 22, ruling 30), d12 (epochs, latch), V12, V23; 0009 B-14, B-32, B-34, B-37; 096's PR body (rates, latencies, the cause); src/host/session/session.cpp (interaction loop around the publish-gap wait), src/host/session/batching.{h,cpp}; tests/blender/gui_latency.py (the two cases skipped with this task's id).
Scope: src/host/session/session.{h,cpp}, src/host/session/batching.{h,cpp}, src/capi/session_api.cpp (ar_session_wait wake-up), tests/cpu/ (batching and pacing tests), tests/blender/gui_latency.py (remove the skips; bounds unchanged), docs/decisions/0007-blender-integration.md (d50 wording only if the pacing statement changes); cli/render.cpp and tests/abi/replay_driver.cpp (their ar_session_wait loops handle PAUSED; lead ruling 2026-11-19).
Out of scope: the display ring's synchronous download (0007 d29's pinned slots and copy stream); the resolution controller.
Reference: 0007 d50 and ruling 30 as written; Cycles' viewport pacing is not a target.
Do:
1. During interaction, start the next pass as soon as the previous one ends; a finished frame waits for the publish gap only if it would exceed B-34's 60/s; a newer view latched meanwhile still replaces older work at the next launch boundary (no stale frames).
2. ar_session_wait wakes on PAUSED and on CANCELLED as on other state changes (B-37); measure F12 return after the break.
3. Tests: a CPU test of the pacing rule (passes back to back, publishes capped at 60/s, a view change latched at the next boundary); gui_latency.py's view-change and orbit-gap cases un-skipped and green on a1_furnace (check full) and on b1m, b5m (run by hand, in the Result).
4. Re-measure with 096's harness (tools/viewport_rate_probe.py via its runner): B-14 on b1m, b5m, b_tx, a1; V12 latencies; V23 schedule unchanged.
Acceptance: `pwsh tools/check.ps1 gate` passes; gui_latency.py passes on a1_furnace, b1m and b5m; the PR body holds before/after tables with commands.
Look at: none.
Canary: tasks/130.canary.patch must turn red: astroray_tests_cpu "batching/session passes run back to back during interaction", astroray_tests_cpu "batching/session a view change during the resolve presents no frame of the old view", astroray_tests_cpu "session/wait wakes on pause", tests/blender/gui_latency.py "TestLatency.test_view_change_latency_within_b32", tests/blender/gui_latency.py "TestLatency.test_orbit_frames_are_for_newer_views"
Budget: none added.
Depends on: 096.
Size: S (about 150 lines); stop and report at 2x.
Progress:
- started: branch task/130-interaction-pacing from b1b3b03 (096 on main); plan approved by the lead: publish-side cap, a waiting frame still presents, wait returns on PAUSED.
- lead ruling 82: V12's frame per pass yields to B-34's 60/s cap; orbit-gap bound = max(2 x pass p99, 1/60 s) + redraw + present (gui_latency orbit case). Lead approved: a due epoch change interrupts at the resolve's return (0007 d25), tested.
- done: passes back to back, frame waits out the gap (measured from present start); wait wakes on PAUSED; CLI and replay loops fail on PAUSED. Canary: 3 hunks, each red alone (CPU cases; a1 view case hunk 3, a1 orbit case hunk 2).
