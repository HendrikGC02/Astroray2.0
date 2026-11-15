# 082 Lamp, camera, view and settings sync
Goal: lamps, camera and render settings do not reach the engine -> point and area lamps, the perspective camera and viewport view, and the M1a render settings are synced with their exact Blender meanings, and engine camera rays match Blender's projection.
Read first: 0007 d20 (classification order), d49 (view hash), d37 (native properties read directly), docs/decisions/0007-appendix-abi.md (`ar_light_desc`, `ar_spectrum_ref`, `ar_camera_desc`/`ar_view_desc`, `ar_settings`), 0003 d11 (Blender watts pass through, factor 1), d15 (working space refuses other ids), 0007 V18.
Scope: blender/astroray/sync/lights.py, sync/camera.py, sync/settings.py, blender/astroray/properties.py, blender/astroray/__init__.py (register properties only) (`scene.astroray` with only the settings M1a uses), tests/blender/test_camera_ndc.py, tests/blender/headless/test_lights_sync.py.
Out of scope: spot and sun lamps, light node trees, IES (M4: such a lamp is not rendered and a visible report line names it); the world (083, which owns the graph emitter); thin lens, orthographic and panoramic cameras (reported by the engine as CAMERA_UNSUPPORTED, 053).
Reference: Cycles `intern/cycles/blender/camera.cpp` and `light.cpp` (Apache-2.0) for Blender's camera and lamp parameter meanings.
Do:
1. lights.py: POINT (`shadow_soft_size` as radius), AREA SQUARE/RECTANGLE/DISK/ELLIPSE (`size`, `size_y`), `energy` passed unscaled, colour as RGB `ar_spectrum_ref`, `normalize`, ray visibility, `cast_shadow`, light group; SPOT and SUN are skipped with a visible report line "lamp type <t> lands in M4".
2. camera.py: `cam_to_world` from the camera object's matrix (scene-origin folded in), `projection_inverse` from Blender's projection for the render size (F12) or the region (viewport: `region_data.window_matrix`/`view_matrix`), shift and sensor fit included; type, DOF and panorama fields filled so the engine can report what it does not support; view desc and the view hash (view matrix, projection, region size, camera-view state, border) for `set_view` only when the hash changes.
3. settings.py: samples (render and viewport), seed and animated-seed flag, max bounces (total) and min bounces, pixel filter type and width, film exposure, working-space interop id from `bpy.data.colorspace.working_space_interop_id`.
4. tests/blender/test_camera_ndc.py (Invoke-ArBlender, CPU engine through `_abi.py`): for persp, shift_x, sensor fit horizontal and vertical, portrait, and the viewport camera view, sync a large plane facing the camera, render the position AOV at 64x48 with the gate-build hook `ASTRORAY_TEST_PIXEL_CENTRE=1` (zero filter offset; engine side in 054 and 101), map each pixel's AOV position through `world_to_camera_view`, and compare with that pixel centre's NDC: difference <= 1e-5 (0007 V18, perspective cases).
Acceptance: the headless and Blender tests pass; `check ci` passes.
Look at: none.
Canary: tasks/082.canary.patch must turn red: tests/blender/test_camera_ndc.py "TestCameraNdc.test_shift_x".
Budget: none.
Depends on: 081, 068, 053, 071, 117 (only test_camera_ndc.py).
Size: M (about 400 lines Python); stop and report at 2x.
Progress:
- Blocked before coding: Session::Session (src/host/session/session.cpp:53) builds FilmRequest with only statistics; ar_settings.passes/aov_names are copied (scene/edit.cpp:168) but never reach the layout, so no position AOV is readable from a session; the NDC test (step 4) needs it. Needs an engine change outside Scope.
