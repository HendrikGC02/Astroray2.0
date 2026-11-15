# 047 FilmLayout, per-sample buffer, sample_add and film_resolve
Goal: contributions have nowhere to land -> a `FilmLayout` allocates only requested channels, every camera contribution goes through `sample_add` into a per-sample fp32 slot, and `film_resolve` folds each pass into fp64 first and second moments (and the class-A sum) with one writer per pixel and no atomics, on CPU and GPU.
Read first: 0008 d1 (one film, layout, rows bottom-up, allocation at the largest size), d2 (channel kinds), d3 (buffer, slots, writes, resolve, interrupted pass, moments statistics), d4 (SPLAT fold), d6 (invalid samples), d21 (ledger), 0008 V2; R1.
Scope: src/kernel/film/film_write.h, src/kernel/film/resolve.h, src/kernel_cpu/film_resolve.cpp, src/kernel_gpu/film_resolve.cu, src/host/film/film_layout.{h,cpp}, src/kernel/launch_config.h, src/kernel/budget.toml, tests/cpu/test_film_resolve.cpp, tests/gpu/test_film_resolve_gpu.cpp.
Out of scope: statistics and AOV channels (049); FITS (050); present (051); light-pass splats beyond the fold (M5 produces them; the plane and fold exist now).
Reference: Chan, Golub and LeVeque 1983 (variance from sums, cancellation); pbrt-v4 `RGBFilm::AddSplat` (Apache-2.0) for the SPLAT footprint semantics.
Do:
1. FilmLayout: channels {name, kind MOMENT|ADD|COUNT|CRYPTO|SPLAT, components, offset}; unrequested channels get offset -1 and no memory; planes SoA, one per component, rows bottom-up, sized at the session's maximum resolution with a sub-rectangle in use; bytes reported to the ledger (film category). Display MOMENT always; S1A only when adaptive sampling is on (off in M1a); SPLAT only when the light pass runs.
2. film_write.h: `sample_add(kg, slot, const Spec&)` (fp32 atomic add into the slot's L[4]); `sample_set_flags(kg, slot, flags)` at path end (SECONDARIES_TERMINATED, invalid bit); typed `film_add`, `film_count_add`, `film_count_max` for ADD and COUNT channels; `film_splat_add(kg, pixel, value)` for the SPLAT plane (producers arrive in M5); nothing else writes film memory.
3. film_resolve (one thread per active pixel; until task 101 wires the active-pixel list, active pixel j is pixel j of the render sub-rectangle): for m = 0..k-1 read slot j*k + m; skip and count an invalid sample (NaN, Inf or any negative lane); recompute (lambda_f, p_f) from the sample key (021, 024); apply `film_weights` and 1/s (radiance scale) in double; convert to every MOMENT channel with 046; add x, x^2 (and x to S1A when n + m is even) with plain fp64 read-add-write; increment n; fold the pixel's SPLAT value; clear slots and splat entry. Per-pass kernel row in launch_config.h and budget.toml.
4. Host statistics helpers: mean = S1/n; variance of the mean = (S2 - S1^2/n) / (n(n-1)) in double.
5. Tests: CPU (fast) and GPU (gate) through sample_add and resolve on a 1x1 film: 3e7 samples of 1.0 give S1 = 3e7 exactly; 2^24 samples of 0.1 give S1 within 1e-6 relative; iid uniform and exponential samples give a variance estimate inside its 99.9% interval; a k = 4 tail pass equals four single-sample passes; an injected NaN slot is skipped and counted; a test stub writing SPLAT entries for three pixels through `film_splat_add` (the roadmap's stub feed) is folded into those pixels once and cleared; CPU single-thread runs twice bit-identical (0008 V2 parts that need no scene).
Acceptance: `pwsh tools/check.ps1 gate` passes with `film_resolve/*` on both devices; kernelgate shows film_resolve with 0 spills.
Look at: none.
Canary: tasks/047.canary.patch must turn red: astroray_tests_cpu "film_resolve/3e7 ones exact", "film_resolve/nan skipped and counted", "film_resolve/splat folded once".
Budget: one device TU; declared spills 0, stack <= 128 B.
Depends on: 045, 046, 021, 024.
Size: M (about 450 lines); stop and report at 2x.
Progress:
- Done: layout, sample_add/film_* writers, resolve (CPU+GPU one source), host stats helpers, tests, canary.
- Outside Scope, needed: cmake/kernels.cmake lists film_resolve in AR_GPU_F64_MODULES (the fp64 ptxas warning is an error otherwise); src/kernel/sample/sobol.h keeps one table row per lane (the 4x32 local cost 512 B of stack, budget 128 B).
- k_film_resolve launch row uses B=1: with B=0 ptxas capped at 80 regs and spilled 8 B.
- GPU large-sum test uses 5e5 x 61 (one GPU thread resolves ~7 us/sample); the CPU test runs the full 3e7 ones.
- Active pixel j is pixel j of the rect (no active list yet); CpuDevice does not register FILM_RESOLVE (cpu_device.cpp not in Scope).
