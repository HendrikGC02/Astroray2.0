# v1 lessons: Blender integration

Blender API behaviours and integration traps found while building v1's render-engine addon (Blender 5.1/5.2). Each bullet gives the rule and why.

## Design consequences

- v1's addon translated node trees by constant-folding them to per-material values, special-casing only image textures. Every per-point node downstream of a texture (ColorRamp, Mix, Math, Bump, Normal Map) silently became grey or a constant. The rewrite must evaluate node graphs per shading point (compile them to the engine's shader representation), and any node or socket it cannot represent must raise a visible warning.
- v1 shipped two binaries (an OpenMP-on dev module and a separately built addon module) and a staged addon copy, which lagged each other by weeks. One build artifact serves tests and Blender.
- v1 relied on process-global GPU state, which forced admission tokens, pause gates and load/exit drains between viewport and F12 renders. The rewrite forbids engine globals (CLAUDE.md rule 8); viewport and final render sessions are independent objects.

## Pixels, coordinates and data access

- `image.pixels` is bottom-up. A top-down region of interest on an unflipped array measured the ground plane instead of the sky and cost a day; an independent HDR decoder had the same flip, so two wrong instruments agreed. Draw every ROI on a saved PNG and look at it.
- `image.pixels[:]` followed by `.tolist()` cost about 195 ms per texture and ran on every final render. Read into a preallocated numpy buffer (`foreach_get`) and cache by a stable key.
- v1 docs state "Blender is Y-up and the engine matches". This is doubtful: Blender world space is Z-up (cameras look down local -Z with local +Y up). Verify against bpy before adopting any convention.
- Volumes: `bpy.types.VolumeGrid` exposes no voxels. Read `.vdb` grids with the openvdb Python module bundled in Blender 5.2 and hand arrays to the engine; v1 planned no engine-side VDB reader.
- Mesh export: fill numpy arrays for all triangles and pass them in one call. Per-object `to_mesh`/`calc_loop_triangles` on every sync was a major cost.

## Depsgraph and updates

- Camera moves in the viewport produce no depsgraph update. Hash the camera and view state on every draw; keep a second "substantive" hash so only optical changes (lens, sensor, clip, DOF) reset accumulation.
- Orbit and zoom change the region view matrix and reach the engine only through `view_draw`. Test tables that drive camera objects miss real navigation; a viewport default validated that way was reverted. Test with real interactive navigation before flipping a default.
- World node-tree edits arrive as NodeTree updates; match them to the world by original datablock.
- Classify updates into a change bitmask (geometry, transform, material, light, world, settings). Selection-only updates are idle. Any unrecognised update id falls back to a full sync. The update list is consumed once per edit; a stateful diff called twice loses changes.
- Incremental upload paths need a reconcile counterpart, and an edit must be verified against the pre-edit frame. Comparing two code paths (worker on vs off) passed while both showed the stale image.
- `bpy.context.evaluated_depsgraph_get()` returns a VIEWPORT-mode depsgraph (`is_render` False); final-render conversion must use the depsgraph Blender passes to `render()`.
- A `RenderEngine` subclass cannot be instantiated outside Blender's render loop. Keep scene conversion in plain functions so it can be unit-tested headlessly.
- Instances: group depsgraph instances by their data; singletons can be flattened. Every feature (emission, caustics, volumes) must either work on instances or exclude them visibly.

## Node trees and materials

- Measure node support by frequency-weighted use across real scenes, not raw socket count. In v1's corpus 340 of 527 sockets (64.5%) were dropped silently. A silently dropped socket is a failure; an approximation counts only if it warns.
- Never key a persistent cache on `id()` of a node from an inlined or temporary node tree. CPython reuses freed addresses, so textures aliased across materials and varied run to run. Key on stable names (material name plus node name) or content. "Wrong texture, varies per run" means id-keyed caching.
- Do not put placeholder `None` entries into lookup maps; `name in map` then lies.
- Engine caches must be invalidated whenever the engine's own state is cleared. v1 suspected (never runtime-verified) that a host-side texture cache survived a full engine reset, so a cache hit skipped reloading.
- Every Cycles setting the addon displays must be read by the engine or flagged. Filter Glossy was stored and never read; the engine also ignored the native device setting in favour of its own property.

