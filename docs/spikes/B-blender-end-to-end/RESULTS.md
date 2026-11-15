# Spike B: Blender 5.3 end to end (present cost, sync cost, first frame vs Cycles)

Date 2026-11-14. Throwaway code; the numbers are the product. Every number below names its script; raw JSON/logs are in `out/`, the full machine-generated tables are in `out/tables.md` (`python tables.py`). Reproduce everything with `.\run_all.ps1` (serial; each Blender instance is isolated and quits itself or is killed by its own PID).

## 0. Setup and versions

| item | value |
|---|---|
| Blender | 5.3.0, hash 02f0899d78e3 (2026-11-10), Python 3.13.13, numpy 2.3.4, GPU backend **VULKAN** (read at runtime in `gui_probe.py`) |
| GPU / driver | RTX 5070 Ti, 621.24; displays: 2560x1440 @ 164 Hz (primary, where windows opened) and 1920x1080 |
| CPU | 8c/16t (Ryzen 7 7800X3D, from Cycles' device list) |
| Toolchain | MSVC cl 19.44.35208 (VS2022 BuildTools), CUDA 12.8.61 (cudart static), OptiX SDK 9.1.0, system Python 3.13.12 only for `tables.py` |
| Native module | `native/sb.cpp` + `native/gas.cpp` -> `build/sbnative.dll` (`build.ps1`, direct `cl.exe`; C ABI, loaded with ctypes by `sbn.py`) |
| Isolation | every Blender: `--factory-startup`, `BLENDER_USER_CONFIG/SCRIPTS/EXTENSIONS/DATAFILES` = `blender_home\*`, addon registered from the `--python` script, own process, killed only by own PID. (A `blender --version` and one `-b --python-expr` run at the very start went out before the env vars were set; both read nothing.) |
| Owner's Blender | PID 9116 was running (minimised) when I started and was gone by ~13:16, before most GUI runs. I never touched it; early runs may have overlapped with it, later ones did not. |

What the native module is: a worker thread writing an animated pattern into a 3-buffer ring (spec asked for double buffering; 3 avoids reading a buffer being rewritten, `nbuf` is a parameter) in RGBA32F / RGBA16F / RGBA8 / packed R11G11B10F, paced with a high-resolution timer or free-running, exposing front pointer, frame counter, per-frame swap timestamps (same QPC clock as `time.perf_counter_ns`, checked); scene copy-in (meshes, instances, transforms, materials, single-vertex edit) timed with QueryPerformanceCounter; and an OptiX 9.1 GAS-per-mesh (+compaction) + TLAS build. The OptiX part was done.

Scenes (`make_scenes.py`, procedural, saved to `scenes/`):
- **S1**: one mesh, 501,264 vertices, 499,849 quads = **999,698 triangles**, UV map, 1 material.
- **S2**: **2,000 unique meshes x 2,520 triangles = 5.04 M unique triangles**, 300 materials, **10,001 mesh instances** (2,000 own-mesh objects + 5,000 linked duplicates + 2,000 collection-instance empties + 1,000 geometry-nodes instances; +1 empty GN host mesh), 12,003 `object_instances` in total (includes empties, camera, light). Effective instanced triangles 25.2 M. Interpretation note: "5 M triangles over 10 k objects" was read as 5 M unique triangles; instanced total is 25 M.

## 1. Q1: present cost (`gui_present.py`, run by `run_gui.ps1`)

Method. In `view_draw` I wrap the native front buffer as numpy (zero copy), build a `gpu.types.Buffer` over it, then either create a texture per frame (path A) or update a persistent SSBO and dispatch a compute shader into a persistent texture (path B), then draw inside `bind_display_space_shader`/`unbind_display_space_shader`, then `tag_redraw`. 60 warm-up frames, 300 measured frames, native producer running at 60 fps. Times are CPU wall time inside `view_draw` (the GPU work is asynchronous; the redraw interval is the end-to-end number). Every path is read back (`GPUTexture.read`) and compared with the source before measuring (all pass; RGBA8 within 1/255 quantisation).

**Region size.** I could not get a 4K viewport (largest display 2560x1440). Main runs use a 1000x650 window (region 818x556) with the texture at the stated size; the texture upload does not depend on the region. Validation with a maximised 1920x1080 region (`gui_present_matrix_fullregion.json`): BASE 5.73 ms/redraw, A32 7.51, B16 6.54, B11 6.05, identical to the small-region numbers (5.99/7.50/6.54/6.33). 4K numbers are therefore "4K texture, small region", not a 4K window.

### 1.1 Present matrix, producer at 60 fps, upload on every redraw (`out/gui_present_matrix.json`)

Median ms unless stated; "shown/s" = distinct producer frames displayed per second.

| path (source -> texture) | res | wrap | upload/update | draw | view_draw min / med / p99 | redraw interval | redraws/s | shown/s |
|---|---|---|---|---|---|---|---|---|
| BASE (no upload, floor) | any | 0 | 0 | 0.04 | 0.0 / 0.1 / 0.1 | 5.99 | 168 | 60 |
| A32: f32 -> RGBA32F, new texture per frame | 1080 | 0.014 | 1.49 | 0.046 | 1.3 / 1.6 / 5.4 | 7.50 | 123 | 60 |
| A16: f32 -> RGBA16F | 1080 | 0.016 | 6.17 | 0.053 | 4.5 / 6.3 / 7.3 | 12.17 | 84 | 60 |
| B16: f16 words -> SSBO -> compute -> persistent RGBA16F | 1080 | 0.019 | 0.74 | 0.033 | 0.8 / 0.8 / 0.9 | 6.54 | 152 | 60 |
| B32: f32 -> SSBO -> compute -> persistent RGBA16F | 1080 | 0.018 | 1.47 | 0.034 | 1.5 / 1.5 / 1.7 | 7.50 | 134 | 60 |
| B11: packed R11G11B10F word -> SSBO -> compute -> RGBA16F | 1080 | 0.020 | 0.37 | 0.037 | 0.4 / 0.4 / 0.5 | 6.33 | 159 | 60 |
| C8B: RGBA8 words -> SSBO -> compute -> persistent RGBA8 | 1080 | 0.020 | 0.37 | 0.034 | 0.4 / 0.4 / 0.5 | 6.16 | 160 | 60 |
| C8A: f32 -> RGBA8 texture per frame (only direct route to RGBA8) | 1080 | 0.015 | 2.10 | 0.044 | 1.9 / 2.2 / 3.0 | 8.06 | 123 | 60 |
| A32 | 4K | 0.019 | 11.39 | 0.074 | 6.7 / 11.5 / **28.8** | 18.21 | 49.5 | 47.3 |
| A16 | 4K | 0.024 | 10.08 | 0.093 | 8.3 / 10.2 / 12.2 | 16.51 | 60.4 | 59.6 |
| B16 | 4K | 0.027 | 11.13 | 0.059 | 10.2 / 11.2 / 13.4 | 17.17 | 57.8 | 57.2 |
| B32 | 4K | 0.031 | **31.00** | 0.082 | 16.4 / 31.1 / 41.2 | 37.25 | 26.9 | 26.8 |
| B11 | 4K | 0.019 | **1.47** | 0.033 | 1.5 / 1.5 / 1.6 | 7.50 | 134 | 60 |
| C8B | 4K | 0.019 | **1.47** | 0.033 | 1.5 / 1.5 / 1.6 | 7.48 | 135 | 60 |
| C8A | 4K | 0.019 | 10.37 | 0.059 | 8.2 / 10.5 / 19.1 | 16.51 | 59.0 | 55.0 |
| B16 striped x2 / x4 (33 / 16 MiB per update) | 4K | 0.024 | 5.66 / 6.38 | 0.05 | 3.0 / 5.8 / 14.4 ; 3.1 / 6.5 / 17.0 | 11.8 ; 12.6 | 84 ; 78 | 60 ; 59.5 |
| B32 striped x2 / x4 (66 / 33 MiB per update) | 4K | 0.03 / 0.02 | 23.12 / 6.36 | 0.07 | 20.7 / 23.2 / 27.6 ; 5.9 / 6.5 / 34.3 | 29.3 ; 12.4 | 34 ; 66 | 34 ; 51 |
| A32 tiled x4 / x8 (separate textures) | 4K | 0.03 | 13.64 / 7.89 | 0.12 / 0.15 | 5.3 / 13.8 / 33.9 ; 5.9 / 8.1 / 33.4 | 20.5 ; 14.2 | 48 ; 58 | 43 ; 51 |
| A16 tiled x4 | 4K | 0.03 | 26.76 | 0.13 | 18.2 / 26.9 / 30.6 | 33.1 | 30.5 | 30.4 |

(`draw_texture_2d` was used for these; a cached batch/shader (`/cached` rows in `out/tables.md`) saves 0.02-0.03 ms, nothing that matters. Repeat-to-repeat variation of the same config across two matrix runs on the same day was up to 35% (B16 4K cached 14.9 ms vs 11.1 ms for tex2d), so read 4K figures as +-30%.)

**Where the time goes.**
- The redraw interval is `view_draw` time plus a nearly constant **~6 ms** owned by Blender (interval minus view_draw: 5.7-6.3 ms in all 30+ configs; BASE with no upload is 6.0 ms = 168 redraws/s). Inferred cause: Blender's main loop sleeps ~5 ms when no input event is pending; I did not confirm it in source. Consequence: the cap on updates/s is 1/(6 ms + view_draw) however light the draw is.
- Python overhead is negligible: numpy wrap + `gpu.types.Buffer` construction 0.014-0.03 ms; draw call 0.01-0.15 ms. Everything is the host-to-GPU transfer inside texture creation / `GPUStorageBuf.update`.
- `GPUTexture(format='RGBA16F', data=Buffer)` converts float32 to half on the CPU: A16 costs 4-6 ms at 1080p, **4x more than A32** (1.5 ms) although it uploads half the bytes. Only `FLOAT` Buffers are accepted for textures (UBYTE raises `Only Buffer of format FLOAT is currently supported`), so 8-bit and half-float sources cannot go through path A directly.
- **Cliff at 32 MiB per `GPUStorageBuf.update`.** `out/gui_present_sweep*.json` (3 runs, back-to-back updates): 24 MiB costs 0.48 ms (~50 GB/s, i.e. memcpy-class) and 32 MiB costs 4.7 ms; above that ~6 GB/s (64 MiB 9.5-10.4 ms, 128 MiB 19.7-20.8 ms). Same in all three runs. Texture creation from a Buffer has no such cliff up to 128 MiB (6.2-6.5 ms for 128 MiB RGBA32F, 20-24 GB/s) but is 1.5-4x slower than SSBO below the cliff in the paced runs. A 4K RGBA8 or R11G11B10F frame is 31.6 MiB, just under the cliff, which is why B11/C8B are 8x cheaper than B16 at 4K.
- Striping the SSBO update into <=33 MiB pieces helps back-to-back (B16x2 5.7 ms vs 11.1 ms; B32x4 6.4 vs 31.0) but **not reliably when frames are paced**: B32x4 at 60 fps paced cost 17.3 ms (see 1.2), B16x4 paced 7.4 ms. Inferred (not tested directly): GPU/PCIe idle power states make sparse uploads slower than back-to-back ones; the same effect shows as A32 at 1080p costing 5.8-6.0 ms at 24/30/60 fps paced vs 1.6 ms when the pipeline is kept busy (120 fps or free-running).

### 1.2 Can the pipeline hold N updates/s? (`out/gui_present_paced.json`)

Producer paced at the target rate, upload only when a new frame exists, 5 s per config. Shown/s = displayed distinct frames.

| path | res | target 24 | 30 | 60 | 120 | free-run (max) |
|---|---|---|---|---|---|---|
| B16 | 1080 | 24.2 | 30.2 | 60.2 | 120.1 | 153 |
| A32 | 1080 | 24.0 | 30.0 | 60.1 | 109 (target missed) | 108 |
| B11 | 1080 | 24.2 | 30.2 | 60.2 | 120.0 | 164 |
| A32 | 4K | 24.0 | 30.1 | **49.3** | 48.0 | 48.1 |
| B16 | 4K | 24.0 | 30.1 | **58.6** | 57.5 | 44.1 |
| B11 | 4K | 24.0 | 30.2 | 60.1 | 120.0 | 130 |
| B16 striped x4 | 4K | - | - | 60.1 | 77.5 | 70.9 |
| B32 striped x4 | 4K | - | - | 40.9 | 36.2 | 37.6 |
| A32 tiled x4 | 4K | - | - | 46.0 | 45.2 | 50.5 |

### 1.3 Latency, wake-up and threading (`out/gui_present_latency.json`, `gui_present_stall.json`)

Latency = native swap timestamp of the frame being drawn to the start of that `view_draw` (60 fps producer, B16, 1080p): free redraw loop median 3.0 / p95 5.5 / max 6.0 ms; `bpy.app.timers` poll at 1 ms + `tag_redraw`: 2.7 / 5.3 / 5.6 ms; Python thread polling + `tag_redraw()` from a non-main thread: 3.3 / 5.6 / 6.9 ms (**worked, no crash in these runs**; thread-safety not established). The distribution is the ~6 ms Blender loop granularity (uniform 0-6 ms), so wake-up mechanism does not matter. Swap -> end of `view_draw` CPU: median 3.7 ms (B16 1080p), 5.6 (A32 1080p), 18-20 (4K B16/A32). 30 and 120 fps producers behave the same at 1080p; at 4K with a 120 fps producer only 46-57 frames/s are shown (upload-limited).

GIL / stall: the producer is a native thread that never touches Python, so the GIL cannot stall it; what slows it is memory/PCIe contention. Free-running producer, 4K RGBA32F: 162 fps with a trivial draw (BASE) vs 115 fps while A32 uploads run (-29%); 4K f16: 282 fps with B16 uploads (standalone 299 fps in `selftest.py`); 1080p: 898 -> 835 fps (A32). Worst gap between two swaps with a 60 fps target: 17-18 ms at 1080p, 19-30 ms at 4K with A32/B16 uploads (nominal 16.7). Whether Blender holds the GIL inside `GPUStorageBuf.update`/`GPUTexture()` was not measured; the worker does not need it.

### 1.4 gpu module facts found at runtime (`gui_probe.py`, `probe_gpu_api.py`)

- `gpu.types.Buffer('FLOAT', n, numpy_array)` takes **0.7-2.4 microseconds at 33 MB: zero copy** (it shares memory: a numpy write was visible through the Buffer; GPU readback of every path matched the source). `Buffer` exports the buffer protocol (`memoryview`, `np.frombuffer` writable), so the native worker could write straight into a Buffer's memory.
- **Footgun:** `Buffer('FLOAT', n, float16_array)` is accepted with no dtype or size check (n floats read over a buffer with half the bytes), `UINT`/`INT` over a view of other dtypes likewise. Always view the numpy array as the matching dtype yourself.
- `GPUTexture.__init__` has no in-place update method; the only in-place routes are `GPUStorageBuf.update` + `gpu.compute.dispatch` into a persistent image (path B) and `GPUTexture.clear`. `GPUStorageBuf.update(Buffer)` requires the same size as at creation.
- Path B works on Vulkan from inside `view_draw`: `gpu.types.GPUShaderCreateInfo` with `local_group_size(16,16,1)`, `storage_buf(0,'uvec2','src[]',qualifiers={'READ'})`, `image(0,'RGBA16F','FLOAT_2D','dst',qualifiers={'WRITE'})`, three `INT` push constants (`W`,`SH`,`Y0`), `gpu.shader.create_from_info`; per frame `shader.bind()`, `shader.image('dst', tex)`, `shader.storage_block('src', ssbo)`, `shader.uniform_int(...)`, `gpu.compute.dispatch(...)`. **No explicit barrier was needed**: the drawn texture matched the SSBO contents (read back). The 4K B32 SSBO is 132 MB and worked; sizes up to 256 MiB were tried in the sweep.
- Colour: with `draw_texture_2d` inside `bind_display_space_shader` the engine output is treated as scene-linear and Blender applies the view transform afterwards. Evidence (`out/gui_screenshot.png`, `gui_screenshot.py`, A32 path, 1920x1080 region): source R=1.18 shows as 205/255 (not clipped), 0.958 as 200, 0.066 as 99 (sRGB encoding alone would give 69), i.e. a filmic/AgX-style compression is applied (default view transform of this profile). The picture is visibly correct (animated gradient/checker). I did not run the full S3 colour-management protocol (no exposure/gamma/other transforms, no HDR display).
- `max_work_group_count_get`/`max_work_group_size_get` need an argument (axis); not queried. Vulkan extension list printed in `out/gui_probe.json` includes `VK_EXT_host_image_copy` and `VK_KHR_external_memory_win32` (Blender's use of host-image-copy is not confirmed).

## 2. Q2: sync cost

### 2.1 Full sync (background Blender, `sync_bench.py`; 5 runs in one process + 3 cold repeats per strategy)

Two read strategies for the same data (positions, triangle vertex indices, corner normals, one UV layer, material index, per-instance transform):
- **naive**: `calc_loop_triangles()`, `loop_triangles.foreach_get` for vertices/loops/material, `vertices.foreach_get('co')`, `corner_normals`, `uv_layers.active.uv`.
- **fast**: `attributes['position'].data.foreach_get('vector')`, `attributes['.corner_vert'].data.foreach_get('value')`, `polygons.foreach_get('loop_start')`, corner normals and UV via attributes, **fan triangulation done natively** (`sb_scene_set_mesh_poly`, included in the native copy time). Note `.corner_vert` is an internal attribute name (fragile across versions); `me.polygons.foreach_get('loop_start')` is public.

Per-phase rows are warm minimum over runs 2-5 of one process (`sync_S1.json`, `sync_S2.json`); the "total, cold" row is three fresh-process runs (`sync_*_quick_*.json`). All times ms.

| | S1 naive | S1 fast | S2 naive | S2 fast |
|---|---|---|---|---|
| iterate `object_instances` (+ key, matrix, name) | 0.06 | 0.04 | 30 | 30 |
| positions | 10.9 | 0.28 | 61 | 5.2 |
| triangle indices (loop_triangles vertices + loops + material / corner_vert + loop_start) | 269 | 62 | 1,357 | 326 |
| corner normals | 1.0 | 0.9 | 7.1 | 7.2 |
| UV | 0.7 | 0.6 | 5.3 | 7.8 |
| native copy (S2: 2,000 calls) | 2.7 | 3.7 (incl. triangulate) | 15 | 23 |
| **total, warm min** | 289 | 68 | 1,502 | 419 |
| **total, cold (3 fresh processes)** | 311 / 314 / 320 | 89 / 88 / 91 | 2,678 / 2,680 / 2,687 | 928 / 928 / 1,009 |
| of which native copy, cold | 7.7 | 9.7 | 114 | 146 |
| OptiX GAS(+compaction)+TLAS from the synced data, cold | 8.9 (GAS 2.9, upload 1.5) | | 491 (upload 30, GAS build 428, compaction 27, TLAS 4.9) | |
| GAS size raw -> compacted | 62 MB -> 22 MB | | 318 MB -> 116 MB (TLAS 3 MB) | |

Blender-side costs that precede any engine code (same for Cycles): `open_mainfile` incl. depsgraph evaluation 27 ms (S1) / 600 ms (S2); tagging every object and `view_layer.update()` 0.08-0.13 s (S1) / 85 ms (S2). `evaluated_depsgraph_get()` after load returns in 0.01 ms (already evaluated).

**What is unavoidable Python overhead?** For S2 fast: per-instance loop 30 ms (about 3 us per instance for key + `matrix_world` + name; 7% of warm sync, 3% of cold), per-mesh call overhead is microseconds (positions 5 ms for 2,000 meshes). The dominant cost is the **integer-attribute read**: `.corner_vert` costs ~25 ns per corner (S1 49 ms for 2.0 M corners, S2 255 ms for 10.2 M) and `loop_start` 12.5 ms per 0.5 M faces, versus 0.19 ms for 0.5 M float3 positions. That is Blender's `foreach_get` slow path for int-typed collections, not our loop, and it is the thing to engineer around (see 5). Cold vs warm: the extra ~500 ms cold in S2 fast is mostly first-touch of fresh memory (native copy 146 vs 23 ms) and first-use caches; a pre-reserved native arena would remove most of the native part.

### 2.2 `foreach_get` fast path vs slow path (S1 mesh, 501 k vertices / 2.0 M corners / 1.0 M triangles; `out/sync_S1.json`, `out/fastpaths_S1.json`)

| call | min ms |
|---|---|
| `vertices.foreach_get('co')` float32 exact | 10.7 |
| same, float64 array (mismatch) | 38.5 |
| `attributes['position'].data.foreach_get('vector')` float32 | **0.19** |
| `corner_normals.foreach_get('vector')` float32 exact / float64 mismatch | 0.76 / **117** |
| `uv_layers.active.uv.foreach_get('vector')` float32 exact / float64 | 0.51 / 72 |
| `attributes[uv].data.foreach_get('vector')` | 0.50 |
| `vertex_normals` / `polygon_normals` float32 | 0.19 / 0.20 |
| `loop_triangles.foreach_get('vertices')` int32 exact / int64 / float32 | 119 / 121 / 134 (no fast path; dtype mismatch changes nothing) |
| `loop_triangles` `'loops'`, `'material_index'`, `'polygon_index'` | 76, 72, 48 |
| `loop_triangle_polygons.foreach_get('value')` | 24 |
| `attributes['.corner_vert'].data.foreach_get('value')` int32 | 49 |
| `loops.foreach_get('vertex_index')` / `'edge_index'` | 85 / 187 |
| `polygons.foreach_get('loop_start')` / `'loop_total'` / `'material_index'` / `'use_smooth'` | 12.5 / 18.5 / 36 / 30 |
| `attributes['sharp_face'].data.foreach_get('value')` (bool) | 0.023 |
| `np.array(me.vertices[i].co)` x 1000 (naive per-vertex) | 1.0 ms per 1000 = 1 us each (500 k vertices would be 0.5 s) |
| `ob.data` x300 / `ob.to_mesh()+to_mesh_clear()` x300 | 0.03 ms / 9.2 ms (S1), 0.2 / 4.5 ms (S2) |

Rule found: **float-vector attribute reads with the exactly matching dtype are memcpy-class (0.2 ms per 6 MB); a mismatched dtype costs 30-150x; int-typed reads and loop_triangles have no fast path** (25-250 ns per element). `mathutils.Matrix` does not export a contiguous buffer (`memoryview` raises `BufferError`); the cheapest matrix extraction found is `np.asarray(inst.matrix_world, np.float32)` at ~0.8-1.1 us (S2: 10 ms per 12 k instances); `np.array(...)` 1.05 us, assigning rows 1.6 us.

Per-instance loop costs (S2, 12,003 instances, `out/sync_S2.json` micro): bare iteration 0.17 us; + `inst.object` 0.25; + `.type` 0.34; + `.data.original.session_uid` (the key used) 0.53; + `.data.name` 0.49; + `matrix_world` via `asarray` +0.67; `is_instance + persistent_id` 0.37 us. **Trap:** `ob.data.session_uid` of evaluated meshes is not a usable key: 2 distinct values for 2,001 meshes (`.original.session_uid`, `.as_pointer()` and name give 2,001). My first S2 run synced 2 meshes before I noticed.

### 2.3 Incremental edits (GUI instance, real `view_update` + `depsgraph.updates`; `gui_edits.py`, `out/gui_edits_S1.json`, `gui_edits_S2.json`)

Edits are made from a timer; "Blender latency" = edit call to `view_update` entry (includes Blender's depsgraph evaluation and loop tick); parse = iterating `depsgraph.updates`; resync = our work for just that change. ms.

| edit | scene | what Blender reports | Blender latency | parse | our resync |
|---|---|---|---|---|---|
| move one object | S1 / S2 | 1 update: Object [transform] | 0.2 / 9.9 | 0.06 / 0.26 | 0.08 / 0.13 (set 4x4 by index) |
| move one collection-instance empty | S2 | 1 update: Object [transform]; instances are not named | 10.0 | 0.39 | 8.1 (re-iterate all 12,003 instances to find its 1 child; bare iteration alone 2.7 ms) |
| change one material value (Roughness) | S1 / S2 | S1: 4 updates (Object[geom+shading], Mesh[shading], Material[shading], NodeTree); S2: 33 updates (every user of the material gets Object+Mesh entries) | 0.3 / 10.5 | 0.02 / 0.44 | 0.11 / 0.28 (read Principled values, 16 floats) |
| edit one vertex (`mesh.update()`) | S1 (1 M tris) | Object[geom+shading], Mesh[geom+shading+transform] | 2.2 | 0.02 | naive full re-extract + re-copy **290 + 3.3**; fast re-extract + native triangulating copy **65 + 3.7**; positions + corner normals only **1.1**; positions only + numpy diff against the previous copy **0.64 + 0.5**, then 1 vertex native edit (0.04; the 35 ms printed in the JSON for that step is the first `np.unique` call in the harness, 0.04 ms on the next edit) |
| same | S2 (2.5 k tris) | Mesh[...] plus 3 linked-duplicate objects | 30.8 | 0.38 | full re-extract 1.2 + 0.06 |
| add one object (linked duplicate) | S1 / S2 | S1: 7 updates; **S2: 9,007 updates** (every object flagged shading + Scene) | 0.5 / **285** | 0.04 / **7.4** | 0.01 native; 10.7 total with the harness's mesh re-read |
| add one object with a new mesh | S2 | 9,008 updates | 285 | 8.0 | new mesh read 0.10 + add 0.00 |
| selection change only | S1 / S2 | Scene only | 9.3 / 23 | 0.02 / 0.28 | none (ignore) |

So in the Python/native split the resync work for every kind of edit is 0.1-10 ms, except a naive re-read of a million-triangle mesh (290 ms). What costs time is on Blender's side: 285 ms depsgraph latency after adding one object to a 10 k-object scene, and an update list with 9,007 entries that the engine must filter.

## 3. Q3: first frame vs Cycles

**Cycles baseline** (`cycles_bench.py`, Cycles OptiX on the 5070 Ti only, 1920x1080, 1 sample, persistent data off, denoise and adaptive off, `--debug-cycles --verbose 3`). The log's own phase times are the precise numbers: "Total render time" minus "Render time (without synchronization)" is everything before the first sample (Blender->Cycles data sync + scene update + BVH + device upload). Render 1 is cold in the process (device init + kernel load); renders 2-4 are warm.

| | S1 | S2 |
|---|---|---|
| render op wall, 1 sample (cold / warm min) | 974 / 252 ms | 1,845 / 1,698 ms |
| Cycles pre-first-sample, from log (cold / warm min..max) | 895 / 186-192 ms | 1,371 / 1,275-1,287 ms |
| of which Blender->Cycles C++ data sync (log) | 5-15 ms | 50-71 ms |
| per-sample cost (65-sample render minus 1-sample, /64) | ~7.6 ms | ~10 ms |
| log: BVH build for 2,003 meshes | | ~950 ms (log timestamps 5.469 -> 6.422 s) |

**Cycles rendered viewport, first draw** (`gui_cycles_viewport.py`, 1920x1080 maximised, camera view, OptiX, same log): I could not detect the first displayed frame from pixels (reading the framebuffer inside a POST_PIXEL draw handler returned zeros for the whole run: failure of the approach, trace kept in `out/gui_cycles_viewport_*.json`). From the Cycles log instead: the viewport session starts, and from the "Total time spent synchronizing data" line to the first "Rendered N samples" line (graphics interop display update follows) takes **0.125 s for S1 and 0.97 s for S2** (sync itself 7 ms / 57 ms; `out/cycview_summary.txt`, `parse_cycview.py`). The absolute time since the switch carries about +-0.3 s of clock-origin uncertainty and is not used.

**Ours** = Python sync into the native module + OptiX GAS/TLAS build + upload (no shaders, textures or lights, which Cycles also processes: 307 shaders in S2).

| | ours naive | ours fast | Cycles F12 pre-first-sample (warm) | Cycles viewport sync->first sample |
|---|---|---|---|---|
| S1, 1.0 M tris | 311 + 9 = **320 ms** | 89 + 9 = **98 ms** | 190 ms | 125 ms |
| S2, 5.0 M tris / 10 k instances | 2,680 + 491 = **3,170 ms** | 928 + 491 = **1,420 ms** | 1,280 ms (cold 1,371) | 970 ms |
| ratio vs F12 (S1 / S2) | 1.7x / 2.5x | 0.5x / 1.1x | 1x | |
| ratio vs viewport (S1 / S2) | 2.6x / 3.3x | 0.8x / 1.5x | | 1x |

In the GUI the first `view_update` full sync (naive path, inside the engine callback) took 307 ms (S1) and 2,609 ms (S2), matching the background numbers (`out/gui_edits_*.json`).

## 4. Verdict against the thresholds

- **Updates per second at 1080p: 24, 30, 60 and even 120 are all reachable** with every path except A32 at 120 (109) and A16 (84 redraws/s, still above 60). B16 (half-float source, compute unpack) is the best at 0.74 ms and 150+ redraws/s; the Blender loop (~6 ms per redraw) caps at ~165/s.
- **Updates per second at 4K (4K texture, small region): 24 and 30 hold for every path except B32 (27/s); 60 holds only with ≤32 MiB per update: packed R11G11B10F or RGBA8 through the SSBO+compute route (60 and 120 held, 1.5 ms).** RGBA16F (B16) reaches 57-59 of 60 (paced 58.6), RGBA32F (A32) 47-49, i.e. marginal or short of 60. Striping the SSBO helped in back-to-back mode but not under pacing (B16 x4 gave 60.1 at 60 and 77 at 120; B32 x4 only 41). Fail rule "cannot hold 24 updates/s at 1080p": **not triggered**. At 4K 24 holds for all paths.
- **First frame within 2x of Cycles:** with the fast read path **yes**: S1 0.5x (F12) / 0.8x (viewport), S2 1.1x / 1.5x (our 1.42 s includes a 0.49 s OptiX GAS build). With the naive `loop_triangles` path **no**: S2 is 2.5x / 3.3x (fail), S1 1.7x / 2.6x. Caveat: ours omits shader, texture and light sync; Cycles' log says 307 shaders and 50-70 ms of C++ data sync for S2, so the real gap will be a little larger than shown. Not yet a pass for the full product; a pass for the mesh/instance part, which is where v1 blew up.
- **Which present path:** the persistent texture + `GPUStorageBuf.update` + compute unpack (path B) with a source format that keeps each update under 32 MiB: **packed R11G11B10F (4 B/pixel) at 4K, RGBA16F half-float words (path B16) at 1080p**. Path A (`GPUTexture(data=)` per frame) is the simple baseline: RGBA32F is fine at 1080p (1.5 ms, 60/s) and weakest at 4K (11 ms, p99 29 ms); never use RGBA16F from a float32 Buffer (4x slower than RGBA32F at 1080p). RGBA8 display-referred (C) is as cheap as R11G11B10F but cannot carry HDR and the view transform then double-applies; I see no reason to use it.
- **Fixed by Blender:** the ~6 ms idle cost per redraw (and the 0-6 ms wake-up jitter, median 3 ms); that textures only take FLOAT Buffers and convert on the CPU; the 32 MiB SSBO-update cliff; int-attribute `foreach_get` at ~25 ns/element and `loop_triangles` at 50-120 ns/triangle; the dtype-mismatch slow path (30-150x); depsgraph evaluation (0.6 s for S2 at load, 285 ms after adding one object) and the 9,007-entry update list; no `UBYTE`/half-float texture upload.
- **Ours to engineer:** use the attribute fast paths and native triangulation (3.5x faster on S1, 2.9x on S2, cold); a native arena for mesh copies (cold native copy 146 ms vs 23 ms warm); GAS build (430 ms for 2,000 meshes, ours to parallelise or cache); per-instance transform extraction (30 ms per 10 k instances); the upload format and chunking; dirty-state handling for update storms.

## 5. Recommendations

Sync:
1. Read meshes only through float-vector attributes with exact dtypes (`position`, corner normals, UV: memcpy-class), and ship **raw topology (corner_vert, face start offsets) to native code and triangulate there**. Do not use `loop_triangles` or per-element RNA. Allocate scratch arrays once, slice them (`arr[:n]`) for `foreach_get`.
2. The only remaining Python-bound cost is reading int topology (~25 ns/corner). Cache topology per mesh and re-read only positions/normals/UV (1 ms for 1 M triangles) when the update flags show shading/transform/position-only changes; re-read topology when counts change. Whether topology is unchanged cannot be known from `depsgraph.updates` (a vertex edit is flagged geometry+transform), so key the cache on counts plus an occasional hash, or accept a re-read (65 ms for 1 M tris with the fast path). An unsafe route (ctypes reads of `Mesh` DNA via `as_pointer()`) was **not tested**.
3. Key meshes by `ob.data.original.session_uid` (or the original name), never by the evaluated mesh's `session_uid`.
4. Per-instance loop: `for inst in dg.object_instances` with `inst.object`, `.data.original.session_uid`, `np.asarray(inst.matrix_world, np.float32)` into a preallocated `(n,16)` array: ~3 us/instance, 30 ms for 10 k. Record object name -> instance index for non-instanced objects at full sync, so moves resync in 0.1 ms; instancer moves (collection instances, geometry nodes) need a re-iteration (5-8 ms at 12 k instances).
5. Treat `depsgraph.updates` as noisy: a material edit produced 33 entries, adding one object produced 9,007. Classify by ID type + flags, dedupe by original datablock, and ignore Scene-only/selection updates.
6. Build the GAS in batches with one stream sync (S2: 2,000 builds in 430 ms, compaction 27 ms, TLAS 5 ms; compacted size is 36% of raw); first-frame budget for 5 M triangles is dominated by the GAS build plus mesh reads, not by Python overhead.

Present:
1. One persistent texture + persistent SSBO + compute unpack + `bind_display_space_shader`-style draw; update only when the engine has a new frame (redraw loop stays at ~165/s, uploads follow the producer).
2. Choose the upload format so one `GPUStorageBuf.update` is **below 32 MiB**: 1080p RGBA16F (16.6 MB) fine; 4K needs 4 B/pixel (R11G11B10F or shared-exponent) or two <=32 MiB halves. Precision of R11G11B10F (6/5 mantissa bits, no alpha, no negatives) is **not evaluated** beyond exact decode against the truncating encoder; banding must be checked before adopting it as default.
3. Resolution policy: the data say dynamic resolution is needed only above 1080p with 16-bit-per-channel uploads. Drop to the lower of (render resolution, 1080p-class) when the measured redraw interval exceeds 41 ms (24/s floor) or 33 ms (30/s); at 1080p no reduction is needed on this machine for any path.
4. Keep the native producer thread and presenter decoupled: the worker is not stalled by the GIL; 4K uploads cost it 10-30% of its throughput through memory contention.
5. Re-measure on a 4K display and with the owner's real viewport load (overlays, other editors); these runs use an 818x556 or 1920x1080 region with overlays off.

## 6. Measured vs inferred, not measured, surprises

Measured (script and JSON named above): every table in 1.1-1.3, 2.1-2.3, 3 (Cycles log and render times), the gpu API behaviours in 1.4, the 32 MiB SSBO cliff (3 sweep runs), OptiX GAS/TLAS times.

Inferred (not confirmed): Blender's ~5 ms idle sleep as the cause of the ~6 ms redraw floor; GPU/PCIe power states as the cause of slower sparse uploads at 24-60 fps paced; "first-touch" as the cause of cold native-copy cost; that the instance order/keys behave the same for modifiers that create per-object evaluated meshes (not tested).

Not measured: a real 4K viewport/display; OpenGL backend; HDR output; `view_draw` while the owner's Blender is rendering; the GIL being held inside gpu calls; shader/material/light/texture sync cost; edit-mode edits (edits were API edits in object mode); Cycles viewport pixel-level first-frame detection (failed, see 3); the `as_pointer()` direct-memory read; R11G11B10F banding; CPU-side `QueryPerformanceCounter` agreement with Blender's own timers beyond the checked 8 us clock offset; any run on the iGPU. The OptiX part was done but only for triangle GAS + instance TLAS with transforms taken from Blender matrices; no SBT, no shading.

Surprises: (1) evaluated-mesh `session_uid` collapses to 2 values for 2,001 meshes; (2) RGBA16F texture creation is 4x slower than RGBA32F; (3) 32 MiB cliff in `GPUStorageBuf.update`, with a 4K 4-byte-per-pixel frame just under it; (4) `Buffer` accepts mismatched numpy dtypes silently; (5) texture and view_update costs are not additive with a fixed Blender loop cost: ~6 ms per redraw regardless; (6) adding one object to the 10 k-object scene costs Blender 285 ms and a 9,007-entry update list; (7) the dtype-mismatch cost (corner normals 117 ms vs 0.76 ms) is bigger than any structural cost in the sync; (8) `Material.use_nodes`/`World.use_nodes` already emit deprecation warnings in 5.3 (noise in logs only).

## 7. Files

`run_all.ps1` (all numbers; `-Only build|scenes|sync|cycles|gui|edits|cycview|tables`), `run_gui.ps1` (Q1 groups), `lib.ps1`/`env.ps1`/`build.ps1`, `native/` (DLL source), `sbn.py` (ctypes), `gui_common.py` (engine + window harness), `gui_present.py`, `gui_probe.py`, `probe_gpu_api.py`, `gui_edits.py`, `gui_cycles_viewport.py`, `gui_screenshot.py` (+ the screenshot is taken by the PowerShell snippet that launches it and calls `CopyFromScreen` on the window rect of its own PID), `make_scenes.py`, `syncmod.py`, `sync_bench.py`, `probe_fastpaths.py`, `cycles_bench.py`, `parse_cycview.py`, `tables.py`, `selftest.py`; results in `out/` (`tables.md` is the full set).

