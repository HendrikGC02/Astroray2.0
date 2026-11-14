# 084 F12 rendering and the gate's Blender smoke
Goal: F12 renders nothing -> `update()` resets and fully syncs the view layer, `render()` drives a FINAL session to completion with progress, previews and cancellation and writes Combined into the RenderResult, and `check gate` runs the Blender smoke: install the staged zip, render A1's .blend at 64x64 through the addon, check it analytically, and compare the addon's recording with the builder's.
Read first: 0007 d23 (F12 job thread), d27 (cancellation, previews), d28 (scene and session per render), d48 (view layers), 0008 d13a (readback), 0009 d17a (launcher, gate smoke, exporter equivalence), R59 (no scene-state readback); 0001 D1.4 (recordings).
Scope: blender/astroray/engine.py, blender/astroray/render_final.py, scenes/a1_furnace/build.py (`--blend <path>`), scenes/lib/ar.py (JSON scene description), scenes/lib/blend_writer.py (created here; 089 and 090 extend it), tests/blender/smoke_a1.py, tools/check.ps1 (gate Blender step), tools/exporter_equivalence.py.
Out of scope: viewport (085); passes beyond Combined (M1b: Depth, Normal, Alpha); denoising (M1b); motion blur (M8).
Reference: Cycles `intern/cycles/blender/session.cpp` (Apache-2.0) for update/render structure.
Do:
1. `update(data, depsgraph)`: one F12 scene and session per job (CPU or CUDA by the scene's device setting), `ar_edit_reset` then full sync of that view layer through 081-083, commit.
2. `render(depsgraph)`: start the session; loop `ar_session_wait` with B-37's timeout; `update_stats`/`update_progress` from `ar_progress`; post a preview with `update_result` every B-37 interval; `test_break()` -> `ar_session_cancel` and return the partial result; at the end copy Combined (`read_pass`, MEAN, rows already bottom-up) into the RenderResult's Combined pass; print every report entry through `self.report` (ERROR entries fail the render with their text).
3. Recording: with `ASTRORAY_RECORD=<path>` set, the context records (033); the smoke uses it.
4. One .blend writer for every builder: scenes/lib/ar.py also writes a JSON description of what it recorded (meshes, constant Principled inputs, lamps, world colour, camera, settings), and scenes/lib/blend_writer.py, run inside headless Blender through Invoke-ArBlender, builds the .blend from it; a1_furnace gains `--blend <path>` (Principled with Specular IOR Level 0 and Roughness 0 on a UV sphere; world Background colour), so the addon and the builder describe one scene.
5. smoke_a1.py (Invoke-ArBlender): installs the staged zip, opens the .blend, renders 64x64 at the analytic harness's spp with the addon on the CPU device, saves the Combined pass as a FITS-compatible float dump the harness reads, and passes A1's truth; the addon's recording and the builder's recording are both rendered with `astroray_cli dump` on CPU at a fixed seed, and tools/exporter_equivalence.py compares the deterministic AOVs (position, normal, material id, UV) within float tolerance (0009 d17a exporter equivalence, done through film output because R59 forbids a scene-state dump).
6. check.ps1 `gate` adds the smoke after the GPU tests (one Blender start; time reported against B-09).
Acceptance: `pwsh tools/check.ps1 gate` passes with the smoke; pressing F12 on the A1 .blend in an interactive Blender shows the furnace image (the lead's look).
Look at: out/evidence/084/a1_f12.png (Blender's render result saved as PNG).
Canary: tasks/084.canary.patch must turn red: tests/blender/smoke_a1.py "TestSmoke.test_a1_analytic", `tools/exporter_equivalence.py` (the patch drops corner normals in mesh sync).
Budget: gate time grows by one Blender start plus the render; report it (0009 Risks: Blender start-up per process).
Depends on: 083, 104, 073, 070.
Size: M (about 400 lines); stop and report at 2x.
Progress:
