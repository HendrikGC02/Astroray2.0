# 127 V7a first frame timed inside Blender's process, and the report's first-frame section
Goal: screen capture cannot see Blender's window on this machine (095, and 127's first attempt: GDI, Windows Graphics Capture and Desktop Duplication see only the window background), so the owner ruled V7a is measured inside the process (owner decision 35, ruling 83) -> the probe times V7a in the process for Cycles and Astroray on S1, S2 and S3, first and second switch, and the report shows it against the 1.5x line.
Read first: ruling 83, 0007 d35 (V7a, V7b, remedies), ruling 21; 095's PR body (`gh pr view 95`: the harness, `--source screen|gpu|none`, V7b tables, and that `read_color` in POST_PIXEL hung Blender under Cycles); branch task/127-ttff-capture (the parked first attempt: `Invoke-ArBlender -BlenderArgs`, the WGC poller, the capture proof); tools/ttff_probe.py (`--source gpu`, draw_cb, detect), tools/measure_ttff.ps1, tools/lib/blender.ps1.
Scope: tools/ttff_probe.py, tools/measure_ttff.ps1, tools/lib/blender.ps1 (`-BlenderArgs`), tools/ttff_poller.ps1 and tools/ttff_capture_proof.py (delete: no screen source remains), blender/astroray/ only for a timestamped first-engine-frame mark in the present if none exists, tools/report.py (first-frame section), tools/budgets.toml (B-15 only with headroom).
Out of scope: system settings (MPO, graphics drivers, display configuration); the d35 remedies themselves.
Reference: ruling 83; Blender Python API `bpy.types.SpaceView3D.draw_handler_add`, `gpu.state.active_framebuffer_get`, `GPUFrameBuffer.read_color`.
Do:
1. Continue on branch task/127-ttff-capture (its `-BlenderArgs` work stands; delete the WGC poller and the capture proof, and `--source screen`).
2. Readback detection: make the in-process read work for Astroray (a small centre patch, 3 channels, the cheapest format that detects); find out why it hung under Cycles (in POST_PIXEL; try POST_VIEW, a smaller read, a read deferred to the next handler call) with a bounded watchdog per attempt, and say which works.
3. Event detection, for an engine where the read stays unusable: the first POST_PIXEL redraw at or after the engine's first displayable frame (Astroray: the present's first engine frame; Cycles: the first `Rendered N samples` line of `--log cycles`, its timestamp put on the probe's clock and the conversion stated). Show both methods agree within one redraw on an engine where both work (Astroray at least), with the numbers.
4. V7a: 3 fresh-process runs per (scene S1 b1m, S2 b5m, S3 s3_textured; engine Cycles, Astroray; first and second switch) under the machine lock; per-run table naming the detection method per engine; ratio to Cycles.
5. tools/report.py first-frame section from history lines (bars per scene and switch, the 1.5x line); out/evidence/127/ttff_s2.png.
6. Outcome per 0007 d35: every scene and switch <= 1.5x Cycles -> gate met, recorded; S2 or S3 above -> report which remedy applies next.
Acceptance: the PR body holds the V7a table with commands and the method agreement numbers; `<python> tools/report.py` shows the first-frame section; check fast passes.
Look at: out/evidence/127/ttff_s2.png.
Canary: none: measurement only (no test added or changed).
Budget: none.
Depends on: 095.
Size: M; stop and report at 2x.
Progress:
- parked: Blender's window is never composed on this machine (WGC, GDI and DDA see only the window background; a D3D11 flip-model test window is captured fine); owner decision 35 answered it: V7a in the process (ruling 83).
- resumed after ruling 83: poller and capture proof deleted; probe stamps every POST_PIXEL redraw (tools/ttff_probe.py).
- read_color in POST_PIXEL: 3 channels corrupts the heap on Vulkan (UBYTE and FLOAT; the old hang/crash, Cycles too); 4 channels (UBYTE, 96x54 patch) is safe on both engines and its content flips at the engine's first frame.
- event method: Astroray first_frame mark; Cycles first `Rendered N samples` line put on the probe clock by `--log cycles,context` anchors (TTFF_ANCHOR).
- method agreement and V7a table: see the PR body.
