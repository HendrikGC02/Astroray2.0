# 054 generate_camera and the active-pixel list (designed when built)
Goal: no path is ever started -> `generate_camera` fills refilled main-pool slots with camera paths (slot, pixel and sample from the active-pixel list, filter offset, film and transport wavelengths, hero lane, camera ray, initial state) on CPU and GPU, and the ownership of the active-pixel list is decided and written into 0008 and 0002.
Read first: R63 (active-pixel list ownership designed when built; compaction in M4), 0008 d3 (slot assignment j*k + m, list readable until the pass drains), 0003 d2, d6, d19, d23 (keys, dims 0-7, initial fields), 0002 D7 step 4 (refill), D8 (generate_camera row); src/kernel/camera/camera.h.
Scope: src/kernel/stages/generate_camera.h, src/kernel_cpu/generate_camera.cpp, src/kernel_gpu/generate_camera.cu, src/kernel/stages/film_build_active.h, src/kernel_cpu/film_build_active.cpp, src/kernel_gpu/film_build_active.cu, src/host/film/active_pixels.{h,cpp}, src/kernel/kernel_globals.h (`SessionUniforms.test_hooks`), src/kernel/launch_config.h, src/kernel/budget.toml, docs/decisions/0008-film-outputs-denoise.md (d3 text), docs/decisions/0002-kernel-architecture.md (D8 per-pass kernel table), tests/cpu/test_generate_camera.cpp, tests/gpu/test_generate_camera_agree.cpp.; src/kernel/film/camera_wavelengths.h (lead: one source for the wavelength recomputation, 054 review); src/kernel/film/resolve.h (that call only); docs/decisions/0004-integrator.md (D16 sentence only)
Out of scope: calling film_build_active and generate_camera from the session and scheduler, and the resolve's use of the list (101); adaptive sampling and list compaction (M4); motion time (M8); light paths (M5).
Reference: Akenine-Moller et al. 2021, "Improved Shader and Texture Level of Detail Using Ray Cones", JCGT 10(1) (initial cone spread from the pixel angle).
Do:
1. Designed when built (R63): the film owns the active-pixel list (u32 pixel indices, capacity = allocated pixels, film ledger category) and its count `u`; a per-pass kernel `film_build_active` writes it at pass start (in M1a every pixel of the render sub-rectangle; M4 adds compaction here); k = 1 outside the adaptive tail. Write this into 0008 d3 and add `film_build_active` to 0002 D8's per-pass kernel table in this diff.
2. generate_camera over the refill launch: item i of the refill gets sample_id = work base + i; pixel = list[sample_id / k], sample index n + sample_id % k; dims 0-1 filter offset (048; offset 0 when `SessionUniforms.test_hooks` has PIXEL_CENTRE, set by 101 from `ASTRORAY_TEST_PIXEL_CENTRE` for 0007 V18 in task 082), 2-3 lens (unused by the pinhole), 4 time (not stored without motion), 5 film wavelength u -> lambda_t = lambda_f (024), 6 hero lane -> flags bits 7-8; PATH_KIND camera; beta = 1, r_u = r_l = 1, eta_scale = 1, bounces 0, event_index 0, ray_type CAMERA; camera ray (053); cone width 0 and spread from the pixel's angle; prev_* empty; term ACTIVE; next_stage INTERSECT_CLOSEST.
3. Rows in launch_config.h and budget.toml for both kernels.
4. Tests: CPU (fast): slot formula and pixel coverage for a 7x5 film over 3 passes; the resolve's recomputed (lambda_f, p_f) equals what generate_camera used for every slot (0003 check 26, first half); GPU (gate): the per-stage fixture through the agreement harness (029): directions (p99.9 <= 1e-5 rad nofma vs CPU float), integer fields bitwise.
Acceptance: `pwsh tools/check.ps1 gate` passes; kernelgate rows exist; `out/bin/astroray_tests_gpu.exe --tier=gate -tc="generate_camera/*"` prints the direction p99.9 and max.
Look at: none.
Canary: tasks/054.canary.patch must turn red: astroray_tests_cpu "generate_camera/resolve recomputes same wavelengths", astroray_tests_gpu "generate_camera/agreement".
Budget: two device TUs; declared spills 0, stack <= 128 B.
Depends on: 052, 053, 048, 024, 029.
Size: M (about 400 lines plus record text); stop and report at 2x.
Progress:
- Active-pixel list: film-owned (host/film/active_pixels), written by film_build_active, read-only for the rest; text in 0008 d3 and 0002 D8.
- KernelGlobals: active_pixels is now non-const (one writer, film_build_active); camera and filter pointers added after display; SessionUniforms.test_hooks.
- generate_camera.h stores in groups and skips the time draw without motion: 37 regs, stack 128 B, 0 spills (budget rows measured).
- Cone spread = atan(film_extent.y / height) (JCGT 2021 pixel angle, shift folded in). ray_type = 0x1 (AR_VISIBILITY_CAMERA).
- GPU agreement: ray_d, lambda_t, cone max 0 on 10^6 samples; canary needs a CUDA-only breakage for the GPU case.
