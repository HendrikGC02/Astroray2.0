# 0005: review and reconciliation

Moved unchanged from the end of `docs/decisions/0005-material-model.md` (ruling 86). History only: nothing here is binding (ruling 50), and the record's Decision section is current.

## Review and resolution

History of the independent review; the Decision section and the Reconciliation section below supersede it where they differ.

1. Stack budget versus 0002 (BLOCKER): ACCEPTED. 512 B adopted and split (record ≤ 304 B, 32-entry register file 128 B as spike A measured, ≥ 80 B other); `ext_dyn`, `metal_n`/`metal_k`, emission and holdout left the record; layout ratified only after M-1; the 1 KiB alert replaced by the 0002 gate.
2. VM call sites and owed interfaces: ACCEPTED, except the second register file: entry table (item 32) and compiler outputs (`MAY_BE_DELTA`, `NON_OPAQUE`, emission estimate, Filter Glossy; item 43) added. REJECTED that NEE needs a second register file: programs run sequentially and share one kernel-owned file, as Cycles does (item 3).
3. Thin film and furnace expectations: ACCEPTED. Closed-form Airy per wavelength; independent characteristic-matrix oracle; per-configuration furnace table with E ≤ 1.002 for bump and rough film; film-aware base attenuation added because film-free layering can exceed 1.
4. Features cut from the first release: ACCEPTED. The 0001 assumption was false (0002 and 0007 land curves before release); HAIR and FLUORESCENT moved into the first release; AO, Bevel and Raycast become reported approximations; the "not callable" rationale replaced by the actual cause, 0002 D11, raised as an owner question.
5. Python translation cost: ACCEPTED. M-8 with a budget tied to 0007's TTFF row, and a translation cache keyed on 0007's update classification (item 29).
6. VM control flow: ACCEPTED. Forward-only branch opcodes, the three-segment layout, per-entry dead-code elimination, holdout as a summed weight, emission attenuation and opacity rules specified (items 31, 38).
7. Extension stage count and names: ACCEPTED. 0002's kernel names adopted; extension code in exactly `shade_surface_ext` and 0004's `photon_scatter`, each with one named split.
8. False or ambiguous statements: ACCEPTED. Constant materials still run slot resolution; the trilinear table lookup versus per-texel coefficients clarified; fuzz LTC stated as imported and checked; E tables indexed by (αx αy)^(1/4).
9. Hooks without a first-release user: ACCEPTED. `ext_dyn` replaced by a zero-cost field map; `STOCHASTIC_PDF` removed; `REWRITES_WL` and NEE in `shade_surface_ext` stay because FLUORESCENT and HAIR are now first-release users.
10. Grating order 0 and generator parameters: ACCEPTED. Order chosen by lane-mean efficiency; order 0 keeps secondaries when its direction is wavelength-independent; groove depth, blaze angle and preset values specified.
11. Linear-space filtering versus Blender: ACCEPTED in a different form. Rather than excluding Linear and Cubic from the check, filtering now follows Blender's encoded-space order (owner decision 14); the check runs at LOD 0 because Cycles has no mips.
12. Float texture into Principled IOR: ACCEPTED. A float link into an IOR slot is a per-point flat spectrum, or per-point n_d with dispersion (item 10).

## Reconciliation (rulings applied)

