# 085 Viewport: view_update, view_draw and the striped RGBA16F present
Goal: the viewport shows nothing -> `view_update` syncs depsgraph deltas, `view_draw` hashes the view, acquires the newest frame and presents it through persistent storage-buffer stripes, a compute unpack and one persistent texture inside Blender's display-space shader, never showing a frame older than one already shown, and sessions close in two steps.
Read first: 0007 d25 (generation tags, no stale frames), d26 (redraw loop and timer), d29-d30 (present path, 16 MiB stripes), d32 (colour management), d49 (view hash, interaction), d23 (two-step destruction); docs/spikes/B-blender-end-to-end/RESULTS.md section 1 (present facts).
Scope: blender/astroray/viewport.py, blender/astroray/present.py, blender/astroray/shaders/unpack_half.glsl (or inline source), blender/astroray/engine.py, tests/blender/gui_present.py, tools/lib/blender.ps1 (`-Gui` switch: no `--background`).
Out of scope: the full 0007 V4 edit matrix and viewport record/replay (111); progressive batching growth and display modes (086); resolution controller and OIDN (M2); depth writing (later).
Reference: spike B `gui_present.py` (read with `git show spikes-final:spikes/B-blender-end-to-end/gui_present.py`) for the measured SSBO, compute and texture calls.
Do:
1. view_update: one scene and session per RenderEngine instance (VIEWPORT mode); first call does a full sync; later calls classify updates (080) and send deltas; commits never block.
2. view_draw: compute the view hash and call `ar_session_set_view` only when it changes; `ar_display_acquire(after = last shown)`; if a frame of generation >= the last presented one exists, update the persistent `GPUStorageBuf` stripes (<= 16 MiB each, reallocated only on size change, up to two sizes cached), dispatch a 16x16 compute shader with `unpackHalf2x16` into one persistent RGBA16F `GPUTexture`, `ar_display_release`; draw with `draw_texture_2d` between `bind_display_space_shader` and `unbind_display_space_shader` with ALPHA_PREMULT blending; then `tag_redraw()` while rendering or work is pending; a `bpy.app.timers` poll at B-37's period exists only while async work is pending after convergence.
3. Destruction: `__del__` calls `ar_session_close` and parks the handles; the timer destroys session then scene once progress reports CLOSED; addon unregister destroys directly.
4. gui_present.py (GUI Blender run, A1 .blend): Rendered shading presents a frame; a scripted object move sends a delta (no full resync) and the next presented frame has a newer generation and differs from the pre-edit frame (frame log, not by eye); closing the viewport reaches CLOSED and destroys session then scene.
5. From 084: in F12 the first `ar_session_wait` (100 ms timeout) seemed to return only when the render finished; an early break did not shorten it. Measure how long the first wait blocks after session start; a viewport draw loop cannot wait like that (0007 d23, B-21), so find and report the cause if it exceeds the timeout.
Acceptance: gui_present.py passes through `Invoke-ArBlender -Gui`; switching an interactive viewport to Rendered on the A1 .blend with an added area lamp and a textured plane shows an image within a second (the lead's look; timing is 095's).
Look at: out/evidence/085/viewport.png (screenshot of the rendered viewport).
Canary: tasks/085.canary.patch must turn red: tests/blender/gui_present.py "TestPresent.test_edit_changes_frame", "TestPresent.test_close_destroys".
Budget: view_draw time per present is measured in 096 (B-13).
Depends on: 084, 051.
Size: M (about 350 lines Python); stop and report at 2x.
Progress:
- started (085)