## Viewport and display

- v1 made about five full-frame host copies per viewport present and created a new GPU buffer and texture each time. Keep one persistent display texture and copy once.
- The Blender UI must not run at the render's frame rate; decouple the render session from the draw callback.
- An off-thread viewport worker that did not present while orbiting was worse than none.
- Denoise runs only on settled frames, never inside the interactive refinement loop. An F12 render pauses the viewport session, as Cycles does.
- Measure latency through the real present path including Blender's GPU blit. An in-process harness showed p99 37.7 ms (184.8 ms with denoise); real present latency was never measured.
- Re-uploading the whole scene on every render call cost about 46 ms at 100k triangles. Cache the device scene per session and invalidate through one hook on every host mutation; never key a GPU cache on a raw pointer.

## Settings and UI

- Use Blender's native Cycles-shaped panels as the only control surface. v1 built a parallel custom UI and retired it.
- For a fair Cycles comparison: equal samples, denoising off, adaptive sampling off, `clamp_direct` and `clamp_indirect` = 0, smooth shading on caustic casters, same view transform. Cycles' bounce-cap behaviour is forced to match during parity runs.

## Packaging and install

- v1 built the addon zip from an allow-list of files; new modules were silently left out (install failed with "No module named ...") and data tables (energy-compensation LUTs) were missing from a release. Generate the package from the source tree and test that every module and data file is present.
- Register the staged addon in headless Blender before installing it anywhere, so a broken `register()` never reaches a profile.
- Blender's `--python` exits 0 on an uncaught traceback. A headless gate checks a PASS sentinel, a zero exit code, and the absence of a FAIL line.
- Smoke runs use an owned, disposable profile and restore every `BLENDER_USER_*` variable in `finally`.
- Clean-machine gate: a fresh profile installs the release zip through Blender's extension installer and renders one F12 frame, with no build toolchain present.
- Never hand-copy a patched file into a staged addon. An mtime-preserving overwrite on a synced (OneDrive) folder silently reverted a fix. Always restage through the build.
- A staged addon binary can lag the source; probe a canary attribute before trusting it.
- Headless Blender runs a script with a different working directory; use absolute output paths or every leg reports "no output".

## Threading

- Release the GIL around the native render call. Holding it deadlocked OpenMP inside headless Blender, which v1 first "fixed" by building without OpenMP (single-threaded addon renders). After releasing the GIL, OpenMP worked; some v1 docs still state the old rule. Verify thread scaling inside Blender, not only in a test process.

## Tooling around Blender

- Blender 5.1 and 5.2 run locally; 5.2 is the Cycles oracle. Agents can re-render references headlessly instead of waiting for the owner.
- MCP bridges (dated 2026-10, may change): the official Blender Lab bridge on port 9876 worked; a community bridge on the same port broke it. Requests time out after about 60 s, so schedule long renders with `bpy.app.timers` and poll.
- A Blender launcher run with redirected output left a hidden child holding the handles, so the shell chain blocked until Blender exited (9 hours lost). Launch Blender as its own job, confirm readiness with a TCP connect, then start the driver separately and check that it runs.

## Vendoring Blender code

- Check the SPDX line per file. Under `intern/cycles/**` code is Apache-2.0. Sky: `sky_single_scattering.cpp` Apache-2.0, `sky_multiple_scattering.cpp` MIT, `kernel/svm/sky.h` Apache-2.0; only `sky_nishita.h` and `sky_math.h` are GPL-2.0-or-later. An earlier "all GPL" belief led v1 to ship a worse baked Preetham sky.

## Open questions inherited from v1

- Whether the suspected stale host texture cache produced wrong output or fell back silently (needs: change an image after a full sync).
- How often real edits hit the full-sync fallback, and the per-present split between copy time and kernel time.
- Whether Python in Blender can consume external GPU memory directly (zero-copy present), and how the Vulkan backend changes the display path.
- What Blender 5.x's inlined shader-tree API offers an external evaluator, and what node-coverage target is enough.
- Real end-to-end viewport latency with and without denoise.
- The coordinate-convention claim above.
