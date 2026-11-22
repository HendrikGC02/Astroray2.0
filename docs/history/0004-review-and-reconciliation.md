# 0004: review and reconciliation

Moved unchanged from the end of `docs/decisions/0004-integrator.md` (ruling 86). History only: nothing here is binding (ruling 50), and the record's Decision section is current.

## Review and resolution

History of the independent review; the Decision section and the Reconciliation section below supersede it where they differ.

1. BLOCKER, per-path accumulator vs 0002/0008: ACCEPTED. Shadow items are self-contained; contributions go to a per-sample buffer resolved at pass end (D4, D5), `L` leaves the path state, the "no atomics" and bit-reproducibility claims are corrected; the 0003/0008 conflict is listed. The `shadow_pending` alternative is rejected in Alternatives.
2. BLOCKER, sky owned by no record: ACCEPTED. D12 owns it: Single and Multiple Scattering from the Apache-2.0 and MIT files, spectral table, disc split only for a proven linear chain, otherwise a warning and table sampling.
3. BLOCKER, dimension map vs 0003 #24 and alpha-card reuse: ACCEPTED. D14/D15 now consume 0003's map and pcg4d; the block index is an event index that opacity pass-throughs advance; a stacked-alpha test is added.
4. MAJOR, film-response importance starves UV lights: ACCEPTED. Excitation spectra join the weighting, a radiometric ε floor guarantees pmf > 0 (D10, D20), light-path wavelengths follow 0003 #21, and a UV-on-brightener test is added.
5. MAJOR, D19 underspecified: ACCEPTED. One delta predicate, reconnection after mixed vertices, ray-type-dependent materials excluded from casters, segment-symmetry rules for visibility, holdout, shadow catchers and linking, and the requested test scenes plus a prism hue gate against an independent oracle.
6. MAJOR, incomplete coverage list: ACCEPTED. L δ+ D D+ E and the window-glass dependence are reported (D24), with a window benchmark that sets the light-pass share.
7. MAJOR, neighbour conflicts: ACCEPTED, item by item. Splats now hit one pixel chosen by filter weight, although the current 0008 #4 already adopts footprint splatting, so the fix is kept for pixel independence and fewer atomics. The 0006 power heuristic is listed as a conflict. The built light pass moves to "before first release" with its interfaces in M1. Lamp crossings are one per step, with no cap, and costed (D8). Light paths use 0002's kernels and light extension variants (D21). The 0008 inputs are now provided (D27).
8. MINOR, silent caps: ACCEPTED. The lamp cap and its SOURCE label are gone (D8); statistics use 0008's u32 counters and nothing saturates (D28).
9. MINOR, exposure and |A−B|: ACCEPTED. The metric uses pre-exposure values, so display edits never stale it, and the Cycles form |I − A| with A = 2·S1A/n (D16), with a test against Cycles' formula.
10. MINOR, adaptive tail starvation: ACCEPTED. The tail rule gives k samples per active pixel per pass within the buffer size, and a measurement is added (D16).
11. MINOR, numbers with no source: ACCEPTED. Each number now has a derivation (P floor, reset, light-build budget), a source (1024 map) or a named measurement (25% share). The 24 B figure is removed because 0008 owns storage, and statistics defaults follow 0003/0008.
12. MINOR, hooks with no user: ACCEPTED. The `sample_direct` flag, reserved dimension and vertex record are removed (D26).
13. MINOR, mis-citations: ACCEPTED. Blue noise is now cited as Ahmed and Wonka 2020 / Cycles `pattern.h` and dropped from this record. NEE plus BSDF is described as multi-sample MIS with one sample per strategy (D1).

## Reconciliation (rulings applied)

- G1: "What lands when" replaced by the must-exist-first and can-wait lists; milestone wording removed from D10, D12, D26, Alternatives, Risks and Assumptions.
- G2: restated content removed and cited: per-sample buffer and sizes (0008, 0007), report schema (0001), mask bits and kernel list (0002), dimension slot numbers and enums (0003), edit classes (0007), time budgets (0009).
- G3: no scheduled checks in this record; Verification states that all checks run on demand.
- G4: no reserved hooks remain; every must-exist item names the layout or signature it protects.
- Ruling 1: D5 reduced to the accumulation contract on 0008's per-sample buffer; `L` stays out of the path state.
- Ruling 2: D23 one-pixel stochastic splat replaced by 0008's footprint splatting; the splat-pixel uniform removed from D15.
- Ruling 3: D13-D15 cite 0003 as sampler owner; DLSS mode uses 0003's independent mode.
- Ruling 4: random hero lane per path applied to light paths (D22) and volume distance MIS (D1).
- Ruling 5: D1 states the balance r_u/r_l form for media, including 0006's distance-strategy combination; the 0006 conflict is removed.
- Ruling 6: `MAX_BOUNCE`, `terminate_secondaries`, `generate_light`, `shade_light_path`, `ShadeKind` used.
- Ruling 7: light-path wavelengths from the sensor table, recomputed from the key (D20-D22); the fluorescent-receiver consequence is listed in Risks.
- Ruling 8: light paths carry the 20 B medium stack (D21).
- Ruling 9: path-state byte accounting removed; field needs listed for 0003.
- Ruling 14: light-pass launches stay inside 0009's launch-duration budget (D20).
- Ruling 18: curved segments disarm the camera-side partition and light paths entering the region terminate (D19), consistent with geodesic segments through the ordinary intersect kernel.
- Ruling 19: per-sample buffer memory cited to 0007's budget table.
- Ruling 21: light-structure build counted toward 0007's first-frame gate (D25).
- Ruling 24: D22 says lane 0 continues after `terminate_secondaries`; no record text says the hero lane continues.
- Ruling 26: D14 cites 0003's stored `event_index`.
- Ruling 29: time budgets cited by id (D20 B-17, D25 B-22/B-23, Verification B-22/B-29); the requirement for 0009 rows removed from Risks.
- Ruling 35: the Blender-watt, sky and IES unit scale cited to 0003 decision 11 (D6, D12, Assumptions).
- Ruling 37: `sample_id` names the sample slot (D4, D5); D25 uses 0007's generation and epoch.
- Ruling 44: D19 excludes wavelength-rewriting receivers from the light pass (ownership, light-path side, camera arming); the fluorescent-receiver risk removed; Verification D19 tests it.
- Ruling 45: light and shadow linking removed (D7 `light_set`, D10 per-set trees, D19 linking symmetry, D29 reason, must-exist, Consequences, Verification, Assumptions).
- Ruling 48: point lamps and spotlights cast caustics through the light pass; the D24 gaps are reported as a warning (owner decision 25); the owner question removed.
- Ruling 49: Review section headed as history; D1's curved-segment NEE text now cites 0006 D28; settled requirements and the spectral-MIS risk corrected.
- Ruling 29 (second pass): the light-pass share is 0009 B-36 (D20, Verification D24 window); the floor clamp is the derived full wave, not 2^15.
- Ruling 44 (second pass): inelastic medium events join wavelength-rewriting surfaces in D19 (ownership, light-path end, camera arming) and D21; D23 states that connections pass 0006 D28's gate.
- Ruling 48 (second pass): every camera type implements `We`/`PDF_We`/`sample_wi` (D23), so D24 no longer drops orthographic and panoramic cameras (owner decision 25); Risk and Verification D23 follow.
- Ruling 49 (second pass): the first light-path vertex is x₁ (v1 means the old project); D4's pass category is read from 0003's side buffer; D5's slot flags match 0008 decision 3; D12 and D19 cite 0005 item 43's sky-split output and `READS_RAY_TYPE`; the requirement removed from Risks.