- G1: "What lands when" replaced by the two lists; milestone wording removed from items 14, 19, 25, 27, 28, 37, 41, sections E and G, Consequences, Verification 12 and Risks.
- G2: restated numbers replaced by citations: 0002 gate limits (Context, items 3, Verification 7, M-1, M-4, Alternatives), 0003 upsampling table size and range rules (items 10-11), 0004 sky model (item 36), 0006 medium-stage behaviour (item 39), 0001 report schema and codes (items 29, 40), 0009 time budgets (item 29, Verification 7, M-3, M-8).
- G3: E-table regeneration runs in `check full` on demand, not a slow scheduled tier (item 6).
- G4: removed the reserved HAIR mode flag, reserved `ext_kind` values 4-5 (now taken by amendment), the "can call it later" reservation of the reradiation file (now a current consumer, 0006 D15) and the masked-extension forwarding plan from the Decision section.
- Ruling 1: holdout, AOV and samples reach the film through 0008's per-sample buffer (Assumptions); the record holds no accumulated radiance.
- Ruling 3: selection and extension dimensions cite 0003's sampler and dimension map (items 26, 38).
- Ruling 4: grating and fluorescence follow the path's random hero lane (items 22, 26).
- Ruling 5: fluorescence after a rewrite uses 0004's MIS (item 26).
- Ruling 6: `photon_generate`/`photon_scatter`/`photon_scatter_ext`/`SurfaceKind`/`rewrite_lane0` renamed to `generate_light`, `shade_light_path`, `shade_light_path_ext`, `ShadeKind`, `rewrite_wavelength` (items 17-18, 26, 32, Assumptions); stale naming conflict removed from Risks.
- Ruling 7: light paths use the sensor wavelength table recomputed from their key (item 25); line sampling is the table's `lines` component (item 36, section G); the fluorescent light-pass connection states the field it needs (item 26).
- Ruling 9: per-path and shadow-item field needs stated (items 26, 33, 43) and returned to 0003; Risk added.
- Ruling 16: NVRTC removed from Assumptions and Alternatives.
- Ruling 17: gate described as spills and stack in the shipping capped build, uncapped registers a `check full` warning.
- Ruling 18: no VM in OptiX programs cited for the entry kernels and AO/Bevel/Raycast (items 32, 41).
- Ruling 19: upsampling-table and mip memory belong to 0007's budget table (Consequences, Assumptions).
- Ruling 20: image decoding attributed to 0007 with libraries on 0001's dependency list (item 11).
- Ruling 21: M-8 cites 0007's first-frame gate instead of restating it.
- Ruling 22: mipmaps stated as part of the design (item 34).
- Ruling 23: per-slot upsampling convention and Add Shader policy recorded as accepted spine amendments (items 10, 38, section H).
- Ruling 24: lane 0 continues after a non-zero grating order and draws the fluorescent excitation (items 22, 25, 26; Verification 8); supersedes the ruling 4 line above for those items.
- Ruling 27: item 4a states the random-walk model and the record's fields; 0003 allocates the buffer, 0002 lists the kernels; items 20, section G, Assumptions updated; exit and step limit raised in Risks.
- Ruling 28: item 2a adds `interior_ior` and `medium_priority`; item 4 lowers `transmission_depth`/`transmission_scatter` to 0006's coefficient-level medium.
- Ruling 29: time budgets cited as 0009 B-24 and B-30 (item 29, Verification 7, M-3, M-8, Assumptions).
- Ruling 34: MikkTSpace cited as on 0001's dependency list (Assumptions).
- Ruling 36: MULTILAYER and MEASURED keep enum values 4 and 5 and one line (items 14, 28); MEASURED design and test removed.
- Ruling 42: front face cited as 0003's flag (item 8).
- Ruling 44: no light-pass connection at FLUORESCENT surfaces; the prefolded-weight requirement on 0003 and its risk removed (items 25-26, Risks).
- Ruling 47: HAIR's split kernels named as in 0002 D8 (item 18).
- Ruling 48: AO and Bevel are reported no-ops (item 41, mapping table, Alternatives, Risks); thin film confirmed physically computed (section G).
- Ruling 49: Review section headed as history; the stale owner question on ray queries and the 0003 check reference in Verification 12 corrected.
- Ruling 27 (second pass): item 4a specifies the walk's entry, exit and step limit from Cycles' random walk; the open-item risk replaced.
- Ruling 29 (second pass): the runtime-compilation alternative cites 0009 B-04 and B-14 instead of times.
- Ruling 47 (second pass): items 3, 32 and Assumptions use 0002 D10's VM stack class; item 32's `volume` row names `advance_geodesic`'s marcher.
- Ruling 48 (second pass): Raycast renders magenta (owner decision 26 covers only AO and Bevel): items 41-42, Consequences, can-wait, Risks.
- Ruling 49 (second pass): item 43 adds `READS_RAY_TYPE` and the sky-split output 0004 D12/D19 read; item 27 no longer claims a curve self-skip 0002 lacks; item 41 states the IES profile's route and parser.
