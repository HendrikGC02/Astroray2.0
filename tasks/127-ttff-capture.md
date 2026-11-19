# 127 V7a first frame with a GPU-visible screen capture, and the report's first-frame section
Goal: 095 found GDI capture (CopyFromScreen, PrintWindow) of Blender's window returns only the window background on this machine (Vulkan and OpenGL alike), so V7a, the first-frame gate (0007 d35), is unmeasured; it landed V7b and M-8 only -> a poller that sees the swapchain (DXGI Desktop Duplication or Windows Graphics Capture) measures V7a for Cycles and Astroray on S1, S2 and S3, first and second switch, and the report shows it against the 1.5x line.
Read first: 0007 d35 (V7a, V7b, remedies), ruling 21; 095's PR body (harness, `--source screen|gpu|none`, V7b tables, the capture finding); tests/blender/probe_ttff.py, tools/ttff_poller.ps1, tools/measure_ttff.ps1; tools/lib/blender.ps1 (Invoke-ArBlender).
Scope: tools/ttff_poller.ps1 or a replacement poller under tools/ (PowerShell with WinRT or Python ctypes against d3d11/dxgi, or a small native helper built by tools/build.ps1; no third-party download), tools/measure_ttff.ps1, tools/lib/blender.ps1 (a `-BlenderArgs` parameter for options before `--python`; then delete measure_ttff.ps1's duplicated launcher), tests/blender/probe_ttff.py, tools/report.py (first-frame section), tools/budgets.toml (B-15 only with headroom).
Out of scope: system settings (MPO, graphics drivers, display configuration); the d35 remedies themselves (task 128 for remedy 1).
Reference: Microsoft DXGI Desktop Duplication API (IDXGIOutputDuplication) or Windows.Graphics.Capture documentation; spike B's poller for the detection rule (first frame with engine pixels in the region, resolution <= 20 ms, own PID's window only).
Do:
1. A poller that captures only Blender's own window region through Desktop Duplication or Windows Graphics Capture at >= 100 Hz (say which and why), fails loudly when it sees no contrast, never returns a number it did not detect; prove it on a known GPU-drawn change (e.g. a solid-colour viewport switch) before using it for V7a.
2. `Invoke-ArBlender -BlenderArgs` (options before `--python`), used for Cycles' `--log cycles --log-level 4`; remove the duplicate launcher from measure_ttff.ps1.
3. V7a: 3 fresh-process runs per (scene S1 b1m, S2 b5m, S3 s3_textured; engine Cycles, Astroray; first and second switch) under the machine lock; per-run table; ratio to Cycles.
4. tools/report.py first-frame section from history lines (bars per scene and switch, the 1.5x line); out/evidence/127/ttff_s2.png.
5. Outcome per 0007 d35: every scene and switch <= 1.5x Cycles -> gate met, recorded; S2 or S3 above -> report which remedy applies next (remedy 1 is task 128; re-measure after it lands).
Acceptance: the PR body holds the V7a table with commands; `<python> tools/report.py` shows the first-frame section; check fast passes.
Look at: out/evidence/127/ttff_s2.png.
Canary: none: measurement only (no test added or changed; if the poller gets a self-test, it needs a canary).
Budget: none.
Depends on: 095.
Size: M (poller about 200 lines plus result); stop and report at 2x.
Progress:
