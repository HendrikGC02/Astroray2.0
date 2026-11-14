# 0007 — Blender integration: scene sync, scene representation, presentation and user interface

Status: proposed (2026-11-14), revised after independent review (same day; see "Review and resolution")

## Context

Astroray 2.0 ships as a stock Blender 5.3 extension (owner decisions 3, 14) over an engine DLL behind a C ABI that knows nothing about Blender (spine 1). The owner's viewport targets are hard: 60 updates/s desirable, 24 the floor, full resolution unless below the floor, and time to first frame (TTFF) comparable to Cycles including on large scenes, where v1 blew up (decisions 9, 10). The first release must carry the full breadth: surfaces, volumes, sky and world, textures, lights, gratings (decisions 7, 17), and the black hole in a nebula follows with its hooks present from the first milestone (spine, release structure).

What the addon cannot change is measured. Python reads float attributes with exact dtypes at memcpy speed but integer topology at ~25 ns/element and `loop_triangles` at 50-120 ns/triangle; Blender owns a ~6 ms floor per redraw, and during navigation `view_draw` is called at that cadence; one `GPUStorageBuf.update` above 32 MiB falls off a 10x cliff; adding one object to a 10k-object scene costs Blender 285 ms and yields 9,007 update entries; there is no zero-copy display route for an addon; Python has no voxel read API (spike B sections 1-2; research 02 F2-F4; research 07). With the fast read path and native triangulation, mesh+instance sync plus OptiX GAS/TLAS build is 1.42 s serial on 5M triangles / 10k instances (spike B section 3), without shaders, textures or lights.

v1's integration failed on five primitives: build identity not observable, sync pushed device state without re-reading Blender, UI wired to dead paths, a re-derived camera, and process-global GPU state that forced admission tokens and pause gates (`v1-lessons/research/blender.md`). This record fixes the boundary, the scene model, sync, threading, presentation and UI so that none of those can recur, and so that Sonnet-class agents can implement each piece from a small spec.

Assumed of the parallel records: 0001 DLL + CLI host + extension, Windows only, CPU device selectable. 0002 OptiX pipelines with a runtime-selected motion variant, launches bounded to ≤ 20 ms, per-pass timing available to the host, module cache directory supplied by the host, texture fetch that works from pitch-linear host-mapped memory. 0003 `ar_spectrum_ref` and working-space primaries. 0004 generation-based progressive accumulation, a light tree per light-link set, emissive-mesh lights, environment-map rebuild rules. 0005 owns graph semantics: the addon translates inlined Blender nodes into 0005's host-neutral op graph (0005 D29); material kinds are authored on the Material panel (0005 D16); true displacement and custom nodes come after the first release. 0006 volume and astro-object representation, unit scale, NanoVDB built natively from dense blocks with no OpenVDB in the engine (0006 D5). 0008 film-to-display conversion into half4, the pass list, denoiser and DLSS modes, science outputs. 0009 a `/MT` engine DLL, header-generated bindings, the CI matrix and the GPU test machine.

## Decision

Numbers are tagged MEASURED (with source) or PROVISIONAL (a budget to be confirmed by the named check). Decision numbers are stable because other records cite them; items added in review take the next free number (47-53) inside their section.

### A. Process and registration

1. **In-process.** A Python `RenderEngine` subclass `AstrorayRenderEngine` (`bl_idname = "ASTRORAY"`) loads `astroray_core.dll` with `ctypes.CDLL(full_path)` (releases the GIL per call; research 08). No Python threads anywhere in the addon. An out-of-process daemon is not built; the ABI below crosses only flat arrays and PODs, so it can be moved behind shared memory if the gate in decision 44 trips.
2. **Engine flags:** `bl_use_shading_nodes_custom = False` (expose Blender's nodes), `bl_use_preview = True`, `bl_use_eevee_viewport = True` (Material Preview shading uses EEVEE, as Cycles), `bl_use_postprocess = True`, `bl_use_gpu_context = False`, `bl_write_viewport_depth = False` (decision 31).
3. **Build identity.** At `register()` the addon compares `ar_abi_version()` major and the `ar_abi_layout()` table (sizeof/offsetof of every ABI struct) with the generated Python bindings, and prints `ar_build_id()`. Any mismatch fails registration with a message naming the field. This replaces v1's stale-module failure mode.

### B. The C ABI (design level; header `astroray_core.h`, Python bindings generated from it)

4. **Conventions.** Every function returns `ar_status` (0 OK, negative error; `ar_last_error(session)` gives UTF-8 text). Every descriptor starts with `uint32_t struct_size` so minor versions append fields. No callbacks into Python (v1 deadlocked on a GIL-acquiring callback); the host pulls progress and messages. No process-global mutable state (CLAUDE rule 8): everything hangs off an `ar_session`. Keys are `ar_key = uint64_t` assigned by the host (decision 18).
5. **Function list.** Every call returns within the main-thread budget of decision 23; slow work (context creation, joins, validation, decode, builds) runs on session threads and reports through `ar_session_get_progress` and `ar_session_poll_message`.

```c
uint32_t ar_abi_version(void); const char* ar_build_id(void);
ar_status ar_abi_layout(const ar_layout_entry** table, uint32_t* n);
/* session: one per RenderEngine instance (viewport region, F12 job, preview) */
ar_status ar_session_create(const ar_session_desc*, ar_session**); /* returns at once in INITIALIZING; device CUDA|CPU, index, mode, cache_dir, worker_threads */
ar_status ar_session_close(ar_session*);    /* non-blocking: cancel, stop workers at the next job boundary, free device memory */
void      ar_session_destroy(ar_session*);  /* frees the handle; blocks only if called before progress reports CLOSED */
ar_status ar_session_start(ar_session*);  ar_status ar_session_pause(ar_session*);
ar_status ar_session_resume(ar_session*); ar_status ar_session_cancel(ar_session*);
ar_status ar_session_set_view(ar_session*, const ar_view_desc*);   /* records the pending view; latched at the next pass boundary (25) */
ar_status ar_session_set_display_mode(ar_session*, uint32_t mode); /* BEAUTY or a heat-map buffer (decision 40) */
ar_status ar_session_get_progress(ar_session*, ar_progress*);      /* state, active/failed generation, spp, pass ms, ledger, device free/total */
ar_status ar_session_wait(ar_session*, uint32_t timeout_ms);       /* F12 job thread only */
ar_status ar_session_trim(ar_session*);                            /* paused session frees its device scene; state TRIMMED */
ar_status ar_session_poll_message(ar_session*, ar_message*);       /* level, code, generation, object key, text */
/* scene edit transaction: one open edit per session; Blender main thread, or update() for F12 */
ar_status ar_edit_begin(ar_session*, ar_edit**);
ar_status ar_edit_reset(ar_edit*);                  /* first call only: the edit replaces the whole scene */
ar_status ar_edit_alloc_mesh(ar_edit*, const ar_mesh_desc*, ar_mesh_buffers* out); /* host fills, then submit */
ar_status ar_edit_alloc_positions(ar_edit*, ar_key geom, float** xyz);             /* deformation fast path */
ar_status ar_edit_alloc_curves(ar_edit*, const ar_curves_desc*, ar_curves_buffers* out);
ar_status ar_edit_alloc_points(ar_edit*, const ar_points_desc*, ar_points_buffers* out);
ar_status ar_edit_alloc_grid_block(ar_edit*, const ar_grid_desc*, float** voxels); /* one dense brick of one grid (decision 47) */
ar_status ar_edit_alloc_image(ar_edit*, const ar_image_desc*, void** pixels);      /* source PIXELS only */
ar_status ar_edit_set_image(ar_edit*, const ar_image_desc*);                       /* source FILE or MEMORY */
ar_status ar_edit_submit(ar_edit*, ar_key);         /* buffers go to the session's workers at once */
ar_status ar_edit_remove(ar_edit*, uint32_t kind, ar_key);
ar_status ar_edit_set_instances(ar_edit*, const ar_instances_desc*);              /* full replace */
ar_status ar_edit_update_transforms(ar_edit*, uint32_t n, const uint32_t* idx, const float* xf3x4);
ar_status ar_edit_set_graph(ar_edit*, ar_key, uint32_t target, const ar_graph_desc*); /* SURFACE|VOLUME|DISPLACEMENT|LIGHT|WORLD */
ar_status ar_edit_set_material(ar_edit*, ar_key, const ar_material_desc*);        /* graph keys + kind + settings */
ar_status ar_edit_set_light(ar_edit*, ar_key, const ar_light_desc*);
ar_status ar_edit_set_world(ar_edit*, const ar_world_desc*);
ar_status ar_edit_set_camera(ar_edit*, const ar_camera_desc*);                    /* final-render camera */
ar_status ar_edit_set_settings(ar_edit*, const ar_settings*);                     /* includes allow_spill (decision 14) */
ar_status ar_edit_set_astro(ar_edit*, ar_key, const ar_astro_desc*);  /* hook for 0006; not in UI until it lands */
ar_status ar_edit_commit(ar_edit*, uint64_t* generation); /* O(1) checks, returns at once; outcome by generation (9) */
ar_status ar_edit_abort(ar_edit*);
/* display and output */
ar_status ar_display_acquire(ar_session*, uint64_t after_frame, ar_display_frame* out); /* AR_NO_FRAME if none newer */
ar_status ar_display_release(ar_session*, uint64_t frame_id);
ar_status ar_pass_read(ar_session*, uint32_t pass_id, float* dst, uint64_t count);
```

6. **Zero-copy staging.** `ar_edit_alloc_*` returns pointers into a session-owned host arena; Python wraps them as numpy views (`np.ctypeslib.as_array`) and `foreach_get` writes straight into them (research 01 D2), then `ar_edit_submit` hands them to workers. The arena grows in 64 MiB chunks pre-faulted by a worker thread ahead of use, which removes most of the cold first-touch cost (spike B: native copy 146 ms cold vs 23 ms warm on S2). Views are deleted by the Python wrapper at submit; arena chunks are recycled only after their jobs finish.
7. **Data layouts.** Matrices are 3x4 row-major float32 object-to-world.

| Descriptor | Fields (host-filled buffers in brackets) |
|---|---|
| `ar_mesh_desc` | key, name, V, C (corners), F, uv_layer_count U, motion_steps S (1 unless deformation blur), flags (HAS_MATERIAL_INDEX, HAS_ATTRIBUTES), attribute list (name, domain POINT/CORNER/FACE, float width 1-4); [positions f32 S×V×3, corner_vert i32 C, face_start i32 F, corner_normals f32 C×3, uv f32 U×C×2, material_index i32 F, attributes f32] |
| `ar_instances_desc` | N, motion_steps S; [geom_key u64, xf f32 S×12, object_key u64, random_id u32, pass_index u32, color f32×4, visibility u32 (camera/shadow/diffuse/glossy/transmission/volume/holdout/shadow-catcher bits), light_set u16, shadow_set u16, material_table u32 offset]; plus pool [material keys u64] |
| `ar_curves_desc` | key, P points, K curves, type (linear/Catmull-Rom/B-spline); [positions f32 P×3, radius f32 P, curve_start i32 K, attributes] |
| `ar_points_desc` | key, P; [positions f32 P×3, radius f32 P, attributes] |
| `ar_grid_desc` | key, grid name, grid bbox min/max ijk, block origin ijk, block dims (x,y,z), axis order X_MAJOR, index-to-object 3×4, channels 1/3, background value, block index and block count; [voxels f32 dense over the block] (representation inside the engine: 0006) |
| `ar_image_desc` | key, source FILE (UTF-8 path) / MEMORY (packed bytes) / PIXELS (f32 RGBA), colour interop id, alpha mode, UDIM tile number, width, height |
| `ar_light_desc` | type POINT/SPOT/SUN/AREA_RECT/AREA_ELLIPSE, xf, size_x, size_y, spot_size, spot_blend, sun_angle, spread, power (W, or W/m² for sun, exposure applied), colour as `ar_spectrum_ref` (0003; RGB or blackbody), normalize, visibility, cast_shadow, graph key, light/shadow-link sets |
| `ar_camera_desc` / `ar_view_desc` | type PERSP/ORTHO/PANO, cam_to_world, projection_inverse 4×4 (NDC to camera space, taken from Blender), near/far, DOF (fstop, focus distance, blades, rotation, ratio), panorama params, shutter open/close + motion steps; view adds region w/h, render scale, border |
| `ar_display_frame` | frame_id, generation, width, height, format (RGBA16F or DIAG_RGBA8), row bytes, data pointer (pinned host ring slot, valid until release) |

8. **Graph transport.** Python walks the tree returned by `inline_shader_nodes()` and translates each Blender node into 0005's host-neutral ops (the Blender-to-engine mapping lives in the GPL addon, 0005 D29; the core sees no Blender node types, spine 1). The result crosses as flat arrays (`ar_graph_desc`: op nodes {op u16, param range}, param pools {f32, i32, UTF-8 strings}, links {from node, socket, to node, socket}); the core compiler of 0005 lowers it to VM bytecode and returns its error list as messages. The compiler therefore runs from the CLI host with no Blender, and Python never emits bytecode.
9. **Validation and edit outcome.** Atomicity is per generation, not per call. `ar_edit_commit` performs only O(1) checks synchronously (struct sizes, enum ranges, known keys, filled counts equal allocated counts) and returns the generation the edit will produce. The O(n) checks (`corner_vert < V`, monotonic `face_start`, finite positions, grid dims, about 1 ms per 1M corners, PROVISIONAL) run on the worker that triangulates or converts that item, before its upload. The engine keeps rendering the previous generation; the new one becomes active only when every job of the commit has succeeded, and replaced device objects are freed only then (peak memory: old plus new for the edited keys). Outcomes:
   - validation failure: the whole generation is dropped, the previous scene stays on screen, `ar_progress.failed_generation` is set and an ERROR message names the datablock; new device objects of that edit are freed, so nothing half-valid is ever rendered;
   - image decode failure: that image becomes a magenta texture with a report entry (an unreadable input is reported, never silently skipped);
   - out of device memory: decision 14.
   The addon reads outcomes in `view_draw` or the timer of decision 26 and shows them in the report (decision 38); F12 `render()` fails the render with the message.

### C. Engine scene representation

10. **Geometry.** One record per geometry key: triangulated mesh (triangle → three corner indices; vertex, normal, UV and attribute lookups go through the corner), curves (OptiX curve primitives), points (OptiX spheres), volume grids (0006). Triangulation is native: triangles copied, quads split by Blender's rule (determined in M1 by a golden test against `loop_triangles` and then implemented identically), n-gons of 5+ by ear clipping on the polygon projected along its Newell normal (Eberly 2002). The spike's fan triangulation is wrong for concave n-gons and is not used. Tangents (MikkTSpace, Mikkelsen 2008) are computed only for geometry whose materials request them.
11. **Acceleration structures.** One compacted OptiX GAS per geometry key (PREFER_FAST_TRACE | ALLOW_COMPACTION), built in batches from one temp arena with one stream sync per batch; one TLAS over all instances, always rebuilt, never refit (MEASURED: TLAS 4.9 ms for 10k instances, GAS 2.9 ms per 1M triangles including compaction, spike B 2.1). GAS are not refit either: a rebuild of the edited geometry costs less than a frame and keeps trace quality (v1 noted refit degrades). GAS live for the session's lifetime and are reused across edits; no disk cache in the first release (decision 35 says when to add one). CPU target: Embree with the same per-geometry/instance structure.
12. **Edit → work map** (every row resets accumulation through a new generation except selection):

| Edit | Host call | Engine work | Acceleration structure |
|---|---|---|---|
| viewport camera / region | `set_view` | none | none |
| object transform (non-instancer) | `update_transforms` | patch instance buffer | TLAS rebuild |
| instancer moved, object added/removed, collection toggled | `set_instances` (+ new geometry) | rebuild instance and material-table buffers | new GAS only for new keys; TLAS rebuild |
| deformation, topology provably unchanged (22) | `alloc_positions` (+ corner normals) | upload positions/normals | GAS rebuild of that key + TLAS |
| any other geometry change | `alloc_mesh` | re-triangulate, upload | GAS rebuild + compaction + TLAS |
| material value or graph | `set_graph` / `set_material` | recompile program, upload | none (true displacement, after R: geometry rebuild) |
| image changed | `set_image` | decode, upload | none |
| volume grid changed | `alloc_grid_block` × blocks | NanoVDB build, majorant grid (0006) | none |
| light / world | `set_light` / `set_world` | light tree or environment map rebuild (0004), only when emission inputs change | none |
| settings | `set_settings` | per field; resolution reallocates film and path state | none |
| full resync, trimmed resume, F12 view layer, preview | `reset` + everything | replace scene | all |
| selection only | nothing | nothing | none |

13. **Textures.** Stored at source precision (8-bit, half or float; 1, 3 or 4 channels), decoded on the session's workers with stb_image (PNG, JPEG, TGA, BMP, HDR) and tinyexr (EXR); colour conversion to the working space by interop id (decision 21). No mip/demand loading in the first release.
14. **VRAM budget and overflow.** Every device allocation goes through a per-session ledger with categories {geometry, acceleration, textures, volumes, film, path state, denoiser, display}; the ledger and the device's free/total bytes are reported in the render panel and in `ar_progress`. Before each large allocation the engine checks `cuMemGetInfo` against need plus a reserve for Blender and the desktop of 1.5 GiB (PROVISIONAL, measured in M1 with Blender open). Order when the budget is exceeded: **trim, then spill, then fail.**
   - Trim: a session cannot free another (no process-global state), so the addon owns this step. An F12 session is created with `allow_spill = false` while any viewport session on the same device is paused and still holds a device scene. If its commit fails with out-of-memory, the addon trims those paused sessions (`ar_session_trim`), sets `allow_spill = true` and re-syncs once with `ar_edit_reset`. A trimmed viewport does a full sync when it resumes.
   - Spill: with `allow_spill` set, textures and volume grids move to host-mapped pinned memory (Cycles' fallback, `generic_alloc` in `intern/cycles/device/`, file to confirm on reading; survey.md item 4) and the render report names what moved. Viewport sessions start with `allow_spill = true`.
   - Fail: acceleration structures, geometry and path state never spill; if they do not fit, the session fails loudly with "needs X GiB for geometry, Y available" and Blender stays alive. Never a silent downscale.
   Fixed per-session cost PROVISIONAL ≤ 1.0 GiB at 1080p and ≤ 2.5 GiB at 4K (path state estimate 270-350 MB at 1080p, research 04; DLSS 154-579 MB, research 10).
15. **Motion and deformation hooks.** Instances and meshes carry `motion_steps` from M1 (S = 1 costs one u32 per descriptor); rays carry time (spine 3). Camera and object (transform) motion blur in final renders: transforms sampled with `engine.frame_set(frame, subframe)` in `update()`, interpolated with T/R/S decomposition and slerp (Shoemake 1985; pbrt-v4 `AnimatedTransform`), built as an OptiX motion TLAS; the motion pipeline variant is selected at runtime only when the scene has motion (assumption on 0002). Deformation blur fills S position steps and a motion GAS; it lands after the first release, and until then an object with deformation and motion blur enabled renders unblurred with a report entry. Viewport never blurs.

### D. Python sync

16. **Plain functions.** Sync lives in `astroray/sync/` as functions taking a depsgraph, an `ar_edit` wrapper and a cache object, unit-tested headlessly with `pip install bpy==5.3.0` (a `RenderEngine` cannot be instantiated outside Blender; v1 lesson).
17. **Mesh read** (spike B 2.2): `attributes['position']` float32, `attributes['.corner_vert']` int32 (falls back to `loops.foreach_get('vertex_index')` with a console warning and a report entry if the internal name disappears), `polygons.foreach_get('loop_start')`, `corner_normals` float32, UV and requested attributes float32 by attribute, `material_index` only if the attribute exists. Never `loop_triangles`, never per-element RNA, never a float64 destination. Curves, text, surfaces and metaballs go through `to_mesh()`/`to_mesh_clear()` on the evaluated object. Hair `Curves` read `position` and `radius` (float) and curve offsets.
18. **Keys.** Identity of a geometry: the original data's `session_uid` when the evaluated data is that datablock's shared evaluated copy (linked duplicates share one GAS); otherwise (modifiers or geometry nodes produced per-object geometry) the tuple (original object `session_uid`, instance `persistent_id[]`). This is Cycles' `BObjectInfo::is_real_object_data()` rule (`intern/cycles/blender/`, read before implementing). Never the evaluated mesh's own `session_uid` (spike B: 2 values for 2,001 meshes). Object identity = original object `session_uid` (+ `persistent_id[]` for instances). The addon maps each identity tuple to an `ar_key` from a per-session counter through a dict; there is no hashing, so keys cannot collide.
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

21. **Images without Python lists.** FILE source when the image is file-backed, not packed and not dirty: the engine reads the resolved path (`filepath_from_user()`, UDIM tiles from `image.tiles`). MEMORY source for packed files: `packed_file.data` passed by buffer protocol. PIXELS source for generated, dirty, movie or engine-undecodable images: `image.pixels.foreach_get` into an arena buffer. Colour space by `colorspace_settings.interop_id` (new in 5.3); the engine supports the ids that are matrices plus transfer functions (sRGB and linear Rec.709, Rec.2020, ACEScg, ACES2065-1, Display P3, Non-Color); any other id renders magenta with a report entry. Engine image cache per session, keyed on (path or packed hash, interop id, alpha mode, tile, file size and mtime, reload counter) (v1 lesson).
22. **Topology cache: only when topology provably cannot have changed.** Per geometry key Python keeps (V, C, F). A geometry update sends positions and corner normals only (MEASURED 1.1 ms for 1M triangles) in exactly two cases: (a) the object's modifier stack is empty or deform-only (a fixed list of Blender's deform-only modifier types: Armature, Cast, Curve, Displace, Hook, Laplacian Deform, Lattice, Mesh Deform, Shrinkwrap, Simple Deform, Smooth, Corrective Smooth, Laplacian Smooth, Surface Deform, Warp, Wave) and the batch has no geometry update on the original Mesh: armature, shape-key and lattice playback; (b) the only modal operator running in `context.window.modal_operators` is on an allow-list that cannot change topology (the transform family; sculpt brush strokes with dynamic topology off). Every other geometry update is a full read (MEASURED 65 ms per 1M triangles): edit-mode operators such as edge rotate, flip normals or sort elements, geometry nodes and generative modifiers, frame changes on non-deform stacks. If counts differ on the fast path the addon does a full read and adds a report entry (an allow-list error is visible). Unknown cases fall to the full read, which costs time but is never wrong. F12 always does a full read.
47. **Volume grids.** Blender 5.3 bundles the OpenVDB Python module (`5.3/python/lib/site-packages/openvdb.cp313-win_amd64.pyd`, present in the local install, 2026-11-14), so voxels leave Blender without OpenVDB in our build or our zip. Per Volume object in `view_update`/`update()`: if it is file-backed with no modifiers and frame-independent, Python opens the resolved `.vdb` path with `openvdb.readAll`; otherwise it writes the evaluated grids with `VolumeGrids.save` to the session's temporary directory and reads that file. For each grid used by the material it takes `evalActiveVoxelBoundingBox()` and copies the bounding box in bricks of at most 128³ voxels (8 MiB at one channel) with `copyToArray` straight into `ar_edit_alloc_grid_block` arena views. The engine builds NanoVDB from the bricks and drops all-background leaves, so device memory stays sparse; host memory is bounded by the bricks in flight, not by the grid; time grows with the bounding-box volume (PROVISIONAL until P6). Grid names map to 0006's density/temperature/colour/velocity/ionization slots by the material's Volume Info and Attribute nodes; a grid type other than float or vec3 is reported and not rendered.

### E. Threading, generations, cancellation, coexistence

23. **Who runs where.** Blender main thread: `view_update`, `view_draw`, panels, all `bpy` reads. F12 job thread: `update()` (all reads, under Blender's draw lock) and `render()` (no scene reads; polls progress). Per session, native only: one render thread (creates the CUDA/OptiX context, owns its streams and launches) and a worker pool of min(8, logical cores / 2) threads for validation, triangulation, decode, uploads and GAS builds. No Python thread, and no engine call blocks the main thread for more than 5 ms except copying data the host itself supplied (PROVISIONAL). Session creation therefore returns at once: context creation (85 ms OptiX, 0-300 ms CUDA if first in the process) runs on the render thread while Python already stages the first sync. Destruction is two steps: `RenderEngine.__del__` calls `ar_session_close` and parks the handle; the timer of decision 26 calls `ar_session_destroy` once progress reports CLOSED. Addon unregister and Blender exit call `ar_session_destroy` directly and may block.
24. **Pipelined sync.** Workers start validating, triangulating, uploading and building a key's GAS as soon as it is submitted, while Python keeps reading the next mesh; the render thread activates the commit's generation when every job of that commit has succeeded (decision 9).
25. **Generations, coalesced at pass boundaries.** A pass is one sample per pixel at the current render scale, split into launches of ≤ 20 ms (PROVISIONAL; also keeps away from the Windows TDR). `set_view` and `commit` only record pending state. At each pass boundary the render thread latches all pending state (every view change and every commit whose jobs have finished since the last latch) into one new generation. A pending change interrupts the current pass at the next launch boundary only if the current generation has already presented a frame; otherwise the pass completes and is presented first. During a continuous orbit (a `view_draw` about every 6 ms, spike B 1.3) the engine therefore presents one pass for the newest view each time a pass completes, instead of restarting forever; latency after a change is at most the rest of the pass in flight plus one pass plus present. Idle accumulation still stops within one launch. Every display frame carries its generation; `view_draw` uploads a frame only if its generation is ≥ the last presented one and keeps showing the old frame until the new generation has one (no black flash, no late stale frame).
26. **Redraw loop.** `view_draw` calls `tag_redraw()` at its end while the session is rendering or has pending async work; when converged it stops. Asynchronous completions after convergence (commit outcomes, image decodes, sessions closing) are picked up by a `bpy.app.timers` poll at 50 ms that exists only while such work is pending. No Blender call is ever made from a non-main thread (spike B saw `tag_redraw` from a thread work but established no thread safety).
27. **Cancellation.** `render()` loops on `ar_session_wait(100 ms)`, calls `update_stats`/`update_progress`, posts a preview with `update_result` every 1 s, and calls `ar_session_cancel` when `test_break()` is true; cancel stops at the next launch boundary. Partial F12 results are returned on cancel, as Cycles does.
28. **Coexistence.** Each viewport region, each F12 render and the material preview have independent sessions; nothing is shared (two rendered viewports cost two scene copies; accepted). Viewport sessions pause in `view_pause` and resume in `view_resume` when a final render runs (5.3 Cycles behaviour; M1 probe P2 verifies Blender calls them on F12, and if not, the addon pauses its weakly-referenced viewport sessions from `update()`). Memory pressure between them follows decision 14 (trim, then spill, then fail). Material previews reuse one long-lived addon-held PREVIEW session behind a Python lock, each preview starting with `ar_edit_reset`, so a thumbnail does not pay OptiX context creation (~85 ms, spike A section 5) each time. `scene.render.use_persistent_data` keeps the F12 session across animation frames with delta sync.
48. **View layers and passes in F12.** Blender calls `update()` and `render()` once per view layer to render. One F12 session serves the job; each layer's `update()` begins with `ar_edit_reset` and syncs that layer's depsgraph, then `render()` writes the layer's result. `update_render_passes` registers every enabled pass of 0008's list with `register_pass` so compositor sockets exist; `render()` writes the same passes into the result. Pass names come from one table shared with 0008 (they are a known 5.4 fragile point, decision 46).

### F. Presentation

29. **Path.** The engine converts the film to scene-linear RGB in the file's working space (spine 6; conversion owned by 0008), clamps to the finite half range (±65504), writes RGBA16F into a 3-slot pinned host ring with an async copy on a copy stream. `view_draw` acquires the newest frame, views it as `uint32` pairs, updates persistent `GPUStorageBuf` stripes, dispatches a 16×16 compute shader (`unpackHalf2x16`) into one persistent RGBA16F `GPUTexture`, and draws it with `draw_texture_2d` inside `bind_display_space_shader`/`unbind_display_space_shader` with `ALPHA_PREMULT` blending (spike B 1.4: no barrier needed, Blender applies the view transform afterwards). The code uses only Blender's `gpu` module, so the same path serves the Vulkan default and the OpenGL backend a user can select (research 01 recommendation 6); only Vulkan was measured, and V8 runs on both.
30. **Formats and the 32 MiB rule.** One format at every resolution: RGBA16F, split into stripes of at most 16 MiB each, each its own SSBO (MEASURED: 1080p is one 15.8 MiB update at 0.74 ms and holds 60 and 120 updates/s; 4K as four 16 MiB stripes held 60.1/s at a 60 Hz producer and 77.5/s at 120, spike B 1.1-1.2; 1440p as two stripes is interpolated, not measured). No 8-bit display-referred present (double view transform). SSBOs and texture are reallocated only when size changes; up to two sizes are cached for the resolution controller. Frames are published at ≤ 60 Hz during interaction and ≤ 10 Hz otherwise, with interaction as defined in decision 49.
31. **Depth for overlays.** Without `bl_write_viewport_depth` Blender runs its own depth prepass of the scene geometry (research 01 B4), which is correct for every rendered surface in the first release. Volumes have no single depth, so overlays behind a volume show through it, as they do under Cycles, which also leaves depth to Blender. The flag is switched on when true displacement (0005: after the first release) or curved rays (the milestone after it) make the engine's surfaces differ from rasterised geometry; it then uses a custom fragment shader writing `gl_FragDepth` from the engine's linear depth and Blender's window matrix. Cost when unused: none.
32. **Colour management.** No engine tonemapping in the beauty path; Blender's view transform, look, exposure and gamma apply (verified by V9).
33. **Presentation budget.** `view_draw` ≤ 2 ms at 1080p and ≤ 8 ms at 4K (MEASURED 0.8 ms and 7.4 ms paced); the ~6 ms Blender floor caps updates at ~1/(6 ms + view_draw).
49. **View changes and interaction.** Orbit and pan do not arrive through `depsgraph.updates`; they only cause `view_draw` (research 01 B8). `view_draw` hashes (view matrix, projection, region size, camera-view state, border) and calls `set_view` only when the hash changes. *Interaction* means a commit or a `set_view` whose hash changed within the last 250 ms; render-scale changes made by the controller do not count, so the controller cannot keep itself in interaction. The controller (decision 34), the publish rate (decision 30) and the return to full resolution use this one definition.

### G. Dynamic-resolution controller

34. Scale levels (linear) {1, 3/4, 1/2, 3/8, 1/4} (PROVISIONAL). Active only during interaction (decision 49). Inputs, sampled in every `view_draw` (about every 6 ms while navigating): the engine's per-pass time at the current scale from `ar_progress` (0002) and the age of the newest presented frame. Step down one level after 3 consecutive passes whose exponential moving average (α = 0.3) is above 41.7 ms (the owner's 24/s floor, decision 10), or at once when the newest presented frame is older than 83 ms (twice the floor) during interaction, so that the controller acts even when no frame arrives. Step up when the average scaled by the next level's pixel ratio predicts ≤ 33.3 ms (30/s) for 0.5 s. When interaction stops, return to scale 1 and accumulate at full resolution. A reduced frame is drawn stretched with linear filtering. With DLSS enabled (0008) the levels map to DLSS modes and the present is full resolution; a mode change recreates the DLSS feature (DLSS-RR does not support dynamic resolution, research 10), cost to be measured. The structure follows Cycles' resolution divider (`intern/cycles/integrator/render_scheduler.cpp`, `calculate_resolution_divider_for_time`), but the trigger is the owner's floor, not a fixed navigation divider; decision 25's pass-boundary coalescing is what keeps frames arriving during navigation.

### H. Time-to-first-frame budget (viewport, 1080p, 1 spp)

35. **Two measurements, bounded the same way for both engines.** V7a (the gate, user-perceived): wall time from a timer callback that switches the region to Rendered shading to the first frame showing engine pixels, detected by an external screen-capture poller of the region at ≥ 100 Hz (spike B's `CopyFromScreen` approach, own PID; resolution ≤ 20 ms), same harness for Cycles and Astroray, on S1, S2 and a textured scene S3, both for the first switch after opening the file and for a second switch. V7b (diagnostic, from logs, excludes session and device creation for both): Cycles = timestamp of the first `Rendered N samples` line minus (timestamp of the `Total time spent synchronizing data: T` line minus T); Astroray = first-frame-presented log line minus first `ar_edit_begin` log line. Gate on V7a: ≤ 1.2× Cycles (PROVISIONAL: the owner's "comparable" needs a number, see open items); goal ≤ 1.0×. If S2 fails after M1, the remedies in order are: `as_pointer()` topology reads (removes ~330 ms, version-fragile), a cross-session GAS disk cache (`optixAccelRelocate`), parallel material inlining moved earlier.

V7b estimate for S2 (5.04M triangles, 10k instances, 300 materials). Cold = first switch in a fresh process after the file load (spike B's 3 fresh-process runs); warm = spike B's warm minimum.

| Phase | Cycles viewport (log, `cycview_summary.txt`) | Astroray | Source |
|---|---|---|---|
| Blender depsgraph evaluation | same | same | excluded from both |
| Session and device creation | before the log window, not measured | 85 ms + 0-300 ms on the render thread, hidden behind the reads (decision 23) | A §5 MEASURED + ESTIMATE |
| Data sync on the main thread | 57 ms (C++ sync, log line) | instance loop 30 + mesh reads 752 cold / 366 warm | B 2.1: 928 − 146 − 30 cold; 419 − 23 − 30 warm |
| Materials | "307 shaders" (inside Cycles' window) | 90-300 ms inline + op walk on the main thread; compile on workers | NOT MEASURED |
| Lights (≤ 100) | | < 5 ms main thread | estimate |
| Triangulate + copy, workers | | 146 cold / 23 warm, overlapped with the reads | B 2.1 |
| GAS + compaction + TLAS + upload, workers/GPU | ~906 ms to `Using OPTIX layout` (BVH dominates; ~950 ms in the F12 log) | 491 cold, overlapped; workers' serial total with the row above 637 cold / 514 warm | B 2.1 |
| OptiX modules (disk cache warm) | | ~1 ms | A; cold ~1 s per module once |
| Tail after the last read | | ≤ 100 ms (last GAS batch + TLAS) | PROVISIONAL |
| Device sync, first sample, present | ~62 ms | 10-20 ms + 0.8 ms + 0-6 ms wait | B + estimate |
| **Total from sync start** | **1,027 ms** (57 + 970) | cold: main thread 877-1,087 + tail + first frame ≈ 1.0-1.21 s (0.97-1.18×); warm: max(main thread 491-701, workers 514) + tail + first frame ≈ 0.63-0.82 s (0.6-0.8×) | |

S1 on the same basis: Astroray 98 ms serial (89 sync incl. native copy, 9 OptiX; spike B 3) against Cycles 132 ms (7 + 125): 0.74×, before materials. The upper end of S2 cold comes from the two unmeasured rows (material inlining, and whether the context really hides behind the reads); V7a also includes Cycles' session and device creation, which no spike measured. M1 measures all of them; if S2 is over the gate, the remedies above are applied in order before any feature work that adds sync cost.

### I. Feature surface order

36. M = first milestone, R = before first release, L = after first release.

| Area | M | R | L |
|---|---|---|---|
| Objects | meshes (incl. curve, text, surface, metaball via `to_mesh`), all instance kinds (linked duplicates, collection instances, geometry nodes) | volumes (VDB-backed `Volume` objects by decision 47, mesh-domain media, world volume), hair `Curves`, point clouds, holdout/shadow catcher, light and shadow linking, camera+object motion blur, persistent data, multiple view layers | deformation blur, true displacement (0005), Gaussian splats, legacy particle hair, bake |
| Lights | point, spot, sun, area (all shapes), blackbody temperature | light node trees, IES, emissive meshes (0004), spectrum modes | portals |
| World | constant and node-graph world via VM | HDRI importance sampling, Sky (multiple scattering) | Preetham/Hosek |
| Cameras | perspective, orthographic, DOF, viewport and F12 | panoramic types, shift/sensor-fit edge cases | stereo, custom camera |
| Passes | Combined, Depth, Normal, Albedo, sample count, bounce count | 0008's full list incl. Cryptomatte, AOV, light groups, science buffers | |
| UI | engine panels, report, heat maps | Material panel kinds (0005 D16), astro object panel, asset library, science export | custom shader nodes (decision 39) |

### J. User interface

37. **Panels.** Built-in Blender panels whose properties the engine honours get `"ASTRORAY"` added to their `COMPAT_ENGINES` (camera lens and DOF, output, colour management, film transparency); a list in `ui/compat_panels.py` is the only place this happens. Engine-specific settings live in Astroray PropertyGroups (`scene.astroray`, `object.astroray`, `light.astroray`, `material.astroray`, `world.astroray`) with Cycles-like layout and names where the meaning matches. Native datablock properties (light energy and size, ray visibility `Object.visible_*`, `render.film_transparent`, `render.use_motion_blur`) are read directly, never mirrored. Every property has a tooltip with units, and a test that changing it changes a render or greys it out with an INFO line (V17).
38. **Report.** Unsupported nodes render magenta (lead default) and are listed once per render in a Diagnostics subpanel and via `self.report`; the same channel lists rejected edits, spills, trims, fallbacks and full-resync counts.
39. **Custom material types and nodes** (owner decision 16). In the first release a material's extension kind and its constants are authored on the Material's Astroray panel (`material.astroray`, 0005 D16), which works whatever the inliner does. Custom shader nodes (`ShaderNodeCustomGroup` subclasses with a Cycles/EEVEE approximation inside, research 17) come after the first release and only if probe P1 (0005's M-7) shows `inline_shader_nodes()` keeps their identity; no side channel through the raw tree is built.
40. **Heat-map view modes** (owner decision 15): `scene.astroray.display_mode` ∈ {Beauty, Sample count, Bounce count, Relative error, Termination reason, Time per pixel, Albedo, Normal, Depth}. The engine colour-maps the chosen buffer (viridis for scalars, Smith and van der Walt 2015; a fixed categorical palette for termination reason) into an RGBA8 sRGB frame with its legend, drawn so the user's view transform cannot distort it (verified by V10). Probe P4 tries the simpler route first: draw it in `view_draw` without `bind_display_space_shader`; if Blender still applies the view transform there, a `SpaceView3D` POST_PIXEL handler draws it instead.
41. **Astrophysics objects.** Black holes are Empty objects (sphere display at the horizon radius) with `object.astroray.astro_kind` and parameters, created by an Add > Astroray menu; sync sends `ar_edit_set_astro` with transform and parameters in the declared unit scale (research 06 R3; contents 0006). The nebula is a Volume object or mesh domain with Astroray emission settings.
42. **Asset library** of spectral materials, lamps and gratings as one `.blend` in the extension; an operator in the addon preferences, run by the user, copies it to `extension_path_user` and registers it as an asset library (never automatic: it changes user preferences).
43. **Science export**: an Output-properties subpanel enabling science outputs (FITS cube, photon-count buffers) written by the engine next to the render; contents in 0008. **What an addon cannot do**: add sockets to native nodes (spectra enter through the Material panel or, later, Astroray wrapper nodes), present without one CPU upload per frame, bypass the ~6 ms redraw floor, read voxels through the RNA API (decision 47 goes through Blender's bundled `openvdb` module), add object types, use Blender's DLSS, link custom socket types into native sockets, change how Cycles or EEVEE evaluate nodes.

### K. Crash containment

44. Exceptions never cross the ABI (catch-all → `AR_ERR_INTERNAL` with text). Release builds turn assertions into session failure. A sticky CUDA error puts the session in FAILED, shows "GPU error: <text>. Save your work and restart Blender.", and the addon refuses new GPU sessions in that process while the CPU device stays available. Inputs are validated (decision 9). The engine never installs process-wide exception handlers (Blender has its own). Out-of-process gate: move the engine behind the same ABI over shared memory if a DLL conflict with Blender cannot be fixed by load order or static linking, or if the build-phase GUI soak (V13) sees more than one engine-caused Blender crash per 100 hours (PROVISIONAL).

### L. Packaging and Blender 5.4

45. Extension zip: `blender_manifest.toml` (id `astroray`, `blender_version_min = "5.3.0"`, no max, `platforms = ["windows-x64"]`, `license = ["SPDX:GPL-3.0-or-later"]` for the addon with the engine's Apache-2.0 text under `LICENSES/`, permission `files` for textures, volumes, caches and science output); Python package, generated ctypes bindings, `bin/astroray_core.dll`, OIDN DLLs, AOT kernel modules, data tables, asset `.blend`. No NVIDIA binaries and no OpenVDB (decision 47 uses Blender's own module). The DLL is loaded by full path after `os.add_dll_directory(bin)`. The zip is generated from the source tree by `blender --command extension build`, never from an allow-list (v1 lesson). OptiX and kernel caches go to `extension_path_user(__package__, create=True)`.
46. Blender 5.4: the headless sync suite and the API-surface check run in CI against every published `bpy` 5.3.x and, as soon as it appears on PyPI, 5.4 (research 01 K4); a GUI smoke test (install, viewport, F12) runs per Blender version on the GPU machine. The nine boundary files were byte-identical between 5.3 and `main` on 2026-11-14 (research 01 K1); the known fragile points are `.corner_vert` (fallback in decision 17), pass names, `Window.modal_operators` (decision 22, which falls back to full reads if it disappears) and the bundled `openvdb` module's API (decision 47); the API-surface check covers all four.

### Challenges to the spine

- **Mesh keying (spine 6).** "Key meshes by the original datablock's `session_uid`" aliases two objects that share a mesh but carry different modifiers. Decision 18 refines it with Cycles' real-object-data rule. Evidence: spike B did not test modifier-generated meshes (its section 6) and keyed geometry-nodes output only through the empty host mesh.
- **Triangulation (spine 6).** The measured native triangulation was a fan, which is wrong for concave n-gons; decision 10 replaces it with Blender's quad rule plus ear clipping. Speed should be unchanged for triangles and quads; V3 confirms.

## Alternatives considered

- **Hydra delegate.** Free incremental export, but a rebuild against Blender's monolithic USD every release, lossy RGB Principled→OpenPBR mapping, no per-object custom data, and the same CPU-buffer display on Vulkan (research 01 H3-H7). Rejected.
- **Out-of-process daemon now.** Crash isolation, but a protocol, lifecycle and one more host copy, and Python still walks the depsgraph (research 01 option c, research 02 F8). Kept as a gated fallback (decision 44); the flat ABI keeps it cheap.
- **nanobind module.** Nicer arrays, but a Python-version axis and `/MD` coupling for a surface this small (research 08). ctypes per spine; revisit only if profiling shows boundary cost.
- **`loop_triangles` reads.** 3.5× slower on S1, 2.9× on S2, cold (spike B 2.1); fails the 2× TTFF threshold on S2 (2.5×/3.3×).
- **`as_pointer()` direct DNA reads** (Malt). Would remove the ~25 ns/corner integer read, but depends on internal layout and an internal attribute name and was not measured (spike B 5). Held as remedy 1 in decision 35.
- **Abort the in-flight pass on every view change** (the first draft). Starves the display during a continuous orbit when a pass takes longer than Blender's ~6 ms redraw cadence; Cycles hides this with a fixed navigation divider, which owner decision 10 forbids. Replaced by decision 25.
- **Topology settle timer** (the first draft: re-read topology after 250 ms without a geometry update). Never settles during playback, and shows wrong geometry without warning meanwhile. Replaced by decision 22's provable-cases rule.
- **Native `.vdb` reading in the engine.** Needs OpenVDB with Boost, TBB and Blosc in our build and zip, against the build-time budget and 0001 D9. Rejected while Blender's bundled module works (decision 47).
- **Present path A (new `GPUTexture` per frame).** RGBA32F at 4K: 11.4 ms with a 28.8 ms p99 and 47-49/s; RGBA16F from float32 is 4× slower than RGBA32F at 1080p (spike B 1.1). Rejected.
- **R11G11B10F or RGB9E5 packed 4-byte present at 4K** (spike B's 4K recommendation: 1.5 ms). Cheaper, but banding was not evaluated and alpha is lost; striped RGBA16F holds 60/s at 4K by measurement. Kept as the fallback if V8 fails on a real 4K display.
- **TLAS/GAS refit.** Measured rebuilds are already below a frame; refit costs trace quality and ALLOW_UPDATE memory (v1 lesson). Rejected.
- **A shared scene between viewport and F12 sessions.** Saves VRAM, but reintroduces the cross-session locking v1 needed (admission token, pause gate). Rejected; addon-driven trim (decision 14) covers the memory case.
- **Reading Cycles' properties (`scene.cycles`) instead of own PropertyGroups.** Zero UI work, but couples to another addon's names and semantics and fails when Cycles is disabled. Rejected.
- **Engine-side tonemapping** (LuxCore). Double transform under Blender's view transform (research 02 F6). Rejected for beauty; used only for heat maps drawn outside the view transform.

## Consequences

- **Easy:** every edit class costs data proportional to the edit; the engine and its graph compiler are testable from the CLI host with recorded deltas; sessions are independent, so viewport, F12, previews and several viewports need no global coordination; edits are atomic per generation with the previous image kept on screen; the boundary can move out of process without changing either side's logic.
- **Hard:** integer topology reads stay on Blender's slow path (~25 ns/corner), and edit-mode topology operators pay a full read (65 ms per 1M triangles); two rendered viewports double VRAM and GPU work; an edited key briefly holds old and new device copies; a trimmed viewport re-syncs after F12; a GPU fault ends GPU rendering for that Blender process; heat maps and engine depth need extra draw code; volume transport depends on Blender's bundled `openvdb` module.
- **Cost when unused:** motion steps (one u32 per descriptor, S = 1); light/shadow-link sets (4 B per instance); `ar_edit_set_astro` (one function, no UI); display-mode switch (one enum check per published frame); depth writing (none until enabled); VRAM ledger (one record per allocation); host-mapped fallback (none until VRAM is short).

## What lands when

- **First milestone:** ABI v0 (decisions 4-9, including `ar_edit_reset`, generation outcomes and asynchronous create/close) for meshes, instances, lights, world, camera, settings; native triangulation and the quad-rule golden test; fast-path mesh reads, keys, the full classification table, the provable-cases topology cache; pipelined GAS/TLAS; the VRAM ledger (reporting only); threading, pass-boundary generations, cancel, F12 with progress, partial results and view layers, viewport pause on F12; presentation path with stripes and the view hash; Beauty plus sample-count and bounce-count heat maps; build identity check; crash rules; extension build and clean-profile install; V7a/V7b instrumentation with S1/S2; probes P1-P6.
- **Before first release:** images (all three sources, UDIM, interop ids), volumes through decision 47, curves, points, panoramic cameras, camera/object motion blur, light linking, holdout/shadow catcher, the full pass list with `register_pass`, dynamic-resolution controller and DLSS mode mapping, trim-spill-fail overflow, Material panel kinds, astro object panel and hook wiring, asset library, science export UI, preview session reuse, persistent data, the 5.4 CI leg, the TTFF gate on S3.
- **After first release:** deformation blur, true displacement and viewport depth writing, custom shader nodes if P1 passes, Gaussian splats, legacy particle hair, bake, stereo, custom camera, GAS disk cache and `as_pointer()` reads only if the TTFF gate requires them, the out-of-process engine only if decision 44's gate trips.

## Verification

| Check | Method | Pass |
|---|---|---|
| V1 ABI layout | register-time table compare; CI | identical, else registration fails naming the field |
| V2 Triangulation | headless golden test vs `loop_triangles` (triangles, planar/non-planar/concave quads, concave n-gons) | tris and quads index-identical; n-gons cover the same area within 1e-6 relative, no degenerate triangles |
| V3 Sync speed | S1/S2 from spike B `make_scenes.py`, cold and warm | S1 ≤ 100 ms, S2 reads ≤ 950 ms; > 20% regression fails |
| V4 Classification | one scripted case per row of decision 20's table, plus: move, instancer move, material, vertex, add/delete object and collection instance, collection exclude toggle, armature and shape-key playback, particle-instancer edit, selection, node group, image, world, light, rename, an unlisted type | expected action class and call counts; no full resync except the unlisted type; each edit visible against the pre-edit frame |
| V5 Keys | shared mesh with and without differing modifiers; nested collection instances | modifiers → two geometries; linked duplicates → one GAS in the ledger; distinct identity tuples → distinct keys |
| V6 Topology cache | edit-mode edge rotate, flip normals, sort elements; geometry nodes with constant counts and animated topology; armature playback; modal translate in edit mode; sculpt with dynamic topology | every presented frame's triangles equal `loop_triangles`-derived geometry; fast path taken exactly for playback, translate and sculpt without dynamic topology |
| V7a TTFF gate | screen-capture poller from the shading switch, S1/S2/S3, first and second switch, Cycles vs Astroray | ≤ 1.2× Cycles each (PROVISIONAL, owner to confirm); goal ≤ 1.0× |
| V7b TTFF phases | log-bounded intervals as in decision 35, per-phase log | reported per phase; explains any V7a gap |
| V8 Present | 300 paced frames at 60 Hz producer, 1080p/1440p/4K texture, Vulkan and OpenGL backends, then a real 4K display | ≥ 58 updates/s each; `view_draw` p99 ≤ 3 ms (1080p), ≤ 15 ms (4K) |
| V9 Colour | scene-linear chart vs Image Editor under Standard, AgX, exposure +1, gamma, a look | per-patch difference ≤ 2/255 |
| V10 Heat-map colour | screenshot of colormap patches under AgX, on the route P4 chose | exact sRGB ±1/255 |
| V11 Controller | unit test on recorded traces + GUI with injected 30/60/90 ms passes; continuous scripted orbit on S2 (view change every 6 ms for 5 s) | never steps down while passes ≤ 41.7 ms; reaches ≤ 41.7 ms within 0.5 s; steps down within 100 ms when no frame arrives; scale changes alone never count as interaction; full resolution within 250 ms of interaction stop |
| V12 Latency | F12 cancel; single camera change; the continuous orbit of V11; frame log | cancel p99 ≤ 100 ms; new-view frame p95 ≤ 2 passes + 10 ms; during the orbit ≥ 24 presented frames/s after the controller settles and no gap > 2 passes + 10 ms; zero stale frames after a newer one |
| V13 Coexistence soak | 30 min scripted: two viewports, F12, previews, edits, a VRAM-tight F12 | zero crashes and CUDA errors; viewports resume (trimmed ones re-sync); ledger back to baseline ±1 MiB |
| V14 VRAM overflow | oversized textures with and without a paused viewport; oversized geometry | trim happens before any spill; then textures spill and the render completes with report; geometry fails with GiB message, Blender alive |
| V15 Fault containment | test-build fault injection; invalid indices; NaN positions; undecodable image | session FAILED, message, Blender alive, CPU device works; bad edit's generation dropped naming the datablock with the previous frame kept; bad image magenta + report |
| V16 Packaging | clean profile install via `blender --command extension install-file`, F12 default cube; 5.3 and 5.4 headless | renders; every module and data file present |
| V17 Honour | enumerate Astroray RNA properties; deterministic CPU renders | each changes a render or is greyed with INFO; every tooltip has units |
| V18 Camera | Blender `world_to_camera_view` vs engine rays via CLI host (persp, ortho, shift, sensor fit, portrait, viewport camera view) | ≤ 1e-5 NDC |
| V19 Images | native decode vs `image.pixels` per format and interop id | 8-bit exact, float ≤ 1e-6 after conversion; unsupported id magenta + report |
| V20 Report | material with an unsupported node | magenta, listed once per render |
| V21 Volumes | OpenVDB sample (bunny_cloud) and a geometry-nodes grid through decision 47 vs `copyToArray` of the whole grid | voxel values identical; host peak ≤ bricks in flight + 64 MiB; transport time recorded |
| V22 Main-thread budget | instrument every ABI call from the main thread, including first session create and close | none > 5 ms except host-data copies |

## Risks and open items

- **Owner: what does "comparable" TTFF mean in numbers?** This record gates V7a at ≤ 1.2× Cycles (PROVISIONAL), with 1.0× as the goal; the draft's 1.5× was looser than the owner's word, and the S2 cold estimate (0.97-1.18×) sits near either threshold.
- P1 (= 0005 M-7): does a custom group node keep its identity through `inline_shader_nodes()`? Decides only the post-release custom nodes.
- P2: does Blender 5.3 call `view_pause`/`view_resume` on viewport engines when F12 starts?
- P3: edit-mode read correctness and cost, and whether `context.window.modal_operators` is populated inside `view_update` during a modal transform (decision 22 falls back to full reads if not). Full reads hold 24/s in edit mode only up to roughly 0.6M triangles (65 ms per 1M); larger edit-mode meshes need remedy 1 (`as_pointer()`).
- P4: which heat-map route escapes the view transform (decision 40). 0008's heat-map display item names the POST_PIXEL handler; it follows whichever route P4 picks.
- P5: VRAM used by Blender and the desktop; host-mapped texture speed.
- P6: the bundled `openvdb` module: `readAll`, `evalActiveVoxelBoundingBox` and `copyToArray` into a writable numpy view (needs its NumPy support), speed for a 256³ grid and bunny_cloud, `VolumeGrids.save` cost. Only the module's presence is verified. If P6 fails, Blender volume grids have no route and the lead must decide before the volumes milestone (owner decision 7 makes them non-optional).
- **Conflict with 0006 D5:** 0006 writes dense blocks as `[nz][ny][nx]`; `copyToArray` fills x-major arrays (OpenVDB `tools::Dense` default layout, to confirm in P6). Decision 47 asks 0006 to accept X_MAJOR blocks (or the worker re-indexes, one pass). 0006 open item 5 (the export route) is answered by decision 47.
- Aligned with 0005 in this revision: node translation in the addon (D29), material kinds on the Material panel (D16), custom nodes and true displacement after the first release.
- Material, texture and light sync cost are unmeasured; the TTFF table is an estimate there and is the most likely gate failure. Cycles' session and device creation, part of V7a, is unmeasured too.
- Real 4K display, the OpenGL backend and the owner's working viewport (overlays, other editors) were not measured; 4K figures carried ±30% run-to-run variation (spike B 1.1).
- OIDN's TBB dependency versus Blender's bundled TBB in one process (research 01 I1; BlendLuxCore 2.11.1 fixed TBB-related crashes with Blender on Linux).
- DLSS feature re-creation cost on a resolution-level change.
- `.corner_vert` is internal and may be renamed in 5.4.
- Images whose colour space has no interop id (custom OCIO configs) render magenta in the first release.
- CUDA context re-initialisation cost when the last session in a process closes.

## Sources

- Research: `research/01-blender-boundary.md` (B, C, D, E, G, J, K, recommendations 4 and 6), `02-viewport-presentation.md` (F2-F7), `07-blender-feature-surface.md` (F1, F4-F6, volumes, recommendation 5-6), `17-materials-and-custom-types.md` (How these appear in Blender), `04-integrator-architecture.md` (R12), `06-astrophysics-core.md` (R3, R5), `08-build-toolchain.md`, `10-denoise-upscale.md`, `11-licensing.md`, `16-test-scenes-and-assets.md` (OpenVDB samples).
- Spikes: `spikes/B-blender-end-to-end/RESULTS.md` (all tables), `spikes/B-blender-end-to-end/out/cycview_summary.txt` (Cycles viewport log lines), `spikes/A-kernel-budget/RESULTS.md` (section 5: OptiX context and module times).
- Local install listing (2026-11-14): `C:\Program Files\Blender Foundation\Blender 5.3\5.3\python\lib\site-packages\openvdb.cp313-win_amd64.pyd`.
- v1: `v1-lessons/blender-integration.md`, `v1-lessons/research/blender.md`, `v1-lessons/research/gpu-build.md` (BVH, instancing), `v1-lessons/research/transport.md` (motion blur), `v1-lessons/research/survey.md` (host-mapped fallback).
- Papers: D. Eberly, "Triangulation by Ear Clipping", Geometric Tools, 2002; I. Sutherland, R. Sproull, R. Schumacker, "A Characterization of Ten Hidden-Surface Algorithms", ACM Computing Surveys 6(1), 1974 (Newell's polygon normal); K. Shoemake, "Animating Rotation with Quaternion Curves", SIGGRAPH 1985; R. Cook, T. Porter, L. Carpenter, "Distributed Ray Tracing", SIGGRAPH 1984; M. Mikkelsen, "Simulation of Wrinkled Surfaces Revisited", MSc thesis, DIKU, 2008; N. Smith and S. van der Walt, "A Better Default Colormap for Matplotlib", SciPy 2015 (viridis, CC0); K. Museth, "NanoVDB: A GPU-Friendly and Portable VDB Data Structure", SIGGRAPH 2021 Talks.
- Reference code (Apache-2.0 unless noted): Cycles `intern/cycles/blender/sync.cpp` (`BlenderSync::sync_recalc`), `object.cpp`, `mesh.cpp`, `camera.cpp`, `display_driver.cpp`, `BObjectInfo` in `intern/cycles/blender/`; `intern/cycles/integrator/render_scheduler.cpp`; `intern/cycles/device/` (`generic_alloc`, host-mapped fallback); pbrt-v4 `src/pbrt/util/transform.h` (`AnimatedTransform`); OpenVDB Python bindings (`openvdb/openvdb/python/`, `copyToArray`; Apache-2.0); Blender (GPL, API facts only) `makesrna/intern/rna_render.cc`, `rna_depsgraph.cc`, `rna_volume.cc`, `rna_wm.cc` (`Window.modal_operators`), `python/intern/bpy_rna.cc`, `python/gpu/gpu_py_storagebuffer.cc`, `draw/engines/external/external_engine.cc`; stb_image (public domain/MIT), tinyexr (BSD-3), MikkTSpace (github.com/mmikk/MikkTSpace; v1 notes say BSD-3-Clause, confirm before use); OptiX 9.1 Programming Guide (acceleration structure build, compaction, relocation, motion).

## Review and resolution

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
