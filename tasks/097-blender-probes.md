# 097 Blender probes P2, P3 (object mode), P5 and the colour check V9
Goal: three Blender behaviours the design depends on are unverified and colour management is unchecked -> probes record whether Blender 5.3 calls `view_pause`/`view_resume` on F12 (P2), whether object-mode reads and `modal_operators` behave as 0007 d22 assumes (P3), how much VRAM Blender and the desktop use (P5, sets the reserve R), and a scene-linear chart shows Blender's view transform applies exactly once (V9).
Read first: 0007 Risks P2, P3, P5 and d14 (reserve R = 1.5 GiB PROVISIONAL), d22 (topology cache relies on `modal_operators`), d28 (pause on F12), d32 (no engine tonemapping), V9; docs/ROADMAP.md Measurements row P2-P5 (P4 is done in task 086; P3 edit mode is M3).
Scope: tests/blender/probe_p2_pause.py, tests/blender/probe_p3_modal.py, tests/blender/probe_p5_vram.py, tests/blender/gui_colour_chart.py, blender/astroray (pause fallback only if P2 fails), docs/decisions/0007-blender-integration.md (R value in d14 only if P5 says so), tools/budgets.toml (none expected).
Out of scope: P1 custom nodes and P3 edit mode (M3); P6 grids (spike G).
Reference: none.
Do:
1. P2 (GUI): two viewports in Rendered mode, start F12; log whether Blender calls `view_pause` and later `view_resume` on the viewport engines. Outcome: called -> record; not called -> the addon pauses its weakly-referenced viewport sessions from `update()` and resumes them after `render()` (implement in this task, test with the same script).
2. P3 (GUI, object mode): during a modal transform (`G` key operator started by script), is `context.window.modal_operators` populated inside `view_update`, and does a positions-only update keep triangles equal to `loop_triangles`? Outcome: yes -> record; no -> the topology fast path (b) is disabled and falls back to full reads (edit 081's decision function, with a test).
3. P5: free VRAM (`cuMemGetInfo` through the engine) with Blender idle, with Blender showing a Solid viewport of b5m, and with EEVEE Material Preview; R = the largest Blender usage plus 25% margin. Outcome: within 1.5 GiB -> record; above -> write the measured R into 0007 d14 and the CudaDevice constant (028) in this diff.
4. V9 (GUI): a scene-linear emission chart rendered by Astroray in the viewport vs the same values in the Image Editor under Standard, AgX, exposure +1, a gamma change and a look; per-patch difference <= 2/255 (screenshot pixels read with NumPy, not by eye).
5. Result (PR body `Result:` with each probe's command and outcome).
Acceptance: each probe script runs through `Invoke-ArBlender -Gui` and prints its outcome; V9 passes; any fallback implemented has its test passing.
Look at: out/evidence/097/colour_chart.png.
Canary: none unless a fallback is implemented; then tasks/097.canary.patch must turn red: tests/blender/probe_p2_pause.py "TestPause.test_viewports_paused_during_f12" (or the P3 test).
Budget: none.
Depends on: 086, 089.
Size: M (probe scripts plus possible fallbacks); stop and report at 2x.
Progress:
- started (agent)
