# 0008: review and reconciliation

Moved unchanged from the end of `docs/decisions/0008-film-outputs-denoise.md` (ruling 86). History only: nothing here is binding (ruling 50), and the record's Decision section is current.

## Review and resolution

History of the independent review; the Decision section and the Reconciliation section below supersede it where they differ (for example "end of M1", now the roadmap's).

Independent review, 2026-11-14 (17 objections, no blockers).

1. Frame budget misread spike B: ACCEPTED. Context now uses B(rate) = 1/rate − t_present (the ~6 ms is Blender's CPU loop, decoupled from the producer); V12 part A(c) is re-derived as B(rate)/2 instead of fixed 4 ms / 10 ms; the OIDN rule uses the same share.
2. Spectral demodulation identity: ACCEPTED. Col is the exact RGB projection (decision-15 rule); Dir = mean RGB(L_cat) / mean Col at resolve; Col = 0 gives 0 and a report count; V6 tests the exact raw identity.
3. Spike F moved to end of M1: ACCEPTED. Part A runs now on synthetic inputs, as CHECKPOINT-1 requires; part B stays at the end of M1; DLSS-only M1 items depend on part A.
4. Heat maps under AgX: ACCEPTED. The engine emits display-referred sRGB RGBA8, which 0007 decision 40 draws outside `bind_display_space_shader`; depends on probe P4.
5. Guides not noise-free, V10 unpassable: ACCEPTED. Decision 15 says the guides are free of wavelength noise only and states Mix Shader and glass behaviour; V10 uses a constant-colour plane and reports guide variance elsewhere.
6. Register cost unbudgeted, variant fallback: ACCEPTED. New decision 22 places the work in `shade_surface` (no separate VM pass), gates it with all flags on, and uses 0002's fallback ladder instead of a variant stage.
7. Denoiser init on first-frame path, OIDN failure unhandled: ACCEPTED. Creation is asynchronous with undenoised frames meanwhile; OIDN failure shows the noisy image and an error with no CPU fallback; V11 times cold init.
8. Precision test non-discriminating: ACCEPTED. V2 uses 3e7 samples of 1.0 and 2²⁴ samples of 0.1 (0003's requirement); the 0009 error is listed in Risks 11.
9. Atomics unnecessary, second write path: ACCEPTED. Samples-per-pass contract stated, plain fp64 writes with no atomics (consistent with 0004 D5); the Risk 1 fallback is now decision 4's fold pattern, replacing the direct write, not adding a path.
10. Filters disagree, double counting: ACCEPTED. Splats use the same normalised pixel filter (0004 D23); the D19 partition is stated as 0004's obligation.
11. Photon channels force Box on everything: ACCEPTED. Decisions 5 and 9 say the filter is per render, display included, and the render refuses rather than switching.
12. Statistics undefined at n < 2 and in DLSS mode: ACCEPTED. Rel error is -1 / "no data" for n < 2; statistics reset on epoch change, not on DLSS's per-frame reset; the rel-error view is unavailable in DLSS mode.
13. Cryptomatte slot count: ACCEPTED. `levels` slots stored as levels/2 RGBA layers.
14. FITS writer under-scoped: ACCEPTED. Cubes are uniform-bin only, the writer emits IMAGE HDUs only, the unsourced line count is removed, and CFITSIO is rejected on build cost, not licence.
15. Reserved PSF and noise interfaces: ACCEPTED. Decision 14 is a one-line deferral.
16. R9 override undeclared: ACCEPTED IN PART. The record does not override R9: 0004 D16 keeps the A/B metric. The draft's error was assuming adaptive reads only `MOMENT` and n; `MOMENT` now carries the class-A sum, and Context says R9 is kept.
17. DOF guide and recreate hitch: ACCEPTED. Part B adds a thin-lens depth-of-field scene with a notice rule (decision 19); part A records feature recreate time, which feeds decision 20's hysteresis.

Also corrected while revising: the splat fold no longer divides by the photon count again, since 0004 D20 already puts 1/P in the weight. The draft pointed to an "Owner questions" section that did not exist; it is now Risks 12.

## Reconciliation (rulings applied)

- G1: "What lands when" replaced by the two lists; milestone assignments removed from Context, decisions 3, 10, 14, 22, Verification (V5, V12), Risks 1 and 9, and the assumptions.
- G2: restated content of other owners removed and cited: termination enum (0003), register gate and fallback ladder (0002), report codes (0001), OIDN version (0001), VRAM reserve (0007), controller hysteresis and redraw ceiling (0007), first-frame gate (0007), time budgets (0009).
- G3: no scheduled job remains; V11 timing and V12 part B run in `check full` or on demand.
- G4: the unused third light-pass category bit is dropped (2 bits instead of 3); decision 14 reserves nothing; no other "for later" hook remains.
- Ruling 1: path-state `L_film` removed; decision 3 now owns the per-sample buffer, `sample_add` (formerly 0002's `film_add`), slot assignment and the resolve; V2, Consequences, Risks 1 and Alternatives follow.
- Ruling 2: decision 4 states the footprint splat explicitly, rejecting single-pixel deposits.
- Ruling 3: the independent sampler mode is cited as 0003's (decisions 3, 19).
- Ruling 6: termination reasons cite 0003's enum; kernel names follow 0002.
- Ruling 7: splats convert at film wavelengths recomputed from the light path's key (decision 4).
- Ruling 9: decision 23 lists every path-state and shadow-item field with size and condition; light-pass state given a concrete size.
- Ruling 12: decision 18 uses 0001's renamed OIDN build and pins no version; Risks 3 and V13 updated.
- Ruling 17: decision 22 states the gate as spills and stack in the capped build.
- Ruling 19: decision 21 reports to 0007's ledger and drops this record's 80% reserve rule.
- Ruling 21: V11 cites 0007's first-frame gate instead of a Cycles timing.
- Ruling 29: B(rate) and its shares cited as 0009 B-25 to B-28 (Context, decision 18, V11, V12 part A(c), Risks 1, Assumptions).
- Ruling 30: Context states the 30/s target and progressive batching; V12 part A(c) uses B-27, which applies the 30/s target.
- Ruling 33: decision 13a states what the readback functions of 0007 decision 5 return.
- Ruling 34: tinyexr's writing role and the NGX headers cited as 0001 list entries (decisions 13, 19).
- Ruling 37: `sample_slot` renamed `sample_id` (decisions 3, 23, Consequences); statistics reset on 0007's epoch (decision 12).
- Ruling 39: NGX headers are included only in `src/denoise/dlss/` (decision 19).
- Ruling 42: decision 23's flag bits match 0003 decision 21, adding the first-bounce-specular bit.
- Ruling 47: decision 22 names the film kernels as in 0002 D8, including `film_prepare_denoise`.
- Ruling 49: Review section headed as history; decision 23 moves `pass_cat`/`pass_frac` to 0003's side buffer; decision 3 keeps the active-pixel list readable; decision 8 cites 0003's band weight; Risks 11's settled conflicts removed.
- Ruling 29 (second pass): decision 18's at-rest delay cites 0009 B-37.
- Ruling 33 (second pass): decision 11's ids come from the addon through 0007's appendix (which also carries IndexMA's material pass index).
- Ruling 37 (second pass): decision 3 states what happens to an interrupted pass (cleared by the epoch reset, which alone can interrupt it).
- Ruling 42 (second pass): decision 23 lists the `AOV_WRITTEN` bit 0003 decision 21 allocates.
- Ruling 49 (second pass): decision 15's previous-frame data cites 0007 decision 15; Assumptions name 0002 D2's atomic max, compare-and-swap and cycle counter; the unfed AO pass is Risk 13.
