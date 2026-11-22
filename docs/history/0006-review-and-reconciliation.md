# 0006: review and reconciliation

Moved unchanged from the end of `docs/decisions/0006-volumes-and-astrophysics.md` (ruling 86). History only: nothing here is binding (ruling 50), and the record's Decision section is current.

## Review and resolution

History of the independent review; the Decision section and the Reconciliation section below supersede it where they differ (for example item 1's OptiX raygen, replaced under ruling 18).

1. Geodesic stage vs 0002/0003/0005 (BLOCKER): ACCEPTED. `advance_geodesic` is an OptiX raygen with in-kernel chord traces in a second module (D22). 0003 D28's names are adopted, plus `static_observer` (D23). Ordinary stages have no policy code. Launch is normalised to unit static-observer frequency. The changes to 0002 D8/D10/D11 are listed in Risk 11.
2. Camera inside R_c (BLOCKER): ACCEPTED. Tetrad launch at the camera (D24); θ_tol from the full output resolution and fixed per epoch; frames defined (D25); NEE gated on segment deflection, not on the sphere (D28).
3. Omitted scope: ACCEPTED. Volumetric fluorescence (D15), photon mode (D11), shadow rays carrying and updating the stack (D19).
4. Grid transport loop: ACCEPTED. Sparse 8³-leaf ABI through Blender's bundled `openvdb` module (D5), gating spike G (Risk 5); 0007 change listed.
5. Two mapping paths, size, Mix/Add: ACCEPTED. One VM lowering to coefficients (D1-D3), `MediumDesc` 48 B with no material parameters, Mix/Add declared as challenge C3; 0005 item 39 change listed.
6. Marcher cost, mixed overlap: ACCEPTED. DDA walk with empty-cell skipping, per-segment estimator rule (any scatterer means delta-track all) (D9), cost measured in V20.
7. Nested dielectrics in the fused kernel: ACCEPTED. Hidden bit dropped (derived), false interfaces exit before the VM, the other side's IOR is a constant spectral source with a declared register delta and a fallback, and the stack is allocated for ≥ 2 transmissive materials (D16-D17).
8. Fake emissivities, log(0), domain rule: ACCEPTED. A generated, committed line table from published fits, flagged unverified (D34); emissivity linear in n_e·n_ion with a clamped lookup coordinate (D32); 0003 D9 clamp-and-report adopted (D4).
9. Precision criteria: PARTLY ACCEPTED. The derivation was wrong and is now based on the narrowest viewport band; the mixed-precision S1 variant is added. g ≤ 1e-5 is REJECTED for the viewport, because thermal-width outputs are science outputs, always double (D37, D40); the remaining tradeoff is an owner question.
10. CCM89 range: ACCEPTED (0.1-3.33 µm, D32).
11. Air vs vacuum: ACCEPTED. The engine's λ is vacuum; line-sensitive inputs are converted at load; the CIE offset is documented (D33).
12. Fournier-Forand missing: ACCEPTED (D12).
13. Null-collision launch cap: ACCEPTED. 4,096 per launch with memoryless requeue and a persisted RNG counter, bit-identical (D8, V6).
14. World volume to the bounding sphere: ACCEPTED. Infinite homogeneous medium, as Blender documents (D21, V3).
15. Covariant entry momentum: ACCEPTED. Contravariant momentum from the static tetrad (D24).
16. Tiers for V11/V12: ACCEPTED. Tier line in Verification.

## Reconciliation (rulings applied)

- G1: "What lands when" replaced by the must-exist and can-wait lists; milestone assignments removed from Context, D5, D9, D10, D15, D31, D33, D35, D38, Risks 1, 3, 5.
- G2 / Ownership: kernel names, queues and gates cited to 0002; path-state and shadow-item sizes to 0003; report schema and codes to 0001 (D4 lists the entries raised here); VRAM to 0007; film buffers and science-output limits to 0008; time and compile budgets to 0009; the grid format (D5) and medium interface stay here as owner.
- G3: no scheduled job in this record; `full`-tier checks are `check full`, run on demand.
- G4: `PhaseKind::TABLE`, `MediumDesc.phase_table`, the 4 reserved bytes (MediumDesc now 40 B) and the reserved `Stage::ADVANCE_GEODESIC` hook deleted.
- Ruling 1: emission from both estimators and the world medium goes to 0008's per-sample buffer; the path state carries no radiance (D9, D19, D21).
- Ruling 2: medium camera connections splat with 0008's normalised filter footprint (D11).
- Ruling 3: `medium_rng` continues 0003's sampler stream; pcg4d reference removed (D8).
- Ruling 4: hero lane replaces lane 0 in tracking, phase sampling, fluorescence and overlap selection (D8, D12, D15, D18, D31).
- Ruling 5: volume NEE and equiangular/distance MIS use the balance heuristic in r_u/r_l form (D10).
- Ruling 6: `apply_frequency_shift`, `rewrite_wavelength`, `GEODESIC_NUMERICAL`, `LIGHT_PATH_IN_REGION`, `shade_volume` and light-path terminology adopted; 0003 cross-references updated to its current ids (D8, D9, D20-D22, D28).
- Ruling 7: line-aware sampling stays a component of 0003's sensor table shared by light paths; no emitter-spectrum sampling (D36).
- Ruling 8: the 20 B medium state is defined here and carried by light-path items (D11, D16).
- Ruling 9: byte arithmetic for the path state and shadow item removed; fields returned to 0003 (D19).
- Ruling 11: the sparse 8³ leaf grid format specified completely, including voxel order and channel interleave (D5).
- Ruling 14: per-launch caps tied to 0009's launch-duration budget; one geodesic step per launch (D8, D22).
- Ruling 17: register-delta and 168-register limits replaced by spill/stack allowances in the capped build, uncapped count reported (D17, V18, Risk 7).
- Ruling 18: `advance_geodesic` is a CUDA kernel emitting chords to `intersect_closest`; no second OptiX module; record holds both chord ends and the chord length (D22, D27, D39, D41, Alternatives, Consequences, Risks 7 and 11).
- Ruling 19: grid and majorant memory counted in 0007's VRAM budget table (D6).
- Ruling 21: spike G passes against 0007's first-frame gate instead of 2x (Risk 5).
- Ruling 23: C1 and C3 recorded as accepted spine amendments.
- Ruling 24: volumetric fluorescence draws λi for lane 0, the lane `terminate_secondaries` keeps (D15).
- Ruling 25: the chord length lives only in `ray_tmax`; removed from the geodesic record, now 164 B / 92 B (D22, D41).
- Ruling 28: `interior_ior` and `medium_priority` cited to 0005 item 2a; the translucent interior medium is 0005 item 4's lowering to `MediumSample` (D17).
- Ruling 29: launch caps and compile time cite 0009 B-17 and B-05 (D8, D22, V18, Consequences).
- Ruling 41: D39 states double geodesic integration on the CPU in every build; the full double build is an unshipped test reference.
- Ruling 43: medium-boundary hulls use mask bit 6, bit 7 spare (D20); the grid descriptor's object-to-world matrix stays in D5.
- Ruling 46: D22 steps in-kernel while chords cannot touch scene geometry, marching media, and emits a segment only when a chord overlaps the scene bounds; Risk 7 and Consequences follow.
- Ruling 49: Review section headed as history; Risk 11 reduced to the two requirements not yet applied; D15's open-item text corrected to 0003 D7.
- Ruling 6 (second pass): D2 uses 0003's `Spec` and `Lambda`.
- Ruling 29 (second pass): D40 and Alternatives state the double-precision estimate against 0009 B-14 instead of seconds per frame.
- Ruling 41 (second pass): D37 and D40 name 0002 D8's `advance_geodesic_f64` for double and mixed GPU geodesics.
- Ruling 43 (second pass): D20 puts bit 6 on every medium-bearing mesh whatever its ray visibility, bit-6-only hits being false interfaces.
- Ruling 44 (second pass): a light path ends without connecting at an inelastic medium event (D11, D15).
- Ruling 49 (second pass): D24's static camera frame cites 0007 decision 41; Risk 7 compares with 0002 D7's about 26 steps; Risk 11 closed (0004 D23 states the connection gate).
