# 0007 — Blender integration: scene sync, scene representation, presentation and user interface

Status: proposed (2026-11-14), revised after independent review (same day; see "Review and resolution"), reconciled with RULINGS.md (same day; see "Reconciliation")

## Context

Astroray 2.0 ships as a stock Blender 5.3 extension (owner decisions 3, 14) over an engine DLL behind a C ABI that knows nothing about Blender (spine 1). The owner's viewport targets are hard: a steady 30 updates/s at full resolution, 24 the floor, 60 a later goal, more samples per displayed update as the image settles, resolution reduced only below the floor, and time to first frame (TTFF) within 1.5x Cycles, aiming for equal, including on large scenes, where v1 blew up (decisions 9, 10, 21-23). The first release must carry the full breadth: surfaces, volumes, sky and world, textures, lights, gratings (decisions 7, 17), and the black hole in a nebula follows (spine, release structure).

What the addon cannot change is measured. Python reads float attributes with exact dtypes at memcpy speed but integer topology at ~25 ns/element and `loop_triangles` at 50-120 ns/triangle; Blender owns a ~6 ms floor per redraw, and during navigation `view_draw` is called at that cadence; one `GPUStorageBuf.update` above 32 MiB falls off a 10x cliff; adding one object to a 10k-object scene costs Blender 285 ms and yields 9,007 update entries; there is no zero-copy display route for an addon; Python has no voxel read API (spike B sections 1-2; research 02 F2-F4; research 07). With the fast read path and native triangulation, mesh+instance sync plus OptiX GAS/TLAS build is 1.42 s serial on 5M triangles / 10k instances (spike B section 3), without shaders, textures or lights.

v1's integration failed on five primitives: build identity not observable, sync pushed device state without re-reading Blender, UI wired to dead paths, a re-derived camera, and process-global GPU state that forced admission tokens and pause gates (`v1-lessons/research/blender.md`). This record fixes the boundary, the scene model, sync, threading, presentation and UI so that none of those can recur, and so that Sonnet-class agents can implement each piece from a small spec.

Owners this record cites instead of restating (RULINGS, Ownership): 0001 C ABI rules, error and fault model, the per-render report (schema, codes, deduplication, polling), packaging, dependency list; 0002 kernels, OptiX pipeline and program list, the instance visibility mask allocation, pools; 0003 path state and shadow item layout, working space; 0004 transport, light tree, epoch reset rules (D25); 0005 graph semantics, node mapping in the addon (item 29), material kinds on the Material panel (item 16), texture filtering and mips (item 34); 0006 medium interface, grid leaf format (D5), the black-hole object's contents (D25); 0008 film, per-sample buffer and resolve, passes and what their readback returns, denoiser and DLSS; 0009 every time budget except the TTFF gate (ruling 21), tests, CI.

## Decision

Numbers are tagged MEASURED (with source) or PROVISIONAL (a budget to be confirmed by the named check). Decision numbers are stable because other records cite them; items added in review take the next free number (47-53) inside their section.

### A. Process and registration

