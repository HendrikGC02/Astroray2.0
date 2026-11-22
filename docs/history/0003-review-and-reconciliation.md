# 0003: review and reconciliation

Moved unchanged from the end of `docs/decisions/0003-spectral-model-and-path-state.md` (ruling 86). History only: nothing here is binding (ruling 50), and the record's Decision section is current.

## Review and resolution

Historical record of the independent review; where it conflicts with the Decision above, the Reconciliation section and the Decision win.

1. Sampler conflicts with 0004 — ACCEPTED at the time (0004 owned the sampler); superseded by ruling 3, which returns the sampler, its dimension map and the independent mode to this record (decision 23).
2. Unit anchor unowned and ambiguous — ACCEPTED: Y(s) defined; Y_w = Y_n stated for 0008; K = 683·Y_n = 72,983 cd/m² for 0005; Blender-light factor fixed at 1; science channels from Blender-unit emitters flagged (`PHYSUNIT = F`). Refusing instead is owner question 1.
3. MIS after termination or rewrite undefined — ACCEPTED in substance, fix REJECTED as proposed: copying pbrt-v4 literally is biased when chromatic hero sampling precedes termination (1.8 vs 1). Decision 6 draws a per-path random hero; decision 7 fixes the rules after termination and rewrites; check 9 adds the discriminating lane-0 variant. The max_λ point is rejected: max is permutation-invariant.
4. No redshift for hits inside the region — ACCEPTED: `apply_frequency_shift` at every hit and exit, with its unit test (check 18).
5. Duplicated formulas — ACCEPTED: photon and band formulas deferred to 0008; line widths from 0006; one termination enum here.
6. Astrophysical scale in float32 — ACCEPTED: origin at the region centre; shutter-relative ray time; radiance scale s = 2^k; channel range matches the Planck test; large-world warning.
7. Silent energy loss across the region — ACCEPTED then; the sphere rule and delta-light refusal are now replaced by 0006 D28's gate (decision 30).
8. Incomplete state — ACCEPTED; the full table is now decision 19 (155 B unconditional, 225 B maximum).
9. Film conversion for light paths — ACCEPTED: light paths use the shared table and `film_weights` at their own λf; no per-light SPD state.
10. Thresholds — ACCEPTED: Šidák gate; deterministic register, instruction and Minkowski checks; Planck tolerance (2x + 8)·2⁻²³.
11. Hooks switched off, builder selection — ACCEPTED: components selected automatically; support always contains the visible band; widening over 4096 bins.

## Reconciliation (rulings applied)

- **G1.** "What lands when" replaced by the must-exist and can-wait lists; milestone tags removed from Context, Verification (checks 16, 18, 19) and Risks; owner question 2 (fluorescence timing) dropped as the roadmap's business.
- **G2.** MIS form cited to 0004 D1 (decision 7 keeps only lane rules); geodesic loop and region gate cited to 0006 D22, D27, D28 (decisions 29-30 keep only what touches the path state); film formulas, buffer and splats to 0008; pool capacity and field storage to 0002; the band mixture weight and the Blender-watt factor stated once here.
- **G4.** Polarisation reserves nothing; `ANY_NON_SPECULAR` deleted (no consumer); every flag bit and field names its consumer; free flag bits are unallocated, not reserved.
- **Ruling 1.** `L` removed from the path state; `film_weights` applies at resolve and at splat delivery; the per-path accumulator moved to Alternatives as rejected.
- **Ruling 2.** Connections carry `film_pos`; the splat footprint is 0008's, so the map has no splat-pixel dimension.
- **Ruling 3.** Decision 23 owns the sampler: Burley 2020 definition, keys, full dimension map for every consumer named by 0004, 0005, 0006, 0007 and 0008, variable streams and the independent mode; no special seed.
- **Ruling 4.** Random hero lane per path everywhere, light paths included; `terminate_secondaries` keeps lane 0, because keeping h is the biased case of decision 6's counterexample (risk 12 lists the records that say otherwise).
- **Ruling 5.** Decision 7 defers the MIS form to 0004 D1 for surfaces and media.
- **Ruling 6.** Names: `terminate_secondaries`, `rewrite_wavelength`, `apply_frequency_shift`, `MAX_BOUNCE`, `GEODESIC_NUMERICAL`, `advance_geodesic`, `route_hit`; cross-references updated to the current ids of 0001, 0002, 0004-0009 (e.g. 0006 D23-D28, D36, D40-D41; 0008 decisions 3, 7-10, 12, 15, 23).
- **Ruling 7.** Light-path film wavelengths recomputed from their key (0 B); no emitter-spectrum sampling.
- **Ruling 8.** Medium state 20 B (stack + `medium_rng`) in the path state and the shadow item; the photon/light-path slot is the same field.
- **Ruling 9.** Decisions 19-20 hold every requested field: path state 155 B unconditional, 225 B maximum (≤ 256); shadow item 126 B maximum (≤ 128) after moving 0008's light-pass split to a side buffer and sharing `r_l`/`film_pos`; derived fields (pixel, sample, event index) and conditional allocations listed; side buffers named with their owners.
- **Ruling 14.** Variable-count streams make capped, re-queued launches bit-identical.
- **Ruling 17.** Check 16 gates spills and stack in the capped build.
- **Ruling 18.** The policy lives only in `advance_geodesic`; segments go through `intersect_closest` with `ray_tmax`; the region branch lives in `route_hit`; `static_observer` added (0006 D23).
- **Ruling 24.** Lane 0 survives termination and rewrites (decision 5); the risk listing the other records' wording removed, since 0004 D22 and 0005 items 22, 25, 26 now say so.
- **Ruling 25.** `ray_tmax` is the only chord length (decision 19 row 24, decision 29); the geodesic record shrinks to 164 B / 92 B (decision 20).
- **Ruling 26.** `event_index` (u16, 2 B) stored in the path state (decision 19 row 6a, decision 23); unconditional 157 B, maximum 225 B; the derivation, its alternative and assumption text corrected.
- **Ruling 27.** The random-walk side buffer is allocated here with fields and size from 0005 item 4a (decision 20, stream 3).
- **Ruling 29.** Check 16's runtime ratio moved to 0009 B-35.
- **Ruling 35.** Unit anchor stays here (decision 11); the risk about 0004's wording removed.
- **Ruling 37.** `sample_id` is the per-sample key everywhere (decision 19 row 1).
- **Ruling 41.** CPU geodesic integration is double in every build (decision 24).
- **Ruling 42.** `FRONT_FACE` (bit 20) and `FIRST_BOUNCE_SPECULAR` (bit 21) added; shadow-catcher bits name their pass; side buffers counted in 0007's budget table (decisions 20-21).
- **Ruling 44.** `FILM_WEIGHT_PREFOLDED`, its `item_flags` bit and the FLUORESCENT `lambda_t` condition removed (decisions 5, 13, 20); shadow item 124 B.
- **Ruling 45.** `prev_light_set` and `shadow_set` removed (decisions 19-20); no linking state.
- **Ruling 46.** Decision 29 writes only the chords `advance_geodesic` emits for intersection.
- **Ruling 49.** Risk 12 (edits owed by other records) removed: every item is applied there or settled by rulings 24-27, 35, 44.
- **Ruling 27 (second pass).** Decision 22 names 0005 item 4a as a consumer of the termination enum (its walk limit ends with `VOLUME_LIMIT`).
- **Ruling 29 (second pass).** Risk 9's `specgen` threshold cites 0009 B-37.
- **Ruling 44 (second pass).** Decision 5's note covers inelastic medium events as well as surfaces (0004 D19, 0006 D11).
- **Ruling 49 (second pass).** Decision 22 calls the light path's first vertex x₁ (v1 means the old project); the must-exist item no longer ties `film_weights` to the hero lane (0008 decision 3's slot holds no hero).
