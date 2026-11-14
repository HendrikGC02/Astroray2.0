# 0007 appendix — C ABI descriptor field lists

Part of record 0007 (decision 7; ruling 33). It gives the field list of every descriptor in 0007 decision 5's function list. Conventions, types, annotations and the header grammar are 0001 D2; `ar_context_desc` and the report structs are 0001's (D2.1, D2.11); what the readback functions return is 0008 decision 13a. Exact widths and field order are fixed when `astroray.h` is written, within 0001 D2.2.

Host-filled arrays are in brackets. Each `ar_*_buffers` struct holds one `AR_STAGED` pointer per bracketed array of its descriptor, in the same order (0001 D2.3). Matrices are 3x4 row-major float32 object-to-world. Keys are `ar_key` (0001 D2.2).

## Scene, session, progress, settings

| Descriptor | Fields |
|---|---|
| `ar_scene_desc` | cache_dir (UTF-8, OptiX disk cache, decision 45); worker_threads (0 = min(8, logical cores / 2), decision 23). The device is the context's (0001 D2.1, ruling 33). |
| `ar_session_desc` | mode VIEWPORT/FINAL/PREVIEW; max width and height (the largest film the session can use, 0008 decision 1); denoiser NONE/OIDN/DLSS_RR (0008 decision 16) |
| `ar_progress` | state (INITIALIZING, RENDERING, PAUSED, CONVERGED, TRIMMED, CLOSED, FAILED); active, pending and failed generation and the current epoch (decision 25); samples per pixel; last pass time; ledger bytes per category and device free/total bytes (decision 14) |
| `ar_settings` | output size, resolution percentage and border (final); samples and viewport sample limit (0004 D25); adaptive threshold and min samples (0004 D16); bounce limits total/diffuse/glossy/transparent/transmission/volume and min bounces (0004 D2-D3); seed and animated-seed flag (0003 decision 23); direct and indirect clamp, Filter Glossy, reflective and refractive caustics toggles (0004 D17); pixel filter type and width (0008 decision 5); film exposure and film transparent (0008 decisions 7, 19); working-space interop id (0003 decision 15); requested passes, light-group count, shader AOV names, Cryptomatte levels and types (0008 decisions 10-11); response channels as copy-in curves with unit ENERGY/PHOTON, cube bins, exposure time and collecting area (0008 decisions 7, 9); denoise start sample (0008 decision 18); allow_spill (decision 14) |

## Geometry, images, volumes

| Descriptor | Fields |
|---|---|
| `ar_mesh_desc` | key, name, V, C (corners), F, uv_layer_count U, motion_steps S (1 unless deformation blur), flags (HAS_MATERIAL_INDEX, HAS_ATTRIBUTES), attribute list (name, domain POINT/CORNER/FACE, float width 1-4); [positions f32 S×V×3, corner_vert i32 C, face_start i32 F, corner_normals f32 C×3, uv f32 U×C×2, material_index i32 F, attributes f32] |
| `ar_positions_buffers` | for `ar_edit_positions_begin` (decision 22): [positions f32 S×V×3, corner_normals f32 C×3] of the key's existing V and C |
| `ar_instances_desc` | N, motion_steps S; [geom_key u64, xf f32 S×12, object_key u64, random_id u32, pass_index u32, crypto_object u32, crypto_asset u32 (MurmurHash3 of the object and asset names, computed by the addon, which also writes the manifest; 0008 decision 11), color f32×4, visibility u32, light_group u8 (0008 decision 10), material_table u32 offset]; plus pool [material keys u64]. Visibility bits: camera, diffuse, glossy, transmission, shadow, volume (mapped onto 0002 D11's instance mask), then instance flags read in shading or by the light pass, not mask bits: holdout, shadow catcher, caustic-caster opt-out (0004 D19). No light or shadow linking sets (ruling 45). |
| `ar_curves_desc` | key, P points, K curves, type (linear/Catmull-Rom/B-spline); [positions f32 P×3, radius f32 P, curve_start i32 K, attributes] |
| `ar_points_desc` | key, P; [positions f32 P×3, radius f32 P, attributes] |
| `ar_grid_desc` | key, grid name, channels 1/3, background value, index-to-object 3×4, object-to-world 3×4 (ruling 43), leaf count n; [leaf origins i32 n×3, voxels f32 n×512×channels]; one call per batch, batches of one grid append (leaf format and engine representation: 0006 D5) |
| `ar_image_desc` | key, source FILE (UTF-8 path) / MEMORY (packed bytes, copy-in) / PIXELS (f32 RGBA, staged), colour interop id, alpha mode, UDIM tile number, width, height |

## Shading, lights, world, camera, astrophysics

| Descriptor | Fields |
|---|---|
| `ar_graph_desc` | node count, [op nodes {op u16, param range}], [param pools f32, i32, UTF-8 strings], [links {from node, socket, to node, socket}] (decision 8; ops 0005 item 30); the target SURFACE/VOLUME/DISPLACEMENT/LIGHT/WORLD is the call's argument |
| `ar_material_desc` | graph keys for SURFACE, VOLUME, DISPLACEMENT (0 = none); ext_kind and its 64 B constant block (0005 items 14, 16); medium_priority u8 (0005 item 2a); pass_index u32 (IndexMA) and crypto_material u32 (MurmurHash3 of the material name, as for objects) (0008 decisions 10-11); emission sampling setting (0005 item 43); bump-correction setting (0005 item 8); displacement method (0005 item 37) |
| `ar_light_desc` | type POINT/SPOT/SUN/AREA_RECT/AREA_ELLIPSE, xf, size_x, size_y, spot_size, spot_blend, sun_angle, spread, power (W, or W/m² for sun, exposure applied; passed through unscaled, 0003 decision 11), colour as `ar_spectrum_ref` (below), normalize, visibility, cast_shadow, graph key, light_group u8 (0008 decision 10) |
| `ar_spectrum_ref` | kind RGB/BLACKBODY/TABLE (the host-authored kinds of 0005 item 9), rgb f32×3, temperature K f32, scale f32, table count n with [wavelength nm f32 n, value f32 n] copy-in (`AR_IN_ARRAY`); the core packs it into 0005's 24 B `SpectrumRef` with the slot's semantic (ILLUMINANT for lights, 0005 item 10). IES profiles cross in the light's graph (0005 item 41) |
| `ar_world_desc` | graph keys for WORLD and VOLUME; light_group u8; mist start, depth and falloff (0008 decision 10) |
| `ar_camera_desc` / `ar_view_desc` | type PERSP/ORTHO/PANO, cam_to_world, projection_inverse 4×4 (NDC to camera space, taken from Blender), near/far, DOF (fstop, focus distance, blades, rotation, ratio), panorama params, shutter open/close + motion steps; view adds region w/h, render scale, border |
| `ar_astro_desc` | astro_kind (BLACK_HOLE), xf (spin axis = local +z), mass, spin a, meters_per_rg (0006 D25); θ_tol and ε_NEE overrides for science renders, 0 = default (0006 D25, D28). No camera velocity exists anywhere in the ABI, so the camera is at rest in the hole's static frame (decision 41, 0006 D24) |

## Display and readback

| Descriptor | Fields |
|---|---|
| `ar_display_frame` | frame_id, generation, width, height, format (RGBA16F or DIAG_RGBA8), row bytes, data pointer (pinned host ring slot, valid until release) |
| `ar_pass_info` | name (UTF-8: Blender pass name or `AR ` name, 0008 decision 10), component count, unit ENERGY/PHOTON/NONE, width, height, flags (DENOISED, 0008 decision 17); rows bottom-up (0008 decision 1) |
