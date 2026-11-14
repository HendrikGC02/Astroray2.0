# 086 Progressive batching, interaction and the heat-map display modes
Goal: the viewport presents every pass and has only Beauty -> after interaction ends the render thread runs a growing number of passes per displayed update on Cycles' display schedule (values copied into 0009 B-34), interaction follows 0007's single definition, and the Sample count and Bounce count modes draw outside the view transform on the route probe P4 picks.
Read first: 0007 d50 (progressive batching), d49 (interaction definition), d30 (publish rates), d40 (display modes, P4), 0009 B-34 and B-37, R30, R57, 0007 V23, V10; 0000 O22.
Scope: src/host/session/batching.{h,cpp}, src/host/session/session.cpp, docs/decisions/0009-build-test-process.md (B-34 row: the schedule values), tools/budgets.toml (B-34 text), blender/astroray/viewport.py, blender/astroray/heatmap_draw.py, blender/astroray/properties.py (`scene.astroray.display_mode` with Beauty, Sample count, Bounce count), tests/cpu/test_batching.cpp, tests/blender/gui_batching.py.
Out of scope: the resolution controller (M2); other display modes (M8); DLSS (M8).
Reference: Cycles `intern/cycles/integrator/render_scheduler.cpp` (Apache-2.0): the viewport display-interval schedule (copy the values and their growth rule with attribution).
Do:
1. Read render_scheduler.cpp, copy its viewport display-interval schedule into batching.cpp with the licence notice, and write the values into 0009 B-34's limit text (and budgets.toml in the same diff, keeping lint's table comparison green).
2. Interaction (0007 d49): a commit, or a `set_view` whose hash changed, within B-37's interaction window (250 ms); scale changes never count. During interaction each displayed update is one pass and frames publish at most 60/s; afterwards passes per update grow per the schedule and frames publish at most 10/s (B-34).
3. Display modes: `ar_session_set_display_mode` switches between Beauty (film_present, RGBA16F) and the heat maps (film_heatmap, RGBA8 sRGB, R57). Probe P4 first: draw the RGBA8 frame in `view_draw` without `bind_display_space_shader` and check whether Blender still applies the view transform (screenshot of known viridis patches); if it does, draw from a `SpaceView3D` POST_PIXEL handler instead. Record the chosen route in Progress and in a one-line comment in heatmap_draw.py.
4. Tests: test_batching.cpp (fast): with a synthetic clock, one pass per update during interaction, growing counts afterwards matching the copied schedule, interaction ends 250 ms after the last event, scale changes ignored; gui_batching.py (GUI): frame log during and after a scripted orbit on a small scene shows one pass per displayed update during interaction and growth after, publish rates within B-34 (0007 V23 on a small scene; S2 in 096); a screenshot of viridis patches under AgX matches exact sRGB +-1/255 (0007 V10).
Acceptance: `pwsh tools/check.ps1 fast` passes with `batching/*`; gui_batching.py passes through `Invoke-ArBlender -Gui`; 0009's B-34 row shows the copied values.
Look at: out/evidence/086/heatmap_samples.png and out/evidence/086/heatmap_bounces.png (viewport screenshots of both modes).
Canary: tasks/086.canary.patch must turn red: astroray_tests_cpu "batching/one pass during interaction", tests/blender/gui_batching.py "TestHeatmap.test_viridis_exact".
Budget: none on kernels.
Depends on: 085.
Size: M (about 350 lines); stop and report at 2x.
Progress:
