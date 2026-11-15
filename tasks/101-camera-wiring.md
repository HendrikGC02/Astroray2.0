# 101 Camera paths in the session: active list, refill and resolve wiring
Goal: generate_camera and film_build_active exist but nothing calls them, and the resolve still treats active pixel j as pixel j -> each pass builds the active-pixel list, the scheduler's refill launches the real generate_camera, the resolve reads the list, and gate-build test hooks reach the kernels through SessionUniforms.
Read first: 0008 d3 (active list, slot j*k + m) as written by task 054; 0002 D7 step 4 (refill), D8 (per-pass kernels); src/host/scheduler.h; src/host/session/session.h; src/kernel/film/resolve.h.
Scope: src/host/session/session.cpp, src/host/scheduler.cpp, src/kernel/film/resolve.h, src/host/session/test_hooks.{h,cpp}, src/kernel/kernel_globals.h (SessionUniforms), tests/cpu/test_camera_wiring.cpp, tests/gpu/test_film_resolve_gpu.cpp; added by lead ruling: src/host/session/session.h (per-session list, camera, filter, hook members), src/host/device.h and src/device/cuda/cuda_device.cpp (KernelId::FILM_BUILD_ACTIVE and its name), src/device/cpu/cpu_device.cpp (registration lines and the generate_camera adapter), src/kernel/film/present.h and src/kernel/film/film_write.h (j to pixel through the list); test fixtures tests/cpu/test_film_resolve.cpp, test_film_stats.cpp, test_present.cpp, tests/gpu/test_present_gpu.cpp (set kg.active_pixels only).
Out of scope: shading stages (103, 071); CUDA upload of scene tables (102); adaptive sampling and compaction (M4).
Reference: none.
Do:
1. At pass start the session launches `film_build_active` (054) before the scheduler; the scheduler's refill launches `generate_camera` over the refill count with the work base, replacing 045's stub in production builds (stubs stay test-only).
2. film_resolve takes its pixel from the active list (thread j -> list[j]) instead of pixel j, on CPU and GPU; so do the other callers of `film_slot_pixel`/`film_active_pixel` in src/kernel/film/film_write.h (`film_path_end_stats`, `film_primary_aov`), so no stage maps j without the list (054 review; film_write.h joins Scope for that).
3. test_hooks: in builds with `AR_GATE_HOOKS` the session reads environment variables `ASTRORAY_TEST_<NAME>` once at create and sets fields of `SessionUniforms.test_hooks`; this task defines PIXEL_CENTRE (read by 054's kernel); later hooks add one row each (NEE mode 106, pool sizes and radiance scale 076, GPU fault 099); the CLI prints active hooks in its report. Release builds compile the reader out.
4. Tests: test_camera_wiring.cpp (gate, host classes, CPU device, empty scene, a test stub registered for SHADE_BACKGROUND that adds 1 and terminates): a 7x5 film over 3 passes at 1 spp gives sample count 3 in every pixel; with a 3x2 render sub-rectangle exactly those 6 pixels count; PIXEL_CENTRE set through the environment gives filter offset 0 for every slot. GPU (gate): film_resolve with a permuted active list folds each slot into list[j].
Acceptance: `pwsh tools/check.ps1 gate` passes with `camera_wiring/*`; the test prints the per-pixel sample counts of the 7x5 film.
Look at: none.
Canary: tasks/101.canary.patch must turn red: astroray_tests_cpu "camera_wiring/every pixel sampled", astroray_tests_gpu "film_resolve_gpu/active list order".
Budget: film_resolve register delta declared (expected 0); host otherwise.
Depends on: 054, 052, 049.
Size: M (about 250 lines); stop and report at 2x.
Progress:
- Stopped before code: needs files outside Scope (see report to lead): session.h, src/host/device.h + src/device/cuda/cuda_device.cpp (KernelId::FILM_BUILD_ACTIVE), cpu_device.cpp wrapper; CameraView/FilterTable source undefined.
