# 0002: review and reconciliation

Moved unchanged from the end of `docs/decisions/0002-kernel-architecture.md` (ruling 86). History only: nothing here is binding (ruling 50), and the record's Decision section is current.

## Review and resolution

History of the independent review; the Decision section and the Reconciliation section below supersede it where they differ (for example "nightly", now `check full` on demand).

1. BLOCKER, D10 gate and ladder: **ACCEPTED.** Added challenge C2 (it absorbs the former C2 and is the reviewer's C4). The merge gate now checks production spills and stack, and uncapped registers are a nightly warning. The ladder re-tunes the cap before any split, and every split must beat the re-tuned cap.
2. MAJOR, D8 extension stage: **ACCEPTED.** `ShadeKind` is now a flat enum that can grow, one TU per value. 0005 partitions the extension kinds into identities.
3. MAJOR, D5/D6 shadow pool: **ACCEPTED.** Atomic slot allocation is replaced by per-producer reservation from the deterministic free list, computed in the step table. Producers that run short of slots launch on a prefix, so D7 step 5 is consistent. The permutation test compares shadow items as a multiset. `shadow_compact` is removed. The Alternatives entry is corrected.
4. MAJOR, D11 any-hit: **ACCEPTED.** The v1 citation is corrected. Shadows use one pass with the Cycles-style recording any-hit (no material code in OptiX), and camera rays pass through alpha in `shade_surface`. The mask bit is replaced by the instance any-hit flag, the pipeline flags are made consistent with Risks 5, and an alpha-foliage benchmark is added to M1.
5. MAJOR, light pass in M1: **ACCEPTED** for mechanics (`path_kind`, routing, shared pool and refill, end-of-pass, a stub scheduler test in M1, and a budget row for the light kernels). The method and the light-sample count stay with 0004, per spine §4.
6. MAJOR, device failures: **ACCEPTED.** Added D15, which gives one contract per case, plus failure-injection tests.
7. MAJOR, gate time: **ACCEPTED.** The record now lists what the merge gate runs, with a 120 s kernel share. The probe, NVRTC identity and all-TU timing moved to nightly. Shipped architectures get a nightly compile-only check and a runtime warning on untested GPUs; whether to refuse them instead is owner question 3.
8. MINOR, refill rule: **ACCEPTED.** Changed to "active < 1/4 of capacity", which is Cycles' rule.
9. MINOR, VRAM arithmetic: **ACCEPTED.** Corrected to about 1.05 GiB, plus 320 MiB of hit records when non-opaque instances exist.
10. MINOR, D9 contradiction: **ACCEPTED.** Removed the "8 registers / minority" trigger. There is now one A/B rule with two pass criteria.
11. MINOR, templates on N: **ACCEPTED.** N is now `constexpr kLambdas = 4`.
12. MINOR, OptiX work and step overhead: **ACCEPTED.** Raygen code is limited to `route_hit` and is gated through `k_route_hit`. OptiX programs get nightly `budget.toml` rows, so the D8 check is consistent. The unsourced "20-40 steps" is replaced by a derivation and an overhead estimate (20-40%, above budget). Partitioning is one pass over both pools' high-water marks. Kernels read counts on the device so that a CUDA graph is a host-only remedy. Whether spike C must run first is a lead question (Risks 2).

## Reconciliation (rulings applied)

- G1: "What lands when" replaced by the must-exist-first and can-come-later lists; the D8 "Lands" column and every M1/first-release assignment elsewhere removed.
- G2: time budgets (compile, build, edit loop, context ready, overhead shares, per-sample time, launch duration, gate share) moved to 0009 by name; VRAM reserve and pool arithmetic to 0007; path-state and shadow-item budgets to 0003; A/B protocol to 0009; per-render report to 0001.
- G3: every "nightly" item is now part of `check full`, run on demand.
- G4: the geodesic enum value and routing bit hook deleted; light-path mechanics kept because a second path kind changes the state layout.
- Ruling 1: path state carries no radiance; contributions go to 0008's per-sample buffer (`sample_add`, formerly `film_add`), resolved by `film_resolve` with one writer per pixel; D7, D12 and the 0008 assumption updated.
- Ruling 2: camera-connection shadow items carry a film position splatted over 0008's normalised filter footprint.
- Ruling 3: D12 cites 0003's sampler instead of restating its key.
- Ruling 6: 0002 names kept for kernels and `ShadeKind`; 0005's `SurfaceKind::Ext` and `photon_*` names mapped to `shade_surface_ext`, `shade_light_path(_ext)`, `generate_light`.
- Ruling 7: `generate_light` recomputes light-path wavelengths from the seed.
- Ruling 9: D13 defers field lists and the 256 B / 128 B budgets to 0003.
- Ruling 10: D15 reports through `ar_status` and 0001's per-render report.
- Ruling 13: release modules embedded in the DLL; loose modules only via `ASTRORAY_KERNEL_DIR` in development.
- Ruling 14: D7 adds launch-duration sizing with per-item caps and re-queue; the number lives in 0009.
- Ruling 15: D12 states AVX2 as required.
- Ruling 16: NVRTC removed from D1, D10, D14, D15, Verification, Risks and Alternatives.
- Ruling 17: C2 recorded as accepted and the spine conflict (old Risk 11) removed.
- Ruling 18: `advance_geodesic` is a CUDA kernel emitting segments for `intersect_closest`; D11 bans material, medium and VM code in OptiX programs.
- Ruling 19: VRAM accounting goes through 0007's ledger, reserve rule and budget table.
- Ruling 21: first-frame gate cited as 0007's.
- Ruling 22: texture filtering with mipmaps cited in D13 as part of the design.
- Ruling 23: `shade_volume` carries the emission-absorption marcher; the register gate amendment is C2.
- Ownership (0002): D8 now lists every kernel other records need (`shade_light`, `shade_surface_ext`, `shade_light_path_ext`, `intersect_subsurface`, film and environment kernels); D11 gives the complete OptiX program list (lamp intersection, curves, spheres, motion variant) and the 8-bit instance mask allocation.
- Ruling 27: `shade_subsurface` and `intersect_subsurface` cite 0005 item 4a for the walk's model (D8).
- Ruling 29: time budgets cited by id (D7 B-17/B-19, D10 B-01/B-02/B-04/B-05/B-09/B-17..B-20, Verification, must-exist).
- Ruling 30: Context states the 30/s target, 24/s floor and 60/s later goal.
- Ruling 41: the f64 CPU build is an unshipped test reference; CPU geodesic integration is double in every build (D3).
- Ruling 46: the `advance_geodesic` row steps in-kernel and emits a segment only when a chord overlaps the scene bounds (D8).
- Ruling 47: D8 enumerates the `Stage` enum and adds `film_prepare_denoise` and the HAIR split pair `shade_hair` / `shade_light_path_hair` (`ShadeKind::Hair`, `LightHair`).
- Ruling 49: Review section headed as history; Risk 12 records owner decision 29 (untested architectures built and labelled).
- Ruling 6 (second pass): D1 cites 0003's `kLanes`; D7's cancellation check reads the pending generation, not an epoch (ruling 37).
- Ruling 29 (second pass): D3, D7, D9, D11, D14, Risk 2 and Consequences carry no time numbers; the D7 overhead derivation moved to 0009 B-19's basis.
- Ruling 41 (second pass): `advance_geodesic_f64` added (D8) for 0006 D37/D40's double and mixed GPU geodesics, the one kernel exempt from the FP64 check (D1, Verification).
- Ruling 43 (second pass): bit 6 is set on every medium-bearing mesh whatever its ray visibility, a bit-6-only hit being a false interface (D11).
- Ruling 47 (second pass): D10 defines the VM stack class by kernel (0005 item 32's list, `bake_environment`, `advance_geodesic`); D2 adds `atomic_max`, `atomic_cas` and `clock` for 0008; D15 splits synchronous driver checks from asynchronous module failures (0001 D2.8); Risk 13 records the unspecified self-intersection rule.
