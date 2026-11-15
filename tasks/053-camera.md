# 053 Perspective camera with We, PDF_We and sample_wi (designed when built)
Goal: no camera model exists -> the perspective (pinhole) camera generates rays from Blender's camera-to-world and inverse projection, implements importance `We`, `PDF_We` and `sample_wi` with the signatures every later camera type will share, reports unsupported camera features, and the decided model is written into 0004.
Read first: R63 (camera and lens models designed when built; perspective in M1a), 0004 D7 and D23 (camera importance signatures, light-path connections), 0003 d9 (metres), d26 (scene origin), docs/decisions/0007-appendix-abi.md (`ar_camera_desc`, `ar_view_desc`), 0007 d36 (camera features covered).
Scope: src/kernel/camera/camera.h, src/kernel/camera/perspective.h, src/host/scene/camera.{h,cpp}, docs/decisions/0004-integrator.md (D23 text), docs/decisions/0001-product-shape.md (one report code row), include/astroray/astroray.h (the new code), tests/cpu/test_camera.cpp (+ f64).
Out of scope: thin lens, orthographic and panoramic rays (M4) and their importance (M5); motion blur (M8).
Reference: pbrt-v4 `src/pbrt/cameras.h` and `cameras.cpp` `PerspectiveCamera::GenerateRay`, `We`, `PDF_We`, `SampleWi` (Apache-2.0), adapted from pbrt's screen window to Blender's NDC-to-camera inverse projection; Veach 1997 ch. 10 (We definition).
Do:
1. Designed when built (R63): the camera is pinhole; rays start at the camera origin through the film point given by `projection_inverse` (NDC to camera space, from Blender) and `cam_to_world`; film area A on the plane z = -1 in camera space defines We = 1/(A cos^4 theta) inside the frustum (pbrt's normalisation); write this decision, its formulas and the pbrt reference into 0004 D23 as the perspective entry.
2. camera.h: `CameraSample {film (x, y) in pixels, lens (u, v), time}`, `generate_ray(cam, sample) -> {origin, direction}` and the importance trio with pbrt's semantics, as one struct `CameraView` filled on the host from the descriptors (scene origin folded in, 0003 d26).
3. Unsupported features fail loudly: camera type ORTHO or PANO, or depth of field enabled, raise a new report code `CAMERA_UNSUPPORTED` (WARNING, SCENE; subject = camera key; detail = feature; rendered as a pinhole perspective) added to 0001 D2.11's table and to astroray.h and report_codes.def (R63, missing report codes).
4. Tests (fast): rays through the four film corners match `projection_inverse` applied to NDC corners within 1e-6; `sample_wi` from a scene point returns the pixel that `generate_ray` maps back to (1e-5 NDC) and its pdf equals `PDF_We`; We integrates to 1 over the film by quadrature (1e-4); a shifted projection (Blender shift_x) round-trips.
Acceptance: `pwsh tools/check.ps1 fast` passes with `camera/*` cases in float and double; 0004's diff shows the perspective entry citing this task; 0001's table has the new row.
Look at: none.
Canary: tasks/053.canary.patch must turn red: astroray_tests_cpu "camera/sample_wi round trip", "camera/We normalised".
Budget: none.
Depends on: 019, 016, 032.
Size: S (about 250 lines plus record text); stop and report at 2x.
Progress:
- 053: perspective camera done; canary tasks/053.canary.patch (cos^3 We, swapped film axes). Choices: pdf = d^2/cos; film y up; subject 0 camera key per call.