1. **In-process.** A Python `RenderEngine` subclass `AstrorayRenderEngine` (`bl_idname = "ASTRORAY"`) loads `astroray_core.dll` with `ctypes.CDLL(full_path)` (releases the GIL per call; research 08). No Python threads anywhere in the addon. An out-of-process daemon is not built; the ABI below crosses only flat arrays and PODs, so it can be moved behind shared memory if 0001 D6's gate trips.
2. **Engine flags:** `bl_use_shading_nodes_custom = False` (expose Blender's nodes), `bl_use_preview = True`, `bl_use_eevee_viewport = True` (Material Preview shading uses EEVEE, as Cycles), `bl_use_postprocess = True`, `bl_use_gpu_context = False`, `bl_write_viewport_depth = False` (decision 31).
3. **Build identity.** At `register()` the addon calls `ar_abi_hash` and `ar_abi_layout`, requires the hash to equal `_abi.py`'s and every layout entry to equal ctypes' own (0001 D2.7), and prints `ar_build_info()`. Any mismatch fails registration with a message naming the hashes or the struct field. This replaces v1's stale-module failure mode.

### B. The C ABI functions for scene edits and render control (header `astroray.h`, conventions 0001 D2)

4. **Conventions.** Naming, POD types, copy-in and staged arrays, annotations, `ar_status` returns, `ar_<object>_last_error`, no callbacks, exact hash match with no cross-version promise: 0001 D2. This record adds only: keys are `ar_key` (0001 D2.2) assigned by the host (decision 18); an open scene edit is an `ar_edit` handle (0001 D2.1); the addon holds one `ar_context` per device in use, the device being selected when the context is created (ruling 33; log path under the extension user directory; recording when `ASTRORAY_RECORD` is set; 0001 D2.1, D2.5) for its lifetime; every `RenderEngine` instance owns one `ar_scene` and one `ar_session` on it, and nothing is shared between instances (decision 28). Messages reach the user through 0001's per-render report; progress through `ar_session_get_progress`.
5. **Function list** (this record owns the render-control, scene-edit and film/pass/statistics readback lists, ruling 33). Every call returns within 0009 B-21; slow work (device init, joins, validation, decode, builds) runs on engine threads and reports through `ar_session_get_progress` and the report. Annotations are in the header (0001 D2.10); `AR_QUERY` calls are marked.

```c
/* identity, device enumeration, context create/destroy, last_error, report polling: 0001 D2 */
/* scene: host scene, its device copy, worker pool and generations; one per RenderEngine instance */
ar_status ar_scene_create(ar_context, const ar_scene_desc*, ar_scene*); /* returns at once; the context's device; device init on a scene thread (23) */
ar_status ar_scene_trim(ar_scene);       /* paused owner session: frees the device copy; state TRIMMED (14) */
ar_status ar_scene_destroy(ar_scene);    /* after its session reports CLOSED */
/* session: one render (viewport region, F12 job, preview) of one scene; render thread, film, pools, display ring */
ar_status ar_session_create(ar_scene, const ar_session_desc*, ar_session*); /* returns at once in INITIALIZING; mode VIEWPORT|FINAL|PREVIEW */
ar_status ar_session_close(ar_session);    /* non-blocking: cancel, stop at the next launch boundary, free session device memory */
ar_status ar_session_destroy(ar_session);  /* frees the handle; blocks only if called before progress reports CLOSED */
ar_status ar_session_start(ar_session);  ar_status ar_session_pause(ar_session);
ar_status ar_session_resume(ar_session); ar_status ar_session_cancel(ar_session);
ar_status ar_session_set_view(ar_session, const ar_view_desc*);   /* records the pending view; latched at the next pass boundary (25) */
ar_status ar_session_set_display_mode(ar_session, uint32_t mode); /* BEAUTY or a heat-map buffer (40) */
ar_status ar_session_get_progress(ar_session, ar_progress*);      /* AR_QUERY: state, active/pending/failed generation, spp, pass ms, ledger, device free/total */
ar_status ar_session_wait(ar_session, uint32_t timeout_ms);       /* named wait call (0001 D2.6); F12 job thread only */
/* scene edit transaction: one open edit per scene; Blender main thread, or update() for F12 */
ar_status ar_scene_edit_begin(ar_scene, ar_edit*);
ar_status ar_edit_reset(ar_edit);                     /* first call only: the edit replaces the whole scene */
ar_status ar_edit_mesh_begin(ar_edit, const ar_mesh_desc*, ar_mesh_buffers*);       /* staged arrays (0001 D2.3) */
ar_status ar_edit_positions_begin(ar_edit, ar_key geom, ar_positions_buffers*);     /* deformation fast path */
ar_status ar_edit_curves_begin(ar_edit, const ar_curves_desc*, ar_curves_buffers*);
ar_status ar_edit_points_begin(ar_edit, const ar_points_desc*, ar_points_buffers*);
ar_status ar_edit_grid_leaves_begin(ar_edit, const ar_grid_desc*, ar_grid_leaf_buffers*); /* a batch of active 8^3 leaves of one grid (47; format 0006 D5) */
ar_status ar_edit_image_begin(ar_edit, const ar_image_desc*, ar_image_buffers*);    /* source PIXELS */
ar_status ar_edit_set_image(ar_edit, const ar_image_desc*);                         /* source FILE, or MEMORY as a copy-in byte array */
ar_status ar_edit_item_commit(ar_edit, ar_key);       /* hands that key's staged arrays to the workers at once */
ar_status ar_edit_remove(ar_edit, uint32_t kind, ar_key);
ar_status ar_edit_set_instances(ar_edit, const ar_instances_desc*);                /* full replace */
ar_status ar_edit_update_transforms(ar_edit, uint32_t n, const uint32_t* idx, const float* xf3x4);
ar_status ar_edit_set_graph(ar_edit, ar_key, uint32_t target, const ar_graph_desc*); /* SURFACE|VOLUME|DISPLACEMENT|LIGHT|WORLD */
ar_status ar_edit_set_material(ar_edit, ar_key, const ar_material_desc*);          /* graph keys + kind + settings */
ar_status ar_edit_set_light(ar_edit, ar_key, const ar_light_desc*);
ar_status ar_edit_set_world(ar_edit, const ar_world_desc*);
ar_status ar_edit_set_camera(ar_edit, const ar_camera_desc*);                      /* final-render camera */
ar_status ar_edit_set_settings(ar_edit, const ar_settings*);                       /* includes allow_spill (14) */
ar_status ar_edit_set_astro(ar_edit, ar_key, const ar_astro_desc*);                /* Astroray object (41; contents 0006 D25) */
ar_status ar_edit_commit(ar_edit, uint64_t* generation); /* O(1) checks, returns at once; outcome by generation (9) */
ar_status ar_edit_abort(ar_edit);
/* display: both AR_QUERY (no render input, never recorded) */
ar_status ar_display_acquire(ar_session, uint64_t after_frame, ar_display_frame* out); /* out->frame_id = 0 if none newer */
ar_status ar_display_release(ar_session, uint64_t frame_id);
/* film, pass and statistics readback: all AR_QUERY; what they return is 0008 decision 13a */
ar_status ar_session_pass_count(ar_session, uint32_t* count);
ar_status ar_session_pass_info(ar_session, uint32_t index, ar_pass_info* info);
ar_status ar_session_read_pass(ar_session, uint32_t index, uint32_t quantity, uint32_t n, float* dst); /* MEAN|VARIANCE|COUNT; AR_OUT_ARRAY(n, float) */
ar_status ar_session_read_counters(ar_session, uint32_t n, uint64_t* dst);    /* 0004 D29 per-pass counters; AR_OUT_ARRAY(n, uint64_t) */
ar_status ar_session_write_fits(ar_session, const char* path, uint32_t what);  /* SCIENCE file or film DUMP (0008 decision 13) */
```

6. **Zero-copy staging.** `ar_edit_*_begin` returns pointers into a scene-owned host arena (0001 D2.3 staging); Python wraps them as numpy views (`np.ctypeslib.as_array`) and `foreach_get` writes straight into them (research 01 D2), then `ar_edit_item_commit` hands them to workers. The arena grows in 64 MiB chunks pre-faulted by a worker thread ahead of use, which removes most of the cold first-touch cost (spike B: native copy 146 ms cold vs 23 ms warm on S2). Views are deleted by the Python wrapper at item commit; arena chunks are recycled only after their jobs finish.
7. **Data layouts.** Matrices are 3x4 row-major float32 object-to-world. Every descriptor of decision 5 has its field list in `0007-appendix-abi.md`, part of this record (ruling 33); what the readback calls return is 0008 decision 13a.

8. **Graph transport.** Python walks the tree returned by `inline_shader_nodes()` and translates each Blender node into 0005's host-neutral ops (the Blender-to-engine mapping lives in the GPL addon, 0005 item 29; the core sees no Blender node types, spine 1). The result crosses as flat arrays (`ar_graph_desc`: op nodes {op u16, param range}, param pools {f32, i32, UTF-8 strings}, links {from node, socket, to node, socket}); the core compiler of 0005 lowers it to VM bytecode and returns its errors as report entries. The compiler therefore runs from the CLI host with no Blender, and Python never emits bytecode.
9. **Validation and edit outcome.** Atomicity is per generation, not per call. `ar_edit_commit` performs only O(1) checks synchronously (enum ranges, known keys, filled counts equal allocated counts) and returns the generation the edit will produce. The O(n) checks (`corner_vert < V`, monotonic `face_start`, finite positions, leaf counts, about 1 ms per 1M corners, PROVISIONAL) run on the worker that triangulates or converts that item, before its upload. The engine keeps rendering the previous generation; the new one becomes active only when every job of the commit has succeeded, and replaced device objects are freed only then (peak memory: old plus new for the edited keys). Outcomes:
   - validation failure: the whole generation is dropped, the previous scene stays on screen, `ar_progress.failed_generation` is set and a report entry (0001) names the datablock; new device objects of that edit are freed, so nothing half-valid is ever rendered;
   - image decode failure: that image becomes a magenta texture with a report entry (an unreadable input is reported, never silently skipped);
   - out of device memory: decision 14.
   The addon reads outcomes in `view_draw` or the timer of decision 26 and shows them through the report (decision 38); F12 `render()` fails the render with the message.

### C. Engine scene representation

10. **Geometry.** One record per geometry key: triangulated mesh (triangle → three corner indices; vertex, normal, UV and attribute lookups go through the corner), curves (OptiX curve primitives), points (OptiX spheres), volume grids (0006). Triangulation is native: triangles copied, quads split by Blender's rule (determined by a golden test against `loop_triangles` before it is implemented, then implemented identically), n-gons of 5+ by ear clipping on the polygon projected along its Newell normal (Eberly 2002). The spike's fan triangulation is wrong for concave n-gons and is not used. Tangents (MikkTSpace, Mikkelsen 2008) are computed only for geometry whose materials request them.
11. **Acceleration structures.** One compacted OptiX GAS per geometry key (PREFER_FAST_TRACE | ALLOW_COMPACTION), built in batches from one temp arena with one stream sync per batch; one TLAS over all instances, always rebuilt, never refit (MEASURED: TLAS 4.9 ms for 10k instances, GAS 2.9 ms per 1M triangles including compaction, spike B 2.1). GAS are not refit either: a rebuild of the edited geometry costs less than a frame and keeps trace quality (v1 noted refit degrades). GAS live for the scene's lifetime and are reused across edits; there is no disk cache unless decision 35's remedies require one. CPU target: Embree with the same per-geometry/instance structure.
12. **Edit → work map** (every row except selection produces a new generation; image-affecting edits start a new epoch, display-only settings do not; terms in decision 25):

| Edit | Host call | Engine work | Acceleration structure |
|---|---|---|---|
| viewport camera / region | `set_view` | none | none |
| object transform (non-instancer) | `update_transforms` | patch instance buffer | TLAS rebuild |
| instancer moved, object added/removed, collection toggled | `set_instances` (+ new geometry) | rebuild instance and material-table buffers | new GAS only for new keys; TLAS rebuild |
| deformation, topology provably unchanged (22) | `positions_begin` (+ corner normals) | upload positions/normals | GAS rebuild of that key + TLAS |
| any other geometry change | `mesh_begin` | re-triangulate, upload | GAS rebuild + compaction + TLAS |
| material value or graph | `set_graph` / `set_material` | recompile program, upload | none (true displacement: geometry rebuild) |
| image changed | `set_image` / `image_begin` | decode, mips, upload | none |
| volume grid changed | `grid_leaves_begin` × batches | NanoVDB build, majorant grid (0006) | none |
| light / world | `set_light` / `set_world` | light tree or environment map rebuild (0004), only when emission inputs change | none |
| settings | `set_settings` | per field; resolution reallocates film and path state | none |
| full resync, trimmed resume, F12 view layer, preview | `reset` + everything | replace scene | all |
| selection only | nothing | nothing | none |

13. **Textures.** Stored at source precision (8-bit, half or float; 1, 3 or 4 channels), decoded on the scene's workers with stb_image (PNG, JPEG, TGA, BMP, HDR) and tinyexr (EXR); both are header-only and are used only as entries on 0001 D9.1's dependency list (ruling 20). Colour conversion to the working space by interop id (decision 21). The workers build the mip chain 0005 item 34 filters with (a third more texture memory, counted in the textures category); there is no demand loading.
14. **VRAM: the reserve rule and the budget table** (ruling 19; this replaces 0002 D13's pool reserve and 0008's free-memory check, which cite this decision).
   - **Ledger.** Every device allocation goes through 0002 D13's allocator, tagged by category; per-scene and per-session totals and the device's free/total bytes are reported in the render panel and in `ar_progress`.
   - **Reserve rule (the only one).** On the CUDA device, any allocation, including 0002 D5's pool sizing and 0008's film layout, proceeds only if need + R ≤ the free bytes `cuMemGetInfo` reports at that moment, with R = 1.5 GiB for Blender and the desktop (PROVISIONAL, measured by P5 with Blender open). Free bytes already reflect Blender and every other session, so the rule needs no shared state. Order when it is not met: **trim, then spill, then fail.**
   - Trim: a session cannot free another (no process-global state), so the addon owns this step. An F12 scene is created with `allow_spill = false` while any viewport scene on the same device is paused and still holds a device copy. If its commit fails with out-of-memory, the addon trims those paused scenes (`ar_scene_trim`), sets `allow_spill = true` and re-syncs once with `ar_edit_reset`. A trimmed viewport does a full sync when it resumes.
   - Spill: with `allow_spill` set, textures and volume grids move to host-mapped pinned memory (Cycles' fallback, `generic_alloc` in `intern/cycles/device/`, file to confirm on reading; survey.md item 4) and the report names what moved. Viewport scenes start with `allow_spill = true`.
   - Fail: acceleration structures, geometry and path state never spill; if they do not fit, the session fails loudly with "needs X GiB for geometry, Y available" and Blender stays alive. Never a silent downscale.
   - **Budget table**, fixed per-session device cost with scene data (geometry, acceleration, textures, volumes) excluded. Sizes come from the named owners; the totals are this record's PROVISIONAL budget, checked by V14 and the ledger.

| Category | Size from | 1080p | 4K | Allocated when |
|---|---|---|---|---|
| main path pool | 0003 bytes/path × 0002 D5 capacity (2^21) | ≤ 512 MiB | ≤ 512 MiB | always |
| shadow pool | 0003 bytes/item × 2× main | ≤ 512 MiB | ≤ 512 MiB | always |
| queue and sort indices | 0002 D6 | ~32 MiB | ~32 MiB | always |
| shadow hit records | 0003 decision 20 side buffer (84 B per shadow slot at K = 4) | 336 MiB | 336 MiB | non-opaque instances |
| shadow-pass split | 0003 decision 20 side buffer (17 B per shadow slot) | 68 MiB | 68 MiB | light passes |
| geodesic record | 0003 decision 20 side buffer (92 B float / 164 B double per path slot) | 184 / 328 MiB | same | black hole |
| random-walk record | 0005 item 4a (40 B per path slot) | 80 MiB | 80 MiB | subsurface materials |
| film incl. per-sample buffer | 0008 `FilmLayout` (decision 21) | ~110 MB moments + 41 MB per-sample buffer + enabled passes | ~4× 1080p | always |
| tables | 0003 upsampling | 9.4 MB per colour space | same | always |
| display frame (device RGBA16F) | decision 29 | 15.8 MiB | 63.3 MiB | viewport |
| denoiser | 0008 (OIDN) | measured by P5 | measured by P5 | denoising on |
| DLSS | 0008 (research 10) | 154-579 MB | 154-579 MB | DLSS mode |
| **Total, always-allocated rows** | | **≤ 1.5 GiB** | **≤ 2.0 GiB** | |

15. **Motion and deformation.** Instances and meshes carry `motion_steps` (S = 1 costs one u32 per descriptor); rays carry `ray_time` (0003). Camera and object (transform) motion blur in final renders: transforms sampled with `engine.frame_set(frame, subframe)` in `update()`, interpolated with T/R/S decomposition and slerp (Shoemake 1985; pbrt-v4 `AnimatedTransform`), built as an OptiX motion TLAS; the motion pipeline variant (0002's program list) is selected at runtime only when the scene has motion. Deformation blur fills S position steps and a motion GAS; until it is implemented, an object with deformation and motion blur enabled renders unblurred with a report entry. Viewport never blurs. **Previous-frame data for 0008's motion buffer** (decision 15 there): a final render with the Vector pass samples camera and transforms (and position steps where deformation blur exists) at frame − 1 as well, through the same `frame_set` route, as Cycles does for its vector pass; in the viewport the engine keeps the camera and instance transforms of the previously presented generation, so no extra ABI field is needed and viewport motion is rigid only.

### D. Python sync

16. **Plain functions.** Sync lives in `astroray/sync/` as functions taking a depsgraph, an `ar_edit` wrapper and a cache object, unit-tested headlessly with `pip install bpy==5.3.0` (a `RenderEngine` cannot be instantiated outside Blender; v1 lesson).
17. **Mesh read** (spike B 2.2): `attributes['position']` float32, `attributes['.corner_vert']` int32 (falls back to `loops.foreach_get('vertex_index')` with a console warning and a report entry if the internal name disappears), `polygons.foreach_get('loop_start')`, `corner_normals` float32, UV and requested attributes float32 by attribute, `material_index` only if the attribute exists. Never `loop_triangles`, never per-element RNA, never a float64 destination. Curves, text, surfaces and metaballs go through `to_mesh()`/`to_mesh_clear()` on the evaluated object. Hair `Curves` read `position` and `radius` (float) and curve offsets.
18. **Keys.** Identity of a geometry: the original data's `session_uid` when the evaluated data is that datablock's shared evaluated copy (linked duplicates share one GAS); otherwise (modifiers or geometry nodes produced per-object geometry) the tuple (original object `session_uid`, instance `persistent_id[]`). This is Cycles' `BObjectInfo::is_real_object_data()` rule (`intern/cycles/blender/`, read before implementing). Never the evaluated mesh's own `session_uid` (spike B: 2 values for 2,001 meshes). Object identity = original object `session_uid` (+ `persistent_id[]` for instances). The addon maps each identity tuple to an `ar_key` from a per-scene counter through a dict; there is no hashing, so keys cannot collide.
19. **Per-instance loop** (spike B recommendation 4): one pass over `depsgraph.object_instances` filling preallocated arrays (`np.asarray(inst.matrix_world, np.float32)`, ~3 µs per instance, MEASURED 30 ms for 10k); per-object properties (visibility, pass index, colour, material slots) read once per original object and cached; a map from original object `session_uid` to instance indices (not the name, which goes stale on rename) makes a plain move cost 0.1 ms (MEASURED).
20. **Update classification.** `depsgraph.updates` is deduplicated by (ID type, original `session_uid`) into one bitmask per datablock, then applied in fixed order: settings, world, images, materials, lights, geometry, instances, camera (v1 order). Every ID type has an explicit entry, each covered by a V4 case; the model is Cycles' `BlenderSync::sync_recalc` in `intern/cycles/blender/sync.cpp`, which reacts to a few types and relies on Blender tagging dependent objects (read before implementing):

| ID type | Action |
|---|---|
| Scene | render-settings hash changed → settings; else ignore (selection, frame number) |
| Object | transform → patch, or re-iterate if instancer (5-8 ms at 12k, MEASURED); geometry → re-read that key (22); shading with unchanged material-slot hash → ignore (absorbs the 9,007-entry storm, MEASURED parse 7.4 ms) |
| Mesh, Curve, Curves, PointCloud, Volume, MetaBall, Text | re-read the keys of objects using it |
| Material, its NodeTree | re-inline that material |
| NodeTree used as a group | re-inline every material whose raw tree contains it (map built at full sync from group nodes only) |
| Light, World, Image, Camera | their setter |
| Collection, or `id_type_updated('OBJECT')` with a changed instance key set | re-iterate instances and diff |
| Armature, Action, Key, Lattice, ParticleSettings, Texture, CacheFile | ignore: their effect arrives as Object transform/geometry entries, which Blender tags |
| WindowManager, Screen, WorkSpace, Brush, Palette, PaintCurve, Text (script), Library, Mask, MovieClip, Sound, Speaker, LightProbe, FreestyleLineStyle, GreasePencil | ignore (not rendered or not scene data) |
| any type not in this table | full resync plus a WARNING counted in the report |

21. **Images without Python lists.** FILE source when the image is file-backed, not packed and not dirty: the engine reads the resolved path (`filepath_from_user()`, UDIM tiles from `image.tiles`). MEMORY source for packed files: `packed_file.data` passed by buffer protocol. PIXELS source for generated, dirty, movie or engine-undecodable images: `image.pixels.foreach_get` into a staged buffer. Colour space by `colorspace_settings.interop_id` (new in 5.3); the engine supports the ids that are matrices plus transfer functions (sRGB and linear Rec.709, Rec.2020, ACEScg, ACES2065-1, Display P3, Non-Color); any other id renders magenta with a report entry. Engine image cache per scene, keyed on (path or packed hash, interop id, alpha mode, tile, file size and mtime, reload counter) (v1 lesson).
22. **Topology cache: only when topology provably cannot have changed.** Per geometry key Python keeps (V, C, F). A geometry update sends positions and corner normals only (MEASURED 1.1 ms for 1M triangles) in exactly two cases: (a) the object's modifier stack is empty or deform-only (a fixed list of Blender's deform-only modifier types: Armature, Cast, Curve, Displace, Hook, Laplacian Deform, Lattice, Mesh Deform, Shrinkwrap, Simple Deform, Smooth, Corrective Smooth, Laplacian Smooth, Surface Deform, Warp, Wave) and the batch has no geometry update on the original Mesh: armature, shape-key and lattice playback; (b) the only modal operator running in `context.window.modal_operators` is on an allow-list that cannot change topology (the transform family; sculpt brush strokes with dynamic topology off). Every other geometry update is a full read (MEASURED 65 ms per 1M triangles): edit-mode operators such as edge rotate, flip normals or sort elements, geometry nodes and generative modifiers, frame changes on non-deform stacks. If counts differ on the fast path the addon does a full read and adds a report entry (an allow-list error is visible). Unknown cases fall to the full read, which costs time but is never wrong. F12 always does a full read.
47. **Volume grids.** Blender 5.3 bundles the OpenVDB Python module (`5.3/python/lib/site-packages/openvdb.cp313-win_amd64.pyd`, present in the local install, 2026-11-14), so voxels leave Blender without OpenVDB in our build or our zip. Per Volume object in `view_update`/`update()`: if it is file-backed with no modifiers and frame-independent, Python opens the resolved `.vdb` path with `openvdb`; otherwise it writes the evaluated grids with `VolumeGrids.save` to the scene's temporary directory and reads that file. For each grid used by the material it copies the active bounding box (`evalActiveVoxelBoundingBox()`) in 64³ chunks (1 MiB transient each at one channel) with `copyToArray`; NumPy keeps the 8³ leaves holding any non-background value and copies them into `ar_edit_grid_leaves_begin` staged arrays, one batch per chunk (the sparse leaf ABI of ruling 11 and 0006 D5). The engine builds NanoVDB from the leaves, so device memory and transfer scale with active voxels; host memory is bounded by the chunks in flight; time grows with the bounding-box volume (PROVISIONAL until P6). Grid names map to 0006's density/temperature/colour/velocity/ionization slots by the material's Volume Info and Attribute nodes; a grid type other than float or vec3 is reported and not rendered.

### E. Threading, generations, cancellation, coexistence

23. **Who runs where.** Blender main thread: `view_update`, `view_draw`, panels, all `bpy` reads. F12 job thread: `update()` (all reads, under Blender's draw lock) and `render()` (no scene reads; polls progress). Native only: per scene, one device-init thread and a worker pool of min(8, logical cores / 2) threads for validation, triangulation, decode, mips, uploads and GAS builds; per session, one render thread that owns its streams and launches. No Python thread, and no engine call blocks the main thread beyond 0009 B-21 except copying data the host itself supplied. Scene creation therefore returns at once: device init (CUDA context, 0002 D11 module and pipeline creation; within 0009 B-18) runs on the init thread while Python already stages the first sync; uploads wait on it, and a failure follows 0002 D15. Destruction is two steps: `RenderEngine.__del__` calls `ar_session_close` and parks both handles; the timer of decision 26 calls `ar_session_destroy` and then `ar_scene_destroy` once progress reports CLOSED. Addon unregister and Blender exit call both destroys directly and may block.
24. **Pipelined sync.** Workers start validating, triangulating, uploading and building a key's GAS as soon as its item is committed, while Python keeps reading the next mesh; the generation of an edit becomes ready when every job of that commit has succeeded (decision 9).
25. **Generations, coalesced at pass boundaries.** *Terms* (ruling 37; every record uses them this way): a **generation** is a version of the scene state; every commit and every latched view or settings change produces one, and display frames, `ar_progress` and report entries carry it. An **epoch** is the accumulation reset counter; it advances when a new generation changes the image (decision 12), which clears film, light image, adaptive state and statistics (0004 D25, 0008). A pass is one sample per pixel at the current render scale (0004 D25), split into launches sized to 0009 B-17 (ruling 14). `set_view` and `commit` only record pending state. At each pass boundary, after 0008 resolves that pass's per-sample buffer into the film, the render thread latches all pending state (every view change and every commit whose jobs have finished since the last latch) into one new generation. A pending change that starts a new epoch interrupts the current pass at the next launch boundary only if the current generation has already presented a frame; the interrupted pass is not resolved, and its per-sample buffer, splat plane and directly written channels are cleared with the rest of the film by that epoch's reset (0008 decision 3). Otherwise, and always for a change that starts no epoch, the pass completes and is presented first. During a continuous orbit (a `view_draw` about every 6 ms, spike B 1.3) the engine therefore presents one pass for the newest view each time a pass completes, instead of restarting forever; latency after a change is at most the rest of the pass in flight plus one pass plus present. Idle accumulation still stops within one launch. Every display frame carries its generation; `view_draw` uploads a frame only if its generation is ≥ the last presented one and keeps showing the old frame until the new generation has one (no black flash, no late stale frame).
26. **Redraw loop.** `view_draw` calls `tag_redraw()` at its end while the session is rendering or has pending async work; when converged it stops. Asynchronous completions after convergence (commit outcomes, image decodes, sessions closing) are picked up by a `bpy.app.timers` poll at 0009 B-37's period that exists only while such work is pending. No Blender call is ever made from a non-main thread (spike B saw `tag_redraw` from a thread work but established no thread safety).
27. **Cancellation.** `render()` loops on `ar_session_wait` with 0009 B-37's timeout, calls `update_stats`/`update_progress`, posts a preview with `update_result` at B-37's preview interval, and calls `ar_session_cancel` when `test_break()` is true; cancel stops at the next launch boundary. Partial F12 results are returned on cancel, as Cycles does.
28. **Coexistence.** Each viewport region, each F12 render and the material preview have their own scene and session; nothing is shared (two rendered viewports cost two scene copies; accepted). Viewport sessions pause in `view_pause` and resume in `view_resume` when a final render runs (5.3 Cycles behaviour; probe P2 verifies Blender calls them on F12, and if not, the addon pauses its weakly-referenced viewport sessions from `update()`). Memory pressure between them follows decision 14 (trim, then spill, then fail). Material previews reuse one long-lived addon-held PREVIEW scene and session behind a Python lock, each preview starting with `ar_edit_reset`, so a thumbnail does not pay OptiX context creation (B-18's basis) each time. `scene.render.use_persistent_data` keeps the F12 scene and session across animation frames with delta sync.
48. **View layers and passes in F12.** Blender calls `update()` and `render()` once per view layer to render. One F12 scene and session serve the job; each layer's `update()` begins with `ar_edit_reset` and syncs that layer's depsgraph, then `render()` writes the layer's result through decision 5's readback calls (contents: 0008 decision 13a). `update_render_passes` registers every enabled pass of 0008's list with `register_pass` so compositor sockets exist; `render()` writes the same passes into the result. Pass names come from 0008's table (a known 5.4 fragile point, decision 46).

### F. Presentation

29. **Path.** The engine converts the film to scene-linear RGB in the file's working space (spine 6; conversion owned by 0008), clamps to the finite half range (±65504), writes RGBA16F into a 3-slot pinned host ring with an async copy on a copy stream. `view_draw` acquires the newest frame, views it as `uint32` pairs, updates persistent `GPUStorageBuf` stripes, dispatches a 16×16 compute shader (`unpackHalf2x16`) into one persistent RGBA16F `GPUTexture`, and draws it with `draw_texture_2d` inside `bind_display_space_shader`/`unbind_display_space_shader` with `ALPHA_PREMULT` blending (spike B 1.4: no barrier needed, Blender applies the view transform afterwards). The code uses only Blender's `gpu` module, so the same path serves the Vulkan default and the OpenGL backend a user can select (research 01 recommendation 6); only Vulkan was measured, and V8 runs on both.
30. **Formats and the 32 MiB rule.** One format at every resolution: RGBA16F, split into stripes of at most 16 MiB each, each its own SSBO (MEASURED: 1080p is one 15.8 MiB update at 0.74 ms and holds 60 and 120 updates/s; 4K as four 16 MiB stripes held 60.1/s at a 60 Hz producer and 77.5/s at 120, spike B 1.1-1.2; 1440p as two stripes is interpolated, not measured). No 8-bit display-referred present (double view transform). SSBOs and texture are reallocated only when size changes; up to two sizes are cached for the resolution controller. Frames are published at decision 50's rates (numbers: 0009 B-34), with interaction as defined in decision 49.
31. **Depth for overlays.** Without `bl_write_viewport_depth` Blender runs its own depth prepass of the scene geometry (research 01 B4), which is correct for every rendered surface while engine surfaces equal rasterised geometry. Volumes have no single depth, so overlays behind a volume show through it, as they do under Cycles, which also leaves depth to Blender. The flag is switched on when true displacement (0005) or curved rays (0006) make the engine's surfaces differ from rasterised geometry; it then uses a custom fragment shader writing `gl_FragDepth` from the engine's linear depth and Blender's window matrix. Cost when unused: none.
32. **Colour management.** No engine tonemapping in the beauty path; Blender's view transform, look, exposure and gamma apply (verified by V9).
33. **Presentation cost.** `view_draw` MEASURED 0.8 ms at 1080p and 7.4 ms at 4K (paced); its budget is 0009's (B-13). The ~6 ms Blender floor caps updates at ~1/(6 ms + view_draw).
49. **View changes and interaction.** Orbit and pan do not arrive through `depsgraph.updates`; they only cause `view_draw` (research 01 B8). `view_draw` hashes (view matrix, projection, region size, camera-view state, border) and calls `set_view` only when the hash changes. *Interaction* means a commit or a `set_view` whose hash changed within 0009 B-37's interaction window; render-scale changes made by the controller do not count, so the controller cannot keep itself in interaction. The controller (decision 34), the publish rate (decision 30), batching (decision 50) and the return to full resolution use this one definition.
50. **Progressive batching** (owner decision 22; ruling 30). During interaction, which covers the view changing and having just stopped (decision 49), each displayed update is one pass, a single sample per pixel at the current scale. After interaction ends, the render thread runs several passes between displayed updates and the number grows as the image settles, following Cycles' viewport display-interval schedule (`render_scheduler.cpp`), so less time goes to resolve, denoise and present and more to rendering. The rates and the schedule's values are 0009 B-34. DLSS mode keeps one sample per frame (0008 decision 19).

### G. Dynamic-resolution controller

34. Scale levels (linear) {1, 3/4, 1/2, 3/8, 1/4} (PROVISIONAL). Active only during interaction (decision 49). Inputs, sampled in every `view_draw` (about every 6 ms while navigating): the engine's per-pass time at the current scale from `ar_progress` (0002) and the age of the newest presented frame. Step down one level after 3 consecutive passes whose exponential moving average (α = 0.3) is above the frame interval of 0009 B-14's floor (owner decision 10), or at once when the newest presented frame is older than twice that interval during interaction, so that the controller acts even when no frame arrives. Step up when the average scaled by the next level's pixel ratio predicts B-14's target rate for 0009 B-37's step-up hold. Response limits: 0009 B-33. When interaction stops, return to scale 1 and accumulate at full resolution. A reduced frame is drawn stretched with linear filtering. With DLSS enabled (0008) the levels map to DLSS modes and the present is full resolution; a mode change recreates the DLSS feature (DLSS-RR does not support dynamic resolution, research 10), cost to be measured. The structure follows Cycles' resolution divider (`intern/cycles/integrator/render_scheduler.cpp`, `calculate_resolution_divider_for_time`), but the trigger is the owner's floor, not a fixed navigation divider; decision 25's pass-boundary coalescing is what keeps frames arriving during navigation.

### H. Time-to-first-frame gate (viewport, 1080p, 1 spp; stated here only, ruling 21)

35. **Two measurements, bounded the same way for both engines.** V7a (the gate, user-perceived): wall time from a timer callback that switches the region to Rendered shading to the first frame showing engine pixels, detected by an external screen-capture poller of the region at ≥ 100 Hz (spike B's `CopyFromScreen` approach, own PID; resolution ≤ 20 ms), same harness for Cycles and Astroray, on S1, S2 and a textured scene S3, both for the first switch after opening the file and for a second switch. Both sides therefore include their own Blender sync. V7b (diagnostic, from logs, excludes session and device creation for both): Cycles = timestamp of the first `Rendered N samples` line minus (timestamp of the `Total time spent synchronizing data: T` line minus T); Astroray = first-frame-presented log line minus first `ar_scene_edit_begin` log line. **Gate on V7a: ≤ 1.5× Cycles; goal ≤ 1.0×** (owner decision 23; ruling 21). If S2 fails the gate, the remedies in order are: `as_pointer()` topology reads (removes ~330 ms, version-fragile), a cross-session GAS disk cache (`optixAccelRelocate`), parallel material inlining moved earlier.

V7b estimate for S2 (5.04M triangles, 10k instances, 300 materials). Cold = first switch in a fresh process after the file load (spike B's 3 fresh-process runs); warm = spike B's warm minimum.

| Phase | Cycles viewport (log, `cycview_summary.txt`) | Astroray | Source |
|---|---|---|---|
| Blender depsgraph evaluation | same | same | excluded from both |
| Session and device creation | before the log window, not measured | 85 ms + 0-300 ms on the init thread, hidden behind the reads (decision 23) | A §5 MEASURED + ESTIMATE |
| Data sync on the main thread | 57 ms (C++ sync, log line) | instance loop 30 + mesh reads 752 cold / 366 warm | B 2.1: 928 − 146 − 30 cold; 419 − 23 − 30 warm |
| Materials | "307 shaders" (inside Cycles' window) | 90-300 ms inline + op walk on the main thread; compile on workers | NOT MEASURED |
| Lights (≤ 100) | | < 5 ms main thread | estimate |
| Triangulate + copy, workers | | 146 cold / 23 warm, overlapped with the reads | B 2.1 |
| GAS + compaction + TLAS + upload, workers/GPU | ~906 ms to `Using OPTIX layout` (BVH dominates; ~950 ms in the F12 log) | 491 cold, overlapped; workers' serial total with the row above 637 cold / 514 warm | B 2.1 |
| OptiX modules (disk cache warm) | | ~1 ms | A; cold ~1 s per module once |
| Tail after the last read | | ≤ 100 ms (last GAS batch + TLAS) | PROVISIONAL |
| Device sync, first sample, present | ~62 ms | 10-20 ms + 0.8 ms + 0-6 ms wait | B + estimate |
| **Total from sync start** | **1,027 ms** (57 + 970) | cold: main thread 877-1,087 + tail + first frame ≈ 1.0-1.21 s (0.97-1.18×); warm: max(main thread 491-701, workers 514) + tail + first frame ≈ 0.63-0.82 s (0.6-0.8×) | |

S1 on the same basis: Astroray 98 ms serial (89 sync incl. native copy, 9 OptiX; spike B 3) against Cycles 132 ms (7 + 125): 0.74×, before materials. The upper end of S2 cold comes from the two unmeasured rows (material inlining, and whether the context really hides behind the reads); V7a also includes Cycles' session and device creation, which no spike measured. V7a/V7b measure all of them; if S2 is over the gate, the remedies above are applied in order before any feature work that adds sync cost.

### I. Feature surface

36. The design covers these Blender features; their order is the roadmap's (G1).

| Area | Covered |
|---|---|
| Objects | meshes (incl. curve, text, surface, metaball via `to_mesh`), all instance kinds (linked duplicates, collection instances, geometry nodes), volumes (VDB-backed `Volume` objects by decision 47, mesh-domain media, world volume), hair `Curves`, point clouds, holdout/shadow catcher, camera+object motion blur, deformation blur (15), true displacement (0005), persistent data, multiple view layers |
| Lights | point, spot, sun, area (all shapes), blackbody temperature, light node trees, IES, emissive meshes (0004), spectrum modes |
| World | constant and node-graph world via VM, HDRI importance sampling, Sky (0004 D12) |
| Cameras | perspective, orthographic, DOF, panoramic types, shift/sensor-fit edge cases, viewport and F12 |
| Passes | 0008's list incl. Cryptomatte, AOV, light groups, science buffers |
| UI | engine panels, report, heat maps, Material panel kinds (0005 item 16), Astroray object panel, asset library, science export, custom shader nodes (decision 39) |

Not designed here, each needing its own decision: Gaussian splats, legacy particle hair, bake, stereo, custom camera, portals, Preetham/Hosek sky, light and shadow linking (an open roadmap item, ruling 45).

### J. User interface

37. **Panels.** Built-in Blender panels whose properties the engine honours get `"ASTRORAY"` added to their `COMPAT_ENGINES` (camera lens and DOF, output, colour management, film transparency); a list in `ui/compat_panels.py` is the only place this happens. Engine-specific settings live in Astroray PropertyGroups (`scene.astroray`, `object.astroray`, `light.astroray`, `material.astroray`, `world.astroray`) with Cycles-like layout and names where the meaning matches. Native datablock properties (light energy and size, ray visibility `Object.visible_*`, `render.film_transparent`, `render.use_motion_blur`) are read directly, never mirrored. Every property has a tooltip with units, and a test that changing it changes a render or greys it out with an INFO line (V17).
38. **Report.** The per-render report is 0001's (schema, codes, deduplication, polling). The addon shows it in a Diagnostics subpanel and via `self.report`. This record's sources of entries: unsupported nodes (magenta, lead default), rejected edits and failed generations, spills, trims, fallbacks (`.corner_vert`, topology fast path, interop ids, grid types, deformation blur), full-resync counts and unlisted ID types.
39. **Custom material types and nodes** (owner decision 16). A material's extension kind and its constants are authored on the Material's Astroray panel (`material.astroray`, 0005 item 16), which works whatever the inliner does. Custom shader nodes (`ShaderNodeCustomGroup` subclasses with a Cycles/EEVEE approximation inside, research 17) are added only if probe P1 (0005's M-7) shows `inline_shader_nodes()` keeps their identity; no side channel through the raw tree is built.
40. **Heat-map view modes** (owner decision 15): `scene.astroray.display_mode` ∈ {Beauty, Sample count, Bounce count, Relative error, Invalid samples, Termination reason, Time per pixel, Rays per sample, Albedo, Normal, Depth}, covering every statistic of 0008 decision 12. The engine colour-maps the chosen buffer (viridis for scalars, Smith and van der Walt 2015; a fixed categorical palette for termination reason) into an RGBA8 sRGB frame with its legend, drawn so the user's view transform cannot distort it (verified by V10). Probe P4 tries the simpler route first: draw it in `view_draw` without `bind_display_space_shader`; if Blender still applies the view transform there, a `SpaceView3D` POST_PIXEL handler draws it instead.
41. **Astrophysics objects.** Black holes are Empty objects (sphere display at the horizon radius) with `object.astroray.astro_kind` and parameters, created by an Add > Astroray menu; sync sends `ar_edit_set_astro` with transform and parameters in the declared unit scale (research 06 R3; contents 0006 D25). No camera velocity crosses the ABI, so the camera is at rest in the hole's static frame, the frame 0006 D24 launches from. The function exists from the start because adding it later changes the C ABI (G4). The nebula is a Volume object or mesh domain with Astroray emission settings.
42. **Asset library** of spectral materials, lamps and gratings as one `.blend` in the extension; an operator in the addon preferences, run by the user, copies it to `extension_path_user` and registers it as an asset library (never automatic: it changes user preferences).
43. **Science export**: an Output-properties subpanel enabling science outputs (FITS cube, photon-count buffers) written by the engine next to the render; contents in 0008. **What an addon cannot do**: add sockets to native nodes (spectra enter through the Material panel or Astroray wrapper nodes), present without one CPU upload per frame, bypass the ~6 ms redraw floor, read voxels through the RNA API (decision 47 goes through Blender's bundled `openvdb` module), add object types, use Blender's DLSS, link custom socket types into native sockets, change how Cycles or EEVEE evaluate nodes.

### K. Crash containment

44. Exceptions, release assertions, structured exceptions (poisoned context) and sticky CUDA errors follow 0001 D2.5. The addon's part: it treats a ctypes `OSError` from an engine call as a poisoned context, shows 0001's messages through the report and as a Blender error, and afterwards offers only the CPU device in that process. Inputs are validated (decision 9). The out-of-process fallback and its triggers are 0001 D6; this record supplies the coexistence soak (V13) that measures trigger (b).

### L. Packaging and Blender 5.4

45. Extension zip contents, manifest and build command are 0001 D10.2 (built with `blender --command extension build`, never from an allow-list; v1 lesson). This record adds: the manifest permission `files` for textures, volumes, caches and science output; the Python package and the generated `_abi.py`; `bin/astroray_core.dll` with kernels and data tables embedded (ruling 13); our pinned OIDN build's renamed `astroray_oidn*` DLLs (0001 D9, ruling 12); the asset `.blend`. No NVIDIA binaries and no OpenVDB (decision 47 uses Blender's own module). The DLL is loaded by full path after `os.add_dll_directory(bin)`. The OptiX disk cache (`cache_dir`) goes to `extension_path_user(__package__, create=True)`.
46. Blender 5.4: the headless sync suite and the API-surface check run in CI against every published `bpy` 5.3.x and, as soon as it appears on PyPI, 5.4 (research 01 K4); a GUI smoke test (install, viewport, F12) runs per Blender version on the GPU machine. The nine boundary files were byte-identical between 5.3 and `main` on 2026-11-14 (research 01 K1); the known fragile points are `.corner_vert` (fallback in decision 17), pass names, `Window.modal_operators` (decision 22, which falls back to full reads if it disappears) and the bundled `openvdb` module's API (decision 47); the API-surface check covers all four.

### Challenges to the spine

- **Mesh keying (spine 6).** "Key meshes by the original datablock's `session_uid`" aliases two objects that share a mesh but carry different modifiers. Decision 18 refines it with Cycles' real-object-data rule. Evidence: spike B did not test modifier-generated meshes (its section 6) and keyed geometry-nodes output only through the empty host mesh.
- **Triangulation (spine 6).** The measured native triangulation was a fan, which is wrong for concave n-gons; decision 10 replaces it with Blender's quad rule plus ear clipping. Speed should be unchanged for triangles and quads; V3 confirms.

## Alternatives considered

- **Hydra delegate.** Free incremental export, but a rebuild against Blender's monolithic USD every release, lossy RGB Principled→OpenPBR mapping, no per-object custom data, and the same CPU-buffer display on Vulkan (research 01 H3-H7). Rejected.
- **Out-of-process daemon now.** Crash isolation, but a protocol, lifecycle and one more host copy, and Python still walks the depsgraph (research 01 option c, research 02 F8). Kept as a gated fallback (0001 D6); the flat ABI keeps it cheap.
- **nanobind module.** Nicer arrays, but a Python-version axis and `/MD` coupling for a surface this small (research 08). ctypes per spine; revisit only if profiling shows boundary cost.
- **`loop_triangles` reads.** 3.5× slower on S1, 2.9× on S2, cold (spike B 2.1); fails the 2× TTFF threshold on S2 (2.5×/3.3×).
- **`as_pointer()` direct DNA reads** (Malt). Would remove the ~25 ns/corner integer read, but depends on internal layout and an internal attribute name and was not measured (spike B 5). Held as remedy 1 in decision 35.
- **Abort the in-flight pass on every view change** (the first draft). Starves the display during a continuous orbit when a pass takes longer than Blender's ~6 ms redraw cadence; Cycles hides this with a fixed navigation divider, which owner decision 10 forbids. Replaced by decision 25.
- **Topology settle timer** (the first draft: re-read topology after 250 ms without a geometry update). Never settles during playback, and shows wrong geometry without warning meanwhile. Replaced by decision 22's provable-cases rule.
- **Native `.vdb` reading in the engine.** Needs OpenVDB with Boost, TBB and Blosc in our build and zip, against the build-time budget and 0001 D9. Rejected while Blender's bundled module works (decision 47).
- **Present path A (new `GPUTexture` per frame).** RGBA32F at 4K: 11.4 ms with a 28.8 ms p99 and 47-49/s; RGBA16F from float32 is 4× slower than RGBA32F at 1080p (spike B 1.1). Rejected.
- **R11G11B10F or RGB9E5 packed 4-byte present at 4K** (spike B's 4K recommendation: 1.5 ms). Cheaper, but banding was not evaluated and alpha is lost; striped RGBA16F holds 60/s at 4K by measurement. Kept as the fallback if V8 fails on a real 4K display.
- **TLAS/GAS refit.** Measured rebuilds are already below a frame; refit costs trace quality and ALLOW_UPDATE memory (v1 lesson). Rejected.
- **A shared scene between viewport and F12 sessions.** The ABI's separate `ar_scene` would allow it and it saves VRAM, but it reintroduces the cross-session locking v1 needed (admission token, pause gate). Rejected; addon-driven trim (decision 14) covers the memory case.
- **Reading Cycles' properties (`scene.cycles`) instead of own PropertyGroups.** Zero UI work, but couples to another addon's names and semantics and fails when Cycles is disabled. Rejected.
- **Engine-side tonemapping** (LuxCore). Double transform under Blender's view transform (research 02 F6). Rejected for beauty; used only for heat maps drawn outside the view transform.

## Consequences

- **Easy:** every edit class costs data proportional to the edit; the engine and its graph compiler are testable from the CLI host with recorded deltas; scenes and sessions are independent, so viewport, F12, previews and several viewports need no global coordination; edits are atomic per generation with the previous image kept on screen; the boundary can move out of process without changing either side's logic.
- **Hard:** integer topology reads stay on Blender's slow path (~25 ns/corner), and edit-mode topology operators pay a full read (65 ms per 1M triangles); two rendered viewports double VRAM and GPU work; an edited key briefly holds old and new device copies; a trimmed viewport re-syncs after F12; a GPU fault ends GPU rendering for that Blender process; heat maps and engine depth need extra draw code; volume transport depends on Blender's bundled `openvdb` module.
- **Cost when unused:** motion steps (one u32 per descriptor, S = 1); `ar_edit_set_astro` (one function); display-mode switch (one enum check per published frame); depth writing (none until enabled); VRAM ledger (one record per allocation); host-mapped fallback (none until VRAM is short).

## What must exist first, and what can come later (G1; order in `docs/ROADMAP.md`)

**Must exist from the first milestone, because retrofitting is expensive:**
- The ABI shape of decisions 4-9 (scene/session/edit handles, staged `_begin`/`item_commit`, generation outcomes, asynchronous create/close, `ar_edit_reset`): every sync function, `_abi.py`, the recorder and the CLI are written against it.
- `motion_steps` in the mesh and instance descriptors, the visibility and instance-flag bits and `light_group` in `ar_instances_desc`, and `ar_edit_set_astro`: adding any of them later changes ABI layouts or the function list, and the path-state fields they feed (0003 decision 19).
- Keys (18), the full classification table (20) and the provable-cases topology cache (22): the per-scene cache and every delta path are built on them.
- Native triangulation with the quad-rule golden test (10): every corner-indexed lookup in shading assumes its output.
- Pass-boundary generations with the generation tag on display frames (25): presentation, cancellation, the interrupted-pass clear and the controller all assume it.
- The VRAM ledger and the reserve rule (14): every allocation site routes through them; adding them later touches every allocator call.
- Threading (23) with no Python threads and no blocking main-thread calls: v1's GIL deadlock and stall class.
- The striped RGBA16F present path and the view hash (29, 30, 49): the 32 MiB cliff fixes the frame format.
- Build identity (3) and crash handling (44): v1's stale-binary and process-state failures.
- V7a/V7b instrumentation on S1/S2: decision 35's remedies change the sync design, so the gate must be measured before breadth adds sync cost.

**Can be added later without redesign:** image sources beyond FILE, UDIM and mips; volumes through decision 47; curves and points; panoramic cameras; camera/object and deformation blur; holdout/shadow catcher UI; the full pass list with `register_pass`; heat-map modes beyond Beauty, sample count and bounce count; the dynamic-resolution controller and DLSS mapping; trim and spill (fail-only until then); Material panel kinds; the Astroray object panel; asset library; science export UI; preview scene reuse; persistent data; the 5.4 CI leg; true displacement and viewport depth writing; custom shader nodes if P1 passes; the GAS disk cache and `as_pointer()` reads, only if the TTFF gate requires them; the out-of-process engine, only if 0001 D6's gate trips.

## Verification

| Check | Method | Pass |
|---|---|---|
| V1 ABI identity | register-time `ar_abi_hash` and layout table compare (0001 D2.7); CI | identical, else registration fails naming the hash or field |
| V2 Triangulation | headless golden test vs `loop_triangles` (triangles, planar/non-planar/concave quads, concave n-gons) | tris and quads index-identical; n-gons cover the same area within 1e-6 relative, no degenerate triangles |
| V3 Sync speed | S1/S2 from spike B `make_scenes.py`, cold and warm | within 0009 B-31 |
| V4 Classification | one scripted case per row of decision 20's table, plus: move, instancer move, material, vertex, add/delete object and collection instance, collection exclude toggle, armature and shape-key playback, particle-instancer edit, selection, node group, image, world, light, rename, an unlisted type | expected action class and call counts; no full resync except the unlisted type; each edit visible against the pre-edit frame |
| V5 Keys | shared mesh with and without differing modifiers; nested collection instances | modifiers → two geometries; linked duplicates → one GAS in the ledger; distinct identity tuples → distinct keys |
| V6 Topology cache | edit-mode edge rotate, flip normals, sort elements; geometry nodes with constant counts and animated topology; armature playback; modal translate in edit mode; sculpt with dynamic topology | every presented frame's triangles equal `loop_triangles`-derived geometry; fast path taken exactly for playback, translate and sculpt without dynamic topology |
| V7a TTFF gate | screen-capture poller from the shading switch, S1/S2/S3, first and second switch, Cycles vs Astroray | decision 35's gate, each scene and switch |
| V7b TTFF phases | log-bounded intervals as in decision 35, per-phase log | reported per phase; explains any V7a gap |
| V8 Present | 300 paced frames at 60 Hz producer, 1080p/1440p/4K texture, Vulkan and OpenGL backends, then a real 4K display | 0009 B-13 limits on each |
| V9 Colour | scene-linear chart vs Image Editor under Standard, AgX, exposure +1, gamma, a look | per-patch difference ≤ 2/255 |
| V10 Heat-map colour | screenshot of colormap patches under AgX, on the route P4 chose | exact sRGB ±1/255 |
| V11 Controller | unit test on recorded traces + GUI with injected 30/60/90 ms passes; continuous scripted orbit on S2 (view change every 6 ms for 5 s) | never steps down while passes meet B-14's floor interval; 0009 B-33's response limits; scale changes alone never count as interaction |
| V12 Latency | F12 cancel; single camera change; the continuous orbit of V11; frame log | within 0009 B-32; zero stale frames after a newer one |
| V13 Coexistence soak | 30 min scripted: two viewports, F12, previews, edits, a VRAM-tight F12 | zero crashes and CUDA errors; viewports resume (trimmed ones re-sync); ledger back to baseline ±1 MiB |
| V14 VRAM overflow and budget | oversized textures with and without a paused viewport; oversized geometry; ledger at 1080p and 4K on S2 | no allocation breaks the reserve rule; trim happens before any spill; then textures spill and the render completes with report; geometry fails with GiB message, Blender alive; always-allocated rows within decision 14's totals |
| V15 Fault containment | test-build fault injection; invalid indices; NaN positions; undecodable image | 0001 D2.5 behaviour, Blender alive, CPU device works; bad edit's generation dropped naming the datablock with the previous frame kept; bad image magenta + report |
| V16 Packaging | clean profile install via `blender --command extension install-file`, F12 default cube; 5.3 and 5.4 headless | renders; every module and data file present; no loose kernel files |
| V17 Honour | enumerate Astroray RNA properties; deterministic CPU renders | each changes a render or is greyed with INFO; every tooltip has units |
| V18 Camera | Blender `world_to_camera_view` vs engine rays via CLI host (persp, ortho, shift, sensor fit, portrait, viewport camera view) | ≤ 1e-5 NDC |
| V19 Images | native decode vs `image.pixels` per format and interop id | 8-bit exact, float ≤ 1e-6 after conversion; unsupported id magenta + report |
| V20 Report | material with an unsupported node | magenta, one report entry per 0001's deduplication |
| V21 Volumes | OpenVDB sample (bunny_cloud) and a geometry-nodes grid through decision 47 vs `copyToArray` of the whole grid cut into 8³ leaves | identical leaf set and voxel values; host peak ≤ chunks in flight + 64 MiB; transport time recorded |
| V22 Main-thread budget | instrument every ABI call from the main thread, including first scene and session create and close | within 0009 B-21 except host-data copies |
| V23 Batching | frame log during and after a scripted orbit on S2 | one pass per displayed update during interaction; passes per update grow afterwards; rates within 0009 B-34 |

## Risks and open items

- **TTFF.** Decision 35 gates V7a at ≤ 1.5× Cycles with 1.0× as the goal (owner decision 23); the S2 cold estimate (0.97-1.18×) passes 1.5× but not 1.0×.
- **Batching schedule** (decision 50): B-34's display-interval values are recorded, copied from Cycles' `render_scheduler.cpp` (task 086).
- P1 (= 0005 M-7): does a custom group node keep its identity through `inline_shader_nodes()`? Decides only custom nodes.
- P2: does Blender 5.3 call `view_pause`/`view_resume` on viewport engines when F12 starts?
- P3: edit-mode read correctness and cost, and whether `context.window.modal_operators` is populated inside `view_update` during a modal transform (decision 22 falls back to full reads if not). Full reads hold 24/s in edit mode only up to roughly 0.6M triangles (65 ms per 1M); larger edit-mode meshes need remedy 1 (`as_pointer()`).
- P4: which heat-map route escapes the view transform (decision 40). 0008's heat-map display item names the POST_PIXEL handler; it follows whichever route P4 picks.
- P5: VRAM used by Blender and the desktop (sets R in decision 14); OIDN's device memory; host-mapped texture speed.
- P6 (= 0006 spike G): the bundled `openvdb` module: reading, `evalActiveVoxelBoundingBox` and chunked `copyToArray` into a writable numpy view (needs its NumPy support), leaf selection speed for a 256³ grid and bunny_cloud, `VolumeGrids.save` cost. Only the module's presence is verified. If P6 fails, Blender volume grids have no route and the lead must decide before volumes are planned (owner decision 7 makes them non-optional).
- Material, texture and light sync cost are unmeasured; the TTFF table is an estimate there and is the most likely gate failure. Cycles' session and device creation, part of V7a, is unmeasured too.
- Real 4K display, the OpenGL backend and the owner's working viewport (overlays, other editors) were not measured; 4K figures carried ±30% run-to-run variation (spike B 1.1).
- DLSS feature re-creation cost on a resolution-level change.
- `.corner_vert` is internal and may be renamed in 5.4.
- Images whose colour space has no interop id (custom OCIO configs) render magenta.
- CUDA context re-initialisation cost when the last scene in a process closes.

## Sources

- Research: `research/01-blender-boundary.md` (B, C, D, E, G, J, K, recommendations 4 and 6), `02-viewport-presentation.md` (F2-F7), `07-blender-feature-surface.md` (F1, F4-F6, volumes, recommendation 5-6), `17-materials-and-custom-types.md` (How these appear in Blender), `04-integrator-architecture.md` (R12), `06-astrophysics-core.md` (R3, R5), `08-build-toolchain.md`, `10-denoise-upscale.md`, `11-licensing.md`, `16-test-scenes-and-assets.md` (OpenVDB samples).
- Spikes: `spikes/B-blender-end-to-end/RESULTS.md` (all tables), `spikes/B-blender-end-to-end/out/cycview_summary.txt` (Cycles viewport log lines), `spikes/A-kernel-budget/RESULTS.md` (section 5: OptiX context and module times).
- Local install listing (2026-11-14): `C:\Program Files\Blender Foundation\Blender 5.3\5.3\python\lib\site-packages\openvdb.cp313-win_amd64.pyd`.
- v1: `v1-lessons/blender-integration.md`, `v1-lessons/research/blender.md`, `v1-lessons/research/gpu-build.md` (BVH, instancing), `v1-lessons/research/transport.md` (motion blur), `v1-lessons/research/survey.md` (host-mapped fallback).
- Papers: D. Eberly, "Triangulation by Ear Clipping", Geometric Tools, 2002; I. Sutherland, R. Sproull, R. Schumacker, "A Characterization of Ten Hidden-Surface Algorithms", ACM Computing Surveys 6(1), 1974 (Newell's polygon normal); K. Shoemake, "Animating Rotation with Quaternion Curves", SIGGRAPH 1985; R. Cook, T. Porter, L. Carpenter, "Distributed Ray Tracing", SIGGRAPH 1984; M. Mikkelsen, "Simulation of Wrinkled Surfaces Revisited", MSc thesis, DIKU, 2008; N. Smith and S. van der Walt, "A Better Default Colormap for Matplotlib", SciPy 2015 (viridis, CC0); K. Museth, "NanoVDB: A GPU-Friendly and Portable VDB Data Structure", SIGGRAPH 2021 Talks.
- Reference code (Apache-2.0 unless noted): Cycles `intern/cycles/blender/sync.cpp` (`BlenderSync::sync_recalc`), `object.cpp`, `mesh.cpp`, `camera.cpp`, `display_driver.cpp`, `BObjectInfo` in `intern/cycles/blender/`; `intern/cycles/integrator/render_scheduler.cpp`; `intern/cycles/device/` (`generic_alloc`, host-mapped fallback); pbrt-v4 `src/pbrt/util/transform.h` (`AnimatedTransform`); OpenVDB Python bindings (`openvdb/openvdb/python/`, `copyToArray`; Apache-2.0); Blender (GPL, API facts only) `makesrna/intern/rna_render.cc`, `rna_depsgraph.cc`, `rna_volume.cc`, `rna_wm.cc` (`Window.modal_operators`), `python/intern/bpy_rna.cc`, `python/gpu/gpu_py_storagebuffer.cc`, `draw/engines/external/external_engine.cc`; stb_image (public domain/MIT), tinyexr (BSD-3), MikkTSpace (github.com/mmikk/MikkTSpace; v1 notes say BSD-3-Clause, confirm before use); OptiX 9.1 Programming Guide (acceleration structure build, compaction, relocation, motion).

## Review and resolution

History of the independent review; the Decision section and the Reconciliation section below supersede it where they differ (for example item 5's 1.2× threshold, now 1.5×).

Independent review, 2026-11-14: 1 blocker, 6 major, 6 minor.

1. BLOCKER, continuous orbit starves the display: ACCEPTED. Decision 25 now coalesces view changes and commits at pass boundaries and never aborts a generation's first pass; decision 34 also reads pass time and the age of the newest frame in every `view_draw`, with an immediate step-down at 83 ms; V11/V12 gain a continuous-orbit trace.
2. MAJOR, atomic commit vs pipelined submit: ACCEPTED. Commit does O(1) checks and returns the pending generation; O(n) validation runs on workers before upload; a failed generation is dropped with the previous scene kept and reported via progress and messages; decode and memory failures have defined routes (decision 9); `ar_edit_reset` added.
3. MAJOR, topology cache can show wrong geometry: ACCEPTED, with a narrower rule than proposed. The settle timer is gone; positions-only is used only for deform-only stacks with no original-mesh update, or allow-listed modal operators (decision 22); everything else is a full read; V6 extended.
4. MAJOR, catch-all full resync: ACCEPTED. Decision 20 is now an explicit per-ID-type table modelled on Cycles' `sync_recalc`, full resync only for unlisted types, each row a V4 case.
5. MAJOR, TTFF compares unlike quantities: ACCEPTED. Gate moved to an end-to-end screen-capture measure for both engines (V7a), with log-bounded phases named line by line (V7b); table recomputed (instance loop counted once, 637/514 serial worker sum, session creation outside Cycles' window); the threshold went from 1.5× to a PROVISIONAL 1.2× pending the owner.
6. MAJOR, volume transport unworkable: ACCEPTED. Decision 47 uses Blender's bundled `openvdb` Python module (present in the 5.3 install) and brick-wise `copyToArray`, so the engine needs no OpenVDB and host memory is bounded; P6 and V21 added; the axis-order conflict with 0006 is listed.
7. MAJOR, spill before trim: ACCEPTED. Decision 14 orders trim, spill, fail: F12 starts with spill disallowed while paused viewports hold scenes, trims them on out-of-memory and re-syncs once.
8. MINOR, controller counts its own scale changes as interaction: ACCEPTED. Decision 49 gives one definition (host commits and view-hash changes only), used by decisions 30 and 34.
9. MINOR, depth deferred though volumes and displacement ship in R: REJECTED in substance. True displacement is after the first release (0005), and volumes have no single depth (Cycles also leaves depth to Blender). Decision 31's wording now says so, and true displacement is in the L column.
10. MINOR, omitted scope: ACCEPTED. OpenGL backend (decision 29, V8), F12 view layers and pass registration (decision 48), the view hash from `view_draw` (decision 49).
11. MINOR, create/destroy block the main thread: ACCEPTED. Create returns at once and builds the context on the render thread, overlapping the first sync; close is non-blocking and destroy is called once CLOSED (decision 23); V22 added.
12. MINOR, three agent guesses: ACCEPTED. Keys come from a counter and a dict keyed on identity tuples, so they cannot collide (decision 18); the move map is keyed on `session_uid` (decision 19); the custom-node uuid side channel is removed and custom nodes wait for P1, matching 0005 (decision 39).
13. MINOR, POST_PIXEL handler may be unnecessary: ACCEPTED. P4 tries drawing in `view_draw` without the display-space shader first and keeps the handler only as the fallback (decision 40).

## Reconciliation (rulings applied)

- G1: "What lands when" replaced by the two lists; milestone labels removed from decisions 10, 11, 12, 13, 15, 28, 31, 35, 36, 39 and from Risks; decision 36's M/R/L table became a coverage table.
- G2: restated content replaced by citations: report (0001), errors and faults (0001 D2.5), out-of-process gate (0001 D6), packaging (0001 D10), pass readback and names (0008), grid leaf format (0006 D5), mips (0005 item 34), mask allocation and motion pipeline (0002), time budgets (0009).
- G3: no scheduled job existed in this record; none added.
- G4: kept `motion_steps`, visibility bits, link sets and `ar_edit_set_astro` because removing them would change the C ABI and path state later; "hook, not in UI until it lands" wording removed.
- Ruling 1: decision 25 latches after 0008's per-sample-buffer resolve and discards an interrupted pass's buffer; decision 12 refers resets to 0004 D25.
- Ruling 10: header `astroray.h`, `ar_context`/`ar_scene`/`ar_session` handles, every function returns `ar_status`, `ar_abi_hash` with exact match, no `struct_size` minor versioning, `_begin`/`commit` staging names, no `AR_NO_FRAME` status.
- Ruling 11: `ar_grid_desc` and decision 47 carry sparse 8³ leaves (64³ extraction chunks, as 0006 D5); V21 updated; the axis-order conflict is gone.
- Ruling 12: packaging ships our renamed `astroray_oidn*` build; the OIDN/TBB risk is closed by 0001 D9.
- Ruling 13: no loose kernel modules in the zip; kernels and tables embedded; V16 checks it.
- Ruling 14: launch sizing in decision 25 cites the 20 ms target and 100 ms cap (numbers in 0009).
- Ruling 19: decision 14 holds the only reserve rule (R = 1.5 GiB) and the VRAM budget table; 0002's and 0008's reserves defer to it.
- Ruling 20: decoders stb_image and tinyexr used only as 0001 D9.1 list entries.
- Ruling 21: TTFF gate ≤ 1.5× Cycles, goal 1.0×, PROVISIONAL, stated once in decision 35.
- Ruling 22: decision 13 builds mip chains (0005 item 34); no demand loading.
- Ruling 29: time numbers replaced by 0009 ids (decisions 5, 23, 25, 30, 34; V3 B-31, V11 B-14/B-33, V12 B-32, V22 B-21).
- Ruling 30: decision 50 states the progressive batching rule; decision 34 steps on B-14's floor and target; decision 30's publish rates moved to B-34; V23 tests batching.
- Ruling 33: `ar_edit` handle and `ar_key` cited (decision 4); device selected on `ar_context` and removed from `ar_scene_desc`; decision 5 owns the readback list (`ar_session_pass_count`, `pass_info`, `read_pass`, `read_counters`, `write_fits`); every descriptor's field list moved to `0007-appendix-abi.md` (decision 7).
- Ruling 37: decision 25 defines generation and epoch; decision 12 uses them.
- Ruling 42: decision 14's budget table adds the side buffers (shadow hit records at 84 B, shadow-pass split, geodesic and random-walk records).
- Ruling 43: `ar_grid_desc` carries the object-to-world matrix (appendix).
- Ruling 45: light and shadow linking removed from `ar_instances_desc`, `ar_light_desc`, decision 36, must-exist, can-wait and Consequences; listed as not designed.
- Ruling 49: Review section headed as history; the TTFF owner question replaced by owner decision 23 (decision 35, Risks).
- Ruling 29 (second pass): decisions 23, 26, 27, 28, 34 and 49 cite 0009 B-18 and B-37 instead of times (decision 35's TTFF estimate table kept as the gate's evidence, raised with the lead).
- Ruling 33 (second pass): decision 5 cites 0001's device and context functions; the appendix defines `ar_spectrum_ref`, carries Cryptomatte ids and the material pass index; Context no longer attributes `ar_spectrum_ref` to 0003.
- Ruling 37 (second pass): decision 25 interrupts a pass only for an epoch-starting change, whose reset clears the interrupted pass's buffers (0008 decision 3).
- Ruling 42 (second pass): decision 14's film row adds 0008's per-sample buffer.
- Ruling 49 (second pass): 0005 cited by item numbers; decision 15 states the previous-frame data 0008's motion buffer reads; decision 40 adds the Invalid and Rays statistics; decision 41 states the static camera frame 0006 D24 needs; the dangling "path_state list" and "0009 Risks g" references fixed.
