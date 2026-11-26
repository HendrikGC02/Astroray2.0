# 0003 — Spectral model, units, path state, sampler, precision and spacetime hooks

Status: proposed (2026-11-14); revised after independent review (2026-11-14); reconciled with RULINGS.md (2026-11-14)

## Context

Astroray 2.0 renders spectra, not RGB. A path carries a small set of wavelengths. Materials, media and lights are evaluated at those wavelengths. Only the film turns radiance into display RGB, sensor bands or photon counts. Wavelengths can change partway along a path: dispersion and gratings make direction depend on wavelength, fluorescence moves energy from one wavelength to another, and Doppler and gravitational redshift scale wavelength. The first astrophysics target is a black hole inside an emission nebula (owner decision 8), so curved rays, redshift and line emission must fit into the core without being added on later. Fluorescence must cover all of its use cases (19). Gratings and the light-tracing pass they need are owner decision 17, and the first release has the full feature breadth (7).

Binding constraints:
- **Spine §2.** 4 wavelengths: 8 was measured at 197 registers, against a budget of 168. Runtime flags, not template matrices. Records are passed by reference.
- **Spine §3.** Physical units inside the engine. 4 stored, rewritable transport wavelengths, kept separate from the film wavelengths. The wavelength sampling distribution is data. One observer. SoA path state with a counter-based sampler and diagnostics. A compile-time spacetime policy whose flat case costs nothing.
- **Spine §6.** Output is scene-linear in the file's working space. **Spine §7.** A double-precision CPU reference for GR.
- **RULINGS.** Rulings 1, 3-9 and 18 bind this record directly; G1-G4 bind its form.

**Owned here (RULINGS, Ownership):** the path-state and shadow-item layouts and their byte budgets, the packet operations and their names, the flag and termination enums, the sampler and its dimension map, the wavelength table mechanism, the unit anchor, precision rules and the spacetime policy interface. **Cited, not restated:** transport, MIS form and light tracing (0004); media, line shapes, geodesic integrator, region gate and redshift law (0006); film formulas, per-sample buffer, splats and passes (0008); kernels, pools and the field-storage mechanism (0002); VRAM (0007); time budgets and test tiers (0009).

Evidence used: spike A (registers, CPU/GPU agreement, timing noise); spike B (scene-linear present path under Blender's view transform); research 04, 05 Part A, 06 (R1-R6) and 17; v1 lessons (observer bug, upsampling nonlinearity, hero collapse, band window, line SPDs, 116x anchor error, GR redshift); pbrt-v4 source read 2026-11-14 (`util/spectrum.h` `TerminateSecondary`; `cpu/integrators.cpp` `VolPathIntegrator::Li`).

## Decision

### Wavelengths

1. **Lane count.** `kLanes = 4` is a compile-time constant in `core/spectrum.h`, the only place that defines `Spec` (`Real v[4]`, radiometric values) and `Lambda` (`Real nm[4]`, nm). MEASURED: spike A §2-3; with 8 lanes the fused kernel uses 197 registers and throughput falls 32% uncapped, 41% capped.
2. **Film wavelengths are sampled once and never stored.** One sampler dimension (decision 23) gives `u`. Lane `i` uses `u_i = frac(u + i/4)`, `λf_i = F⁻¹(u_i)`, `pf_i = p(λf_i)`: stratification in CDF space (pbrt-v4 `SampleVisible`, after Wilkie et al. 2014; v1 measured that offsetting wavelengths themselves is biased under a non-uniform pdf). `(λf, pf)` is recomputed from the sample's or light path's sampler key and the table wherever needed: 0 bytes of state, for camera and light paths alike (ruling 7).
3. **The wavelength distribution is a table** (`WavelengthTable`): K+1 bin edges (float, nm, non-uniform) and K+1 CDF values; pdf constant per bin; binary search. Built by the host in double once per render epoch, immutable within it, shared by camera and light paths (ruling 7; 0004 D20). Sampling proportional to an emitter's spectrum is not designed in.
   - **Support** = the union of the supports of the active film channels (0008 decisions 7-8). Display channels are always active, so the support always contains [360, 830] nm. Channel curves are accepted on [0.1 nm, 1e9 nm], the range the Planck test covers (check 10); a curve outside it is refused with an error. There is no user-set render domain.
   - **Components, selected by the host from the film and the scene (no user setting):**
     - `visible`, always: pbrt's sech² pdf on [360, 830], 1024 uniform bins. PROVISIONAL until spike E (v1 found no proposal that wins everywhere).
     - `response`, when band or cube channels are active: p ∝ Σ R_c over those channels, mixed with `visible` at weight 0.5. The weight is owned here (G2) and PROVISIONAL until spike E; 0008 D8 cites it.
     - `lines`, when the scene has line emitters: components `(λ_l, σ_l, weight)` supplied by their owners. Media lines follow 0006 D36 (σ = √(σ_l² + (λ_l·v_max/c)²), weight = scene-integrated line power); lamp line tables supply their own widths and powers. Bins near a line are at most FWHM/8 wide over ±4 FWHM (FWHM = 2.3548σ). Mixture (1−α)·rest + α·lines, α = 0.5 PROVISIONAL (spike E).
   - **Defensive floor.** A 5% uniform component over the support (PROVISIONAL; Hesterberg 1995). The build asserts pdf > 0 wherever any channel responds.
   - **Bin cap.** K ≤ 4096 (PROVISIONAL). If line components need more, the builder merges overlapping line windows, then widens line bins uniformly until K fits. The table is still the exact pdf, so only variance changes; the report says so (0001 `WAVELENGTH_BINS_WIDENED`).
4. **Transport wavelengths.** `λt[4]` is stored (16 B) and set to λf at path start. Values may lie anywhere in (0, ∞).
5. **Packet operations**, in `core/packet.h`, their only owner (ruling 6):
   - `terminate_secondaries(λt, beta, flags)`: copies lane 0 of `λt` and `beta` into lanes 1-3 and sets `SECONDARIES_TERMINATED`; `r_u`, `r_l` are untouched. Afterwards every lane evaluates at λt[0], so every lane reduction equals lane 0 and later per-lane pdf ratios are 1. Used by dispersion, gratings and any event whose direction depends on wavelength. The kept lane is lane 0, never the hero lane: keeping h is the biased case of decision 6's counterexample.
   - `rewrite_wavelength(λt, beta, λi, ratio, flags)`: fluorescence. The caller has terminated secondaries and drawn λi from the excitation distribution for λt[0]; this sets `beta *= ratio` (= M(λi→λt[0])/p(λi)), all lanes of λt to λi, and `WAVELENGTHS_REWRITTEN`. The film still weights by λf[0].
   - `apply_frequency_shift(λt, beta, g, flags)`: for a change of frame with `g = ν_from/ν_to`, `from` on the camera side. Sets `λt_i *= g` on every lane, `beta *= g⁵`, `WAVELENGTHS_REWRITTEN`. It never terminates lanes: geodesics are achromatic. g⁵ is the wavelength form of the invariant I_ν/ν³, namely I_λλ⁵ (Rybicki and Lightman 1979 §1.5). The law that supplies g is 0006 D30's.
   - `film_weights(flags, pf) -> Spec`: `(1/pf_0, 0, 0, 0)` if `SECONDARIES_TERMINATED`, else `w_i = 1/(4pf_i)`. Applied to a sample's whole contribution at resolve (0008 D3) and to a light-path connection at delivery (0008 D4). This is pbrt-v4's arithmetic (`TerminateSecondary` sets pdf[1..3] = 0 and pdf[0] /= 4). No connection carries a prefolded film weight: the light-tracing pass never connects at a surface or medium event that rewrites wavelengths (ruling 44, 0004 D19).
   - `hero(flags)`: the path's hero lane (decision 6).
6. **Every lane's estimator is unbiased on its own**, so dropping or rewriting lanes never biases the result. Binding everywhere, media and geodesics included (ruling 4).
   - **Surface decisions** (Mix select, lobe, direction, light choice, Russian roulette, grating order choice) use permutation-invariant reductions over lanes (mean or max; 0004 D2's max_λ and 0005 item 22's mean qualify), never one lane. Their pdf is the same for every lane, so surfaces need no per-lane pdf bookkeeping.
   - **Hero-sampled decisions** (null-scattering event and distance, wavelength-dependent phase, overlapping-medium choice, the volumetric fluorescence event: 0006 D8, D12, D15, D18) use the path's hero lane `h`, drawn uniformly from {0..3} once per path from its own dimension, independent of the wavelength dimension (decision 23), and stored in 2 flag bits. Light paths draw their own hero the same way (0004 D22). Then `beta = f/p_h` and `r_u,j *= p_j/p_h`, so `beta/avg_j(r_u,j) = f/((1/4)Σ_j p_j)`: the one-sample balance heuristic over 4 techniques, each chosen with probability 1/4 (Veach 1997 §9.2.4). It is unbiased for each lane separately, but lane h's estimate alone, given h, is not; so the lane that survives `terminate_secondaries` must not depend on h (lane 0).
   - **Why not pbrt literally.** pbrt-v4 fixes the hero at lane 0 and keeps lane 0 at termination; its 4-lane average is correct only while all lanes reach the film. Counterexample (DERIVED): 2 lanes, uniform λ pdf, a discrete event x ∈ {1, 2}, p_a = (0.9, 0.1), p_b = (0.1, 0.9), f_a = (1, 0), f_b = (0, 1), target 1. Keeping the hero lane gives an expectation of 1.8, whether the hero is fixed or random; a random hero with lane 0 kept gives 1.
7. **Lane rules for MIS.** The MIS form (balance heuristic, r_u/r_l, lane averages) is 0004 D1's, everywhere (ruling 5). This record fixes what packet operations do to it:
   - **After termination**, averages still run over 4 lanes: the copied lanes keep the pre-termination ratios that earlier hero decisions need; post-termination ratios are 1 because the lanes are identical.
   - **After a rewrite** (fluorescence draw, frequency shift): the event is shared by every strategy that can produce the path, so it scales numerator and denominator alike. `beta` absorbs M/p(λi) or g⁵; `r_u` and `r_l` are unchanged. 0005 item 26 and 0006 D15 rely on this.
   - Hero-wavelength MIS for rough dispersive transmission (Wilkie 2014) is not built; it may be added only with a measured variance reduction of at least 10% at equal time (v1's park criterion).

### Ranges

8. **Outside a source's range: clamp and report.** A spectral source evaluated outside its defined range returns its edge value and is listed once in the per-render report with the requested range (0001 `SPECTRUM_OUT_OF_RANGE`).
   - RGB reflectance and unbounded sources: the sigmoid at clamp(λ, 360, 830). RGB illuminant: 0 outside [360, 830], so fluorescent brighteners need a spectral light that contains UV.
   - Tables: their end values. Cauchy/Abbe: defined on [360, 830], so it clamps. Planck and Gaussian lines: analytic everywhere.
   - When the table support extends past [360, 830], the report lists every RGB-only material and light.

### Units and colour

9. **Internal units.** Spectral radiance L_λ in W·m⁻²·sr⁻¹·nm⁻¹ in the frame of the static observer at the vertex where the current segment starts. Wavelength in nm (vacuum), length in scene metres (0007 applies Blender's unit scale), time in seconds relative to the frame's shutter centre (decision 26). No radiance is RGB anywhere in transport.
10. **Radiance scale.** Per render the host picks `s = 2^k` so that the largest emitter spectral radiance over the table support (from 0004 D9-D10's power estimates) lies in [1, 2). `s` is folded into every emitter's strength on the host (lights, surface emission, world, medium j); the resolve multiplies by 1/s in double (0008 D3). Float then stays in range across [0.1 nm, 1e9 nm] × [3 K, 1e9 K] (research 06 R3). A power of two is exact, so the output does not depend on s.
11. **Display and the unit anchor.**
   - **Definition.** `Y(s) := ∫ s(λ) ȳ(λ) dλ / Y_n` over 360-830 nm, with `Y_n = ∫ ȳ dλ` from the shipped CMF table (106.856895 nm). Display: `XYZ_k = Σ_i w_i L_i k̄(λf_i) / Y_n`, so display Y = Y(L). In 0008 decision 7's notation, `Y_w = Y_n × 1 W·m⁻²·sr⁻¹·nm⁻¹ = 106.856895 W·m⁻²·sr⁻¹`. Working RGB = `M_ws · XYZ`, no chromatic adaptation.
   - **Anchor.** Scene-linear 1.0 ≙ Y(L) = 1 W·m⁻²·sr⁻¹·nm⁻¹ ≙ luminance 683 lm/W × Y_n = **72,983 cd/m²** (K_m = 683 lm/W, CIE 018:2019).
   - **K for 0005.** `emission_luminance = Strength × K` with `K = 683·Y_n cd/m² = 72,983` (generated by `specgen` from the shipped table, never typed); the engine's radiance is `L(λ) = (emission_luminance / K) · S_ill(colour; λ)`.
   - **Blender lights and sky.** One Blender "watt" is one Y-normalised unit: the spectral quantity is 0004 D6's geometric form × P × S_ill(colour; λ), and for the sun E × S_ill. The factor from Blender's numbers is exactly 1, owned here; 0007 passes values through. Consequence (DERIVED): 1 Blender W of equal-energy white radiates 470 W in 360-830 nm, i.e. 72,983 lm.
   - **Blackbody node** → Planck(T)/Y(Planck(T)), so "colour × strength" keeps its Blender meaning.
   - **Physical-unit inputs** (Astroray spectral lights in W or lm, astrophysical tables) enter unscaled.
   - **Science outputs.** When a band, cube or photon channel is requested and any Blender-unit emitter is enabled, the report lists them as a warning and the science file carries `PHYSUNIT = F` (0008). Not refused (owner question 1).
12. **RGB and Blender inputs.** RGB radiances from Blender use the illuminant convention (decision 16), so they render back as the same working-space RGB to within table error.
13. **Bands and photons** are 0008's formulas (decisions 7 and 9). This record requires only that film conversion uses λf (observed), never λt, and that h and c are the exact CODATA 2018 values, folded in double on the host.
14. **One observer.** CIE 1931 2°, 1 nm steps, 360-830 nm (471 samples).
   - **Source.** pbrt-v4 `src/pbrt/util/spectrum.cpp` (Apache-2.0), arrays `CIE_X/Y/Z`, `CIE_Illum_D6500`, `ACES_Illum_D60`, at a pinned commit; provenance and the CIE dataset DOI in `data/MANIFEST.toml` (0001 D8.2). The 1964 10° observer is not in the repository.
   - **Generated together.** `tools/specgen` produces from that one input set: Y_n and K; D65 and ACES-white SPDs with Y = 1; `M_ws` for `lin_rec709_scene` (BT.709-6, D65), `lin_rec2020_scene` (BT.2020-2, D65) and `lin_ap1_scene` (AP1, ACES white x 0.32168, y 0.33767); the upsampling tables.
   - **Consistency.** Every data file carries the same `observer_hash`; the engine refuses a set whose hashes differ.
15. **Working space.** At sync, 0007 reads `bpy.data.colorspace.working_space_interop_id` and selects `(M_ws, illuminant, table)`. Any other value refuses the render with a visible error. All RGB from Blender is linear in that space; images decode by `Image.colorspace_settings.interop_id`, and `data` images skip upsampling. Film output is scene-linear in the same space and Blender applies the view transform (spike B §1.4). A change of working space resets the epoch and resyncs materials.

### RGB-to-spectrum upsampling

16. **Method.** Jakob and Hanika 2019: `s(λ) = ½ + x/(2√(1+x²))`, `x = c0λ² + c1λ + c2`, λ in nm.
   - **Table.** 64³ (PROVISIONAL), pbrt's layout `float[3][64][64][64][3]` = 9.4 MB per space; scale axis `smoothstep(smoothstep(k/63))`. Lookup: bisection on the scale axis (pbrt `FindInterval`), trilinear coefficients. CPU and GPU share the layout; only the active space is uploaded.
   - **Conventions** (pbrt-v4): reflectance clamps RGB to [0,1] (reported); unbounded uses m = max(r,g,b), `s = 2m·S(rgb/2m)`; illuminant = unbounded × I_ws(λ) with Y(I_ws) = 1. Zero RGB gives an exact zero spectrum; negative components clamp to 0 and are reported. Which convention each slot uses is 0005 item 10's.
17. **Where and how often.** Once per shading point, after texture filtering, in material evaluation, at λt; all nonlinear material maths per lane afterwards. Scalar factors above 1 (η², strength) are flat multipliers after upsampling (v1: 2.5% chroma error otherwise). Textures store RGB, never coefficients.
18. **Table generation.** `tools/specgen` is one C++ file adapted from `rgb2spec_opt.cpp` (BSD-3, in `THIRD_PARTY.md`), built on demand, not in the default target. Outputs `data/spectral/{rec709,rec2020,ap1}.jh` (header: res, observer_hash, illuminant id, generator version) plus `MANIFEST.toml` entries, committed (~28 MB) and embedded as a binary resource (0001 D8.4). Regenerated only when inputs change; the merge gate checks hashes and round trips and never regenerates.

### Path state and shadow items (ruling 9)

19. **Main path state** (camera and light paths, one SoA array per field; storage mechanism, alignment and feature tags: 0002 D13; capacity: 0002 D5). Bytes at `Real` = float (GPU and default CPU build); the f64 CPU reference build is not budgeted. No accumulated radiance (ruling 1). Every field any record needs:

| # | field | type | B | condition | needed by |
|---|---|---|---|---|---|
| 1 | `sample_id` | u32 | 4 | always | camera path: its slot in 0008's per-sample buffer (0008 D3; ruling 37); pixel and sample index are derived from it (decision 23). Light path: its rank in the pass's light work, which keys its sampler (0004 D15, D20) |
| 2 | `next_stage` | u8 | 1 | always | 0002 D6 |
| 3 | `term` | u8 | 1 | always | decision 22; 0004 D28; 0008 D12 |
| 4 | `ray_type` | u8 | 1 | always | Blender ray type of the next ray: 0004 D3, D19; 0005 item 33; 0007 decision 7 (mask bits, 0002 D11) |
| 5 | `flags` | u32 | 4 | always | decision 21 |
| 6 | `bounces` | u16[6] | 12 | always | total, diffuse, glossy, transmission, volume, transparent (Blender allows 1024): 0004 D3, D28; 0008 D12 |
| 6a | `event_index` | u16 | 2 | always | sampler block index e (decision 23); stored, not derived (ruling 26): 0004 D14 |
| 7-8 | `ray_o`, `ray_d` | Real3 ×2 | 24 | always | 0004 D1; 0002 D11; geodesic chords (0006 D22) |
| 9 | `cone` | Real2 | 8 | always | ray-cone width and spread: 0005 items 33, 34, 37 |
| 10 | `lambda_t` | Spec | 16 | always | decisions 4-5; 0004 D22; 0005 items 22, 26, 33; 0006 D2 |
| 11-13 | `beta`, `r_u`, `r_l` | Spec ×3 | 48 | always | 0004 D1, D2, D20; 0006 D8 |
| 14 | `eta_scale` | Real | 4 | always | 0004 D2 |
| 15 | `prev_ng` | u32 | 4 | always | octahedral geometric normal of the previous scattering vertex: 0004 D1 |
| 16 | `prev_ns` / `light_index` | u32 | 4 | always | camera path: octahedral shading normal, 0004 D1. Light path (no NEE, no emitter-hit MIS): the emitting light's index, from which its group id and visibility bits are read (0004 D19, D27) |
| 17 | `prev_dist` | Real | 4 | always | `ctx_prev.p = ray_o − prev_dist·ray_d` across pass-throughs and null collisions: 0004 D1 |
| 18-21 | `hit_t`, `hit_prim`, `hit_inst`, `hit_uv` | Real, u32, u32, Real2 | 20 | always | hit record written by `intersect_closest`/`intersect_subsurface`, read by `route_hit`: 0002 D11 |
| | **unconditional** | | **157** | | |
| 23 | `ray_time` | Real | 4 | motion blur | decision 26; 0004 D23; 0007 decision 15 |
| 24 | `ray_tmax` | Real | 4 | curved region or subsurface | geodesic chord length, local-ray length; the only segment length the intersect stage reads (ruling 25): 0002 D11; 0006 D22; ruling 18 |
| 25-26 | `medium_stack`, `medium_rng` | u32[4], u32 | 20 | any medium (incl. world volume and `transmission_depth` > 0, 0005 item 4) or ≥ 2 transmissive materials | 0006 D8, D16 (ruling 8); light paths too (0004 D21) |
| 27 | `min_ray_pdf` | Real | 4 | Filter Glossy on | 0005 item 43; 0004 D17 |
| 28 | `guide_tint` | Real3 | 12 | denoiser guides or light passes | 0008 D10, D15 |
| 29 | `pass_frac` | unorm16[8] | 16 | light passes | 0008 D10 |
| 30 | `stats_time` | u32 | 4 | time statistic | 0004 D28; 0008 D12 |
| 31 | `stats_rays` | u32 | 4 | rays statistic | 0008 D12 |
| | **maximum, every feature on** | | **225** | | budget 256 B (ruling 9): fits |

   Derived, 0 B: pixel and sample index (from `sample_id`, decision 23); `path_kind`, hero lane, partition, medium depth, front face, denoising and shadow-catcher state (flag bits, decision 21); light-path film wavelengths (ruling 7); light-path group id (from `light_index`). Light and shadow linking has no field (ruling 45).
20. **Shadow item** (shadow pool, 0002 D5; self-contained, never reads its creating path). Two kinds, NEE (camera paths, surface and volume NEE) and camera connection (light paths, 0004 D21), told apart by `item_flags`:

| field | type | B | condition | needed by |
|---|---|---|---|---|
| `next_stage` | u8 | 1 | always | 0002 D6 |
| `ray_o`, `ray_d`, `tmax` | Real3, Real3, Real | 28 | always | 0002 D11 (overflow re-queue advances o and tmax to the K-th t); 0004 D4 |
| `time` | Real | 4 | motion blur | 0004 D23; 0007 decision 15 |
| `contribution` | Spec | 16 | always | 0004 D4, D23 |
| `target` | u32 | 4 | always | NEE: `sample_id` of the path (0008 D3). Connection: the light path's `sample_id`, its sampler key (0004 D21) |
| `item_flags` | u8 | 1 | light pass, media or a baked environment world | bit 0 kind; 1 `SECONDARIES_TERMINATED`; 2-3 hero lane; 4 `ITEM_WORLD_LE`; 5-7 unallocated. Connections need bit 1 for `film_weights` at delivery; media need the hero (bits 2-3) for ratio tracking (0006 D8); NEE toward an environment world sets bit 4, and delivery multiplies the world program's radiance along `ray_d` at `lambda_t` (0004 D11) |
| `transparent_left` | u8 | 1 | non-opaque materials | 0004 D4; 0005 item 33 |
| `light_group` | u8 | 1 | light groups | 0004 D27; 0008 D10 |
| `lambda_t` | Spec | 16 | media, non-opaque materials or a baked environment world | opacity T(λ) (0005 items 32-33), media (0006 D19), the environment world's radiance at delivery (0004 D11) |
| `r_u` | Spec | 16 | media | 0006 D8, D19 |
| `r_l` (NEE) / `film_pos` (connection) | Spec / Real2 | 16 (8 without media) | media or light pass | r_l: ratio-tracked MIS of NEE (0006 D19). film_pos: splat position (0004 D23, ruling 2). A connection has one strategy and needs no r_l |
| `medium_stack`, `medium_rng` | u32[4], u32 | 20 | media | 0006 D8, D19 |
| **maximum, every feature on** | | **124** | | budget 128 B (ruling 9): fits after the changes below |

   **What changed to fit.** The requested fields sum to 149 B. (a) The light-pass category and fractions (`pass_cat` u8 + `pass_frac` 16 B, 0008 D10, D23) leave the item for the shadow-pass side buffer below; 0008 loses them as item fields and reads them by shadow slot. (b) `r_l` and `film_pos` share storage, since no item needs both. (c) The connection's packet flags and kind share one byte. (d) The connection's light key shares `target` with the NEE slot. Narrowing `r_u`/`r_l` to fp16 was rejected: MIS weights would no longer sum to 1.
   **Side buffers** (outside both budgets; allocated only with their feature; indexed by slot; neither pool is compacted, 0002 D6; if 0002 adopts main-pool compaction, path-slot side buffers move with it):
   - shadow-pass split, by shadow slot: `pass_cat` u8 (first-vertex class, direct/indirect) + `pass_frac` unorm16[8], 17 B, light passes (0008 D10);
   - `ah_record`, by shadow slot: K × 20 B (t, primitive, instance, u, v) + `hit_count` u8 with overflow bit, 84 B at K = 4, non-opaque instances (0002 D11);
   - geodesic record, by path slot: 164 B double / 92 B float, black hole present (0006 D41);
   - random-walk record, by path slot, subsurface materials present: fields and size are 0005 item 4a's (40 B, PROVISIONAL); this record only allocates it (ruling 27);
   - the pass's active-pixel list is 0008's (D3).
   Their device memory is in 0007 decision 14's budget table (ruling 42).
21. **Path flags (u32).** Assigning a free bit amends this record.

| bits | meaning | needed by |
|---|---|---|
| 0 | `SECONDARIES_TERMINATED` | decision 5; 0004 D1, D5, D22; 0005 items 22, 26 |
| 1 | `WAVELENGTHS_REWRITTEN` | decision 5; 0005 items 22, 26 |
| 2 | `LAST_BOUNCE_SPECULAR` | delta-sample emitter hits: 0004 D1 |
| 3 | `AOV_WRITTEN` | primary-hit AOVs written once across pass-throughs: 0008 D10 |
| 4 | `LAST_SEGMENT_CURVED` | 0006 D28 MIS gate; 0004 D19 disarm |
| 5 | `IN_CURVED_REGION` | 0006 D22 routing |
| 6 | `PATH_KIND` (0 camera, 1 light) | 0002 D5, D8; 0004 D21; 0005 item 8 (`TransportMode`) |
| 7-8 | hero lane | decision 6 |
| 9-11 | partition: armed, chain non-empty, `caster_bit` | 0004 D19 |
| 12-14 | medium-stack depth | 0006 D16 |
| 15 | `DENOISE_SURFACE_DONE` | 0008 D15 |
| 16-17 | first-vertex class: pending, surface, volume | 0008 D10 |
| 18-19 | `SHADOW_CATCHER_HIT`, `SHADOW_CATCHER_PASS` | 0007 decisions 7, 36; Shadow Catcher pass, 0008 D10 (ruling 42) |
| 20 | `FRONT_FACE` | hit side, surviving instance transforms: 0005 items 8, 33; 0006 D16 (ruling 42) |
| 21 | `FIRST_BOUNCE_SPECULAR` | the first bounce sampled a specular, metal or coat lobe: 0008 D15 specular hit distance (ruling 42) |
| 22-31 | false-interface crossings, 0-1023 (the counter of a path's medium-boundary crossings that update only the stack; task 205, the field that exists instead of a new one) | 0006 D17 |

22. **Termination reason (u8)**, the single enum that 0004 D28-D29, 0005 item 4a, 0006 D8, D16, D27-D28 and 0008 D12 count: `ACTIVE, ESCAPED, RUSSIAN_ROULETTE, ZERO_THROUGHPUT, MAX_BOUNCE, TRANSPARENT_LIMIT, ABSORBED, VOLUME_LIMIT, STACK_OVERFLOW, INVALID, CAPTURED, GEODESIC_STEP_LIMIT, GEODESIC_NUMERICAL, LIGHT_PATH_IN_REGION, LIGHT_PATH_END, CANCELLED`. `LIGHT_PATH_END` is a light path ended by 0004 D19's partition rules (discarded at its first vertex x₁ or after its connection; reasons are 0004 D29's counters). A NaN or Inf in beta or a contribution sets `INVALID` and is counted, never dropped silently. Step exhaustion is never folded into `CAPTURED` (v1).

### Sampler (ruling 3)

23. **Definition.** Owen-scrambled Sobol with hash-based padding (Burley 2020, JCGT 9(4); Cycles `kernel/sample/sobol_burley.h`, Apache-2.0; pbrt-v4 `util/lowdiscrepancy.h` `FastOwenScrambler`). Counter-based, keyed on (pixel, sample index, dimension, seed) as spine §3 requires; no state is stored except the variable-stream counters below.
   - **Draw.** `u = sample(id, index, stream, d, seed)`: block b = ⌊d/4⌋, lane j = d mod 4; s = hash(id, seed, stream, b) (pbrt-v4 `MixBits`); the sample index is Owen-shuffled with s (Burley §4 padding); 4-D Sobol with Joe and Kuo 2008 direction numbers gives lane j; that value is Owen-scrambled with hash(s, j) (Burley's Laine-Karras-style hash). `u = (x >> 8)·2⁻²⁴`, exact in float and double. Integer arithmetic only, so every build and target draws identical numbers (0009 decision 17 (i)).
   - **Keys.** Camera path: id = pixel (y·W + x at the allocated film size), index = n + (sample_id mod k), pixel = active list[⌊sample_id / k⌋], with n and k pass-uniform (0008 D3, 0004 D16). Light path: id = 2³¹ + sample_id, index = the pass number in the epoch. One extra 4-B read of the active list replaces a stored pixel and sample.
   - **Seed.** A u32 from Blender's seed (0007 hashes the frame in for an animated seed). It enters only through hashes; no value, 0 included, is special.
   - **Independent mode.** Same keys and dimension map; `u = (pcg4d(id, index, stream·2²⁴ + d, seed).x >> 8)·2⁻²⁴` (Jarzynski and Olano 2020). Samples are mutually independent. Used by DLSS mode (index = frame counter, 0004 D25, 0008 D19), by tests that need an exact variance (0008 D3) and by null calibrations (0009). Selected per session, never per path.
   - **Event index.** e is the path-state field `event_index` (decision 19, stored explicitly, ruling 26), read on arrival at a vertex. It is incremented at every vertex with a random decision: scattering vertices (delta and real volume scatters included) and opacity pass-throughs. Lamp crossings, false-interface crossings and null collisions draw nothing from the fixed map and leave it unchanged (0004 D3, D8, D14; 0006 D17). The segment that arrives at vertex e draws its distance dimensions from block e.
   - **Fixed dimension map (stream 0).** Each 4-D block is jointly stratified.

| dims | camera path | light path |
|---|---|---|
| 0-3 | pixel-filter offset x, y (0008 D5; DLSS mode uses the frame jitter instead, 0008 D19); lens u, v | light pick; position u, v; time (0004 D7, D20) |
| 4-7 | time (0007 decision 15); film wavelength u (decision 2); hero lane ⌊4u⌋ (decision 6); light alias-test uniform, mixed with the event index (ruling 73) | direction u, v (0004 D20); film wavelength u (ruling 7); hero lane |
| 8+12e .. +3 | BSDF/phase direction u, v; lobe, Mix and opacity choice (0005 item 38; pbrt-v4 `SampleDiscrete` rescaling); Russian roulette (0004 D2) | same |
| 8+12e+4 .. +7 | light position u, v; light pick incl. the floor branch (0004 D7, D10, D11); distance-strategy choice (0006 D10) | the connection's lens sample u, v (0004 D23); unused; distance-strategy choice |
| 8+12e+8 .. +11 | extension choice (grating order, fluorescent event: 0005 items 22, 26); extension value (λi, 0005 item 26, 0006 D15); equiangular distance (0006 D10); marcher first sub-step offset (0006 D9) | same |

   - **Light alias-test uniform** (ruling 73, task 074). The alias sampler (0004 D10) takes its bin from 32 bits of the light-pick dimension (offset 6 of the event block) and needs an independent uniform for the alias test: rescaling one uniform loses bits at large light counts (task 067). The event block has no free offset, so camera paths draw it from per-path dimension 7 (`DIM_CAM_ALIAS_Q`, formerly unused), mixed with the event index through a bijective 32-bit mix (`MixBits`, task 020): uniform, and independent of every per-event dimension at each vertex. Light paths use their unused offset 6 the same way when they gain NEE (M5).
   - **Variable-count streams.** Each draw is one 4-D block; the block index is a counter, so a capped launch that re-queues continues bit-identically (0006 D8, ruling 14).

| stream | counter | one block | consumer |
|---|---|---|---|
| 1 path medium | path `medium_rng`: event e owns blocks [e·2²⁰, (e+1)·2²⁰) (2²⁰ = 0006 D8's limit, shared by the event's flights); a flight continues the stored counter when it lies in e's range and starts at e·2²⁰ otherwise, so flights separated by a false interface or a lamp crossing (which leave e unchanged) never share a block (task 205, as Cycles advances `rng_offset` at a volume-bounds crossing); the opacity pass-through that enters a medium sets it to the new event's start, so a reused slot's stale value is never read; a requeued flight continues it. Known limits (BACKLOG): the Sobol dimension 4·block + lane wraps at e ≥ 1024 and the u32 counter at e ≥ 4096; independent mode keeps a dimension below 2²⁴ only for e < 4 | tentative-collision distance; collision event (absorb, scatter, null, fluorescent); overlapping-medium choice; unused | 0006 D8, D15, D18 |
| 2 shadow medium | item `medium_rng`, initialised to e·2¹⁷ (0006 caps 131,072 collisions per shadow segment) | ratio-tracking distance; unused ×3 | 0006 D8, D19 |
| 3 random walk | walk step counter + e·2¹⁶ | free-flight distance; direction u, v; event choice | random-walk subsurface (0005 item 4a) |

### Precision

24. **Real.** `Real` = float on the GPU; the CPU builds float and double (spine §2; 0002 D3). Kernel arithmetic and path state use `Real`; tables are stored as float and widened when read. Double regardless of `Real`: CPU geodesic integration in every build (`GeoReal`; ruling 41; GPU per 0006 D40); host construction of tables and CDFs; unit and photon constant folding; film moments (requirement: < 1e-6 relative error added at 2²⁴ samples per pixel; mechanism 0008 D3).
25. **Planck** in `Real` with λ in µm: `c1L λ⁻⁵ · e^{−x}/(−expm1(−x))`, `x = c2/(λT)`, CODATA 2018 `c1L = 1.191042972e8 W µm⁴ m⁻² sr⁻¹`, `c2 = 14387.7688 µm K`; ×10⁻³ per nm. No overflow over [0.1 nm, 1 m] × [3 K, 1e9 K]. Float accuracy is limited by the condition number x of e^{−x}, hence check 10's tolerance.
26. **Positions and time.**
   - Positions are float32 in scene metres relative to a per-scene origin: the centre of the curved region when one exists, otherwise the centre of the scene bounds. 0007 folds the translation into every transform and the camera at sync. Inside the region the geodesic state is hole-centred in units of r_g (0006 D25).
   - **Large-world check.** The angular error of a camera ray to the nearest visible point is at most ulp(|camera − origin|)/near_clip. Sync warns when it exceeds 1e-4 rad (≈ 0.15 px at 1080p, 40° field of view; DERIVED).
   - `ray_time` is seconds relative to the frame's shutter centre (|t| ≤ shutter/2), so float resolves it; without motion blur it is not stored and reads as 0. The geodesic stage keeps coordinate time in its own `GeoReal` state, relative to its entry point.

### Ray and spacetime

27. **Time along a path.** In flat transport `ray_time` is fixed for the whole path (instantaneous light; motion blur samples time once). Only the geodesic stage advances coordinate time.
28. **Compile-time policy**, a type with no virtual calls, instantiated only in `advance_geodesic` (0002 D8; ruling 18). A policy `ST` provides `kFlat`; `Sphere region()`; `State`; `State enter(o, d, time)`; `rhs(s, ds)` in Hamiltonian form `H = ½ g^{μν} p_μ p_ν`; `hamiltonian(s)`; `max_step(s)`; `captured(s)`, `escaped(s)`; `to_world(s, pos, dir, time)`; `redshift(s, u_from, u_to)`; `tetrad(s, u, frame)`; `static_observer(s, u) -> bool` (false inside the ergosphere; 0006 D23).
   - `enter` accepts o on the region sphere or inside it (a camera inside the region), normalised so that the static observer at o measures frequency 1; the tetrad launch is 0006 D24's.
   - **Region branch.** No ordinary stage holds policy code. When a curved region exists, `route_hit` (0002 D11) takes one launch-uniform branch: it clamps tmax at the region sphere and routes entering rays, rays whose origin lies inside the region, and every result of an `IN_CURVED_REGION` item to `advance_geodesic`; such items trace with `ray_tmax` as tmax. With no region the branch is skipped.
   - `FlatSpacetime` is the no-region case, instantiated in the CPU test build (0006 D23). The shipping policy is Cartesian Kerr-Schild (0006 D23), whose M = 0 case is the Minkowski check. At most one curved region per scene (0006 D25).
29. **What the geodesic round trip does to the path state.** The loop itself is 0006 D22 and D27. `advance_geodesic` writes each chord it emits for intersection (0006 D22, ruling 46) into `ray_o`, `ray_d` and its length into `ray_tmax`, the only copy of the length (ruling 25), and sets `IN_CURVED_REGION`. On a surface hit it calls `apply_frequency_shift` to the hit's static frame, sets `LAST_SEGMENT_CURVED` and clears `IN_CURVED_REGION`. On escape it writes the exit point and direction (never the entry: v1's dark ring), calls `apply_frequency_shift` to infinity and sets `LAST_SEGMENT_CURVED`. Other outcomes end the path with `CAPTURED`, `GEODESIC_STEP_LIMIT` or `GEODESIC_NUMERICAL`.
30. **Connections near the region** are 0006 D28's gate. This record supplies `LAST_SEGMENT_CURVED`, which makes the emitter-hit MIS weight use that gate, and `LIGHT_PATH_IN_REGION` for light paths entering the region.

### Polarisation

31. **Not designed into the path state or BSDF returns**, and nothing is reserved (G4; 0005 item 13). Polarised surface transport needs a Mueller form of every lobe, a second implementation at 1.5-2x time (Mitsuba 3 docs). Polarised astrophysical transfer would live only in the geodesic and medium stages' own state (KPolaris, ipole).

### Challenges to the spine

None backed by evidence. Clarifications: the spacetime policy is instantiated only in `advance_geodesic`, and `route_hit` pays one launch-uniform branch when a region exists; stage-time and ray statistics are conditional fields, so they cost nothing when off; the hero lane is drawn from the counter-based sampler and stored in 2 flag bits.

## Alternatives considered

- **8 lanes.** 197 registers, 32-41% slower (spike A).
- **Hero seed only, wavelengths recomputed** (research 04 R2). Cannot express fluorescence or redshift; the spine requires stored transport wavelengths (16 B).
- **RGB or hybrid transport.** v1 measured about 33% for 4 lanes against RGB; redshift, lines, dispersion and science bands are wrong in RGB.
- **A fixed analytic proposal** (pbrt sech², Cycles' D65 logistic). v1: no single proposal wins; line sources need a scene-built mixture.
- **pbrt-v4's fixed lane-0 hero.** Biased once a lane-0 hero decision precedes termination (decision 6). **Keeping the hero lane at termination**: biased by the same counterexample. Keeping a uniformly random lane drawn independently of h is unbiased but needs a lane rotation carried into the film; lane 0 is simpler.
- **A per-path radiance accumulator.** Shadow results arriving after a path ends would be lost or misrouted; ruling 1 puts every contribution in 0008's per-sample buffer.
- **Storing pixel and sample (8 B).** Derivable from `sample_id` through 0008's active list (decision 23), saving 4 B per path. (Deriving the event index from the bounce counters was dropped by ruling 26.)
- **fp16 `r_u`/`r_l` in shadow items, or a narrower medium stack, to fit 128 B.** The first breaks MIS weight sums; the second contradicts ruling 8. Moving the light-pass split to a side buffer costs no accuracy.
- **r_u and r_l only when media exist.** Two MIS code paths; rejected until state traffic is measured.
- **Blue-noise sample ordering** (Cycles `pattern.h`, after Ahmed and Wonka 2020). Some modes need the sample count up front, and its benefit under OIDN is unmeasured. **Independent hashing as the default**: loses stratification; kept as the independent mode.
- **A user-set render domain.** Derived from the active channels instead.
- **A hard error above 4096 bins.** Widening bins keeps the estimator exact and only costs variance.
- **Camera-relative origin.** Rewrites every instance transform and the TLAS on each camera move. Kept as the remedy if the large-world check fires on real scenes.
- **Peters et al. 2019 moment upsampling.** No head-to-head numbers, licence unchecked (research 05 A5).
- **Photon radiance internally.** The CMFs and Blender expect energy; the photon factor is one multiplication at the film.
- **Generating tables at build time.** Adds generator time to every clean build; committing costs ~28 MB once.
- **Virtual metric** (v1): CPU-only. **Policy code in every intersect step**: puts curved cost on flat rays.
- **1964 10° observer.** Source of v1's most expensive colour bug.

## Consequences

**Easy.** A new band or instrument is a response curve. Fluorescence, gratings and redshift need no state change. Any of Blender's three working spaces works. Lanes can be dropped anywhere without bias analysis. The sampler is bit-identical on CPU and GPU. Flat scenes never run GR code. Every field any record asked for has a place; features that are off allocate nothing.

**Hard.** After dispersion, a grating or fluorescence a path carries one wavelength, so colour noise rises. Line emission through a varying redshift cannot be targeted by a camera-side table (risk 6). RGB-only content outside 360-830 nm is a guess, reported. 157-225 B per path and 49-124 B per shadow item of state traffic. Pixel lookups go through 0008's active list. Blender-unit scenes give science numbers only by convention (flagged).

**Cost when unused.** Curved region: nothing until one exists, then one uniform branch in `route_hit`. Media, motion blur, Filter Glossy, statistics, guides and light passes: no memory or reads when off. Line and response components: host time per epoch. Fluorescence and redshift: the 16 B λt every path carries. Hero lane: 2 flag bits and one sampler draw per path.

## What must exist from the first milestone, and what can wait

Milestones are assigned only in `docs/ROADMAP.md` (G1).

**Must exist from the first milestone, because retrofitting is expensive:**
- The complete path-state and shadow-item tables (decisions 19-20) as 0002's X-macro with feature tags, the flag allocation and the termination enum. Reason: every stage reads them; changing a layout later touches every kernel.
- Stored λt and the packet-operation semantics, including `film_weights`, and the hero lane's flag bits. Reason: the path-state layout and 0008's resolve depend on them.
- `beta`, `r_u`, `r_l` as spectra and the random hero lane with its own dimension. Reason: layout, every MIS weight, every random sequence.
- The sampler with its full dimension map, event-index rule and variable streams. Reason: changing it later changes every random sequence and invalidates every pinned reference result.
- `sample_id` with pixel and sample derived through 0008's active list. Reason: path-state and shadow-item layout, and 0008's slot contract.
- The unit anchor, K and the radiance scale. Reason: they fix every emitter's strength and every reference image.
- `Real` float and double builds, the scene-origin rule and shutter-relative time. Reason: precision of every stage.
- The `WavelengthTable` interface (a table, not an analytic pdf). Reason: `generate_camera`, `generate_light` and the resolve call it.

**Can be added later without redesign:** the `response` and `lines` components and the bin cap; the AP1 and Rec.2020 tables; `rewrite_wavelength` callers (FLUORESCENT, volumetric fluorescence); the geodesic stage, the region branch, `FlatSpacetime` tests and Kerr-Schild (fields and bits are already in the tables); producers of every conditional field; the side buffers; the independent sampler mode; the large-world warning; Wilkie hero MIS for rough dispersion, only with a measured gain.

## Verification

Statistical checks use 0009's gate: Šidák-derived per-test thresholds for the suite's declared family size (0009 decision 15), never a fixed kσ. Every check runs on demand (G3).

| # | Check | Pass criterion |
|---|---|---|
| 1 | Each working-space illuminant through CMF and `M_ws` (unit test) | (1, 1, 1) within 5e-4 (v1 found 3e-4 at 2°) |
| 2 | All `data/spectral` files against `Y_n` | identical `observer_hash`; `Y_n` = 106.856895 ± 1e-6 relative |
| 3 | Jakob-Hanika round trip, Rec.709, 7³ grid on [0.05, 0.95] | max abs error ≤ 1e-3 (v1: 9.9e-4) |
| 4 | Round trip, Rec.2020 and AP1 | max error recorded at generation; gate at recorded + 10% |
| 5 | White reflectance (1, 1, 1) | spectrum = 1 ± 1e-4 at all 471 CMF wavelengths |
| 6 | Illuminant convention: `film_weights` and XYZ by deterministic quadrature over λ | an RGB emitter returns its working-space RGB within 1e-3 relative |
| 7 | Wavelength estimator per component set (display only; display + 100 nm-5 µm band; + 9 lines), 2²⁰ samples; a 200-line lamp table | mean passes the 0009 gate against the analytic integral; pdf > 0 on the support; the 200-line table builds with K ≤ 4096 and reports the widening |
| 8 | Stratification (unit test) | lane CDF positions are exactly `frac(u + i/4)` |
| 9 | Lane dropping: (a) glass with dispersion scale 1e-6 vs 0; (b) a chromatic homogeneous medium (σ_s spanning 10x across lanes) in front of glass with dispersion scale 1e-6 vs 0; (c) scene (b) with the hero fixed at lane 0 | (a), (b) pass the 0009 gate; (c) fails it, so the test discriminates; dispersive vs flat-IOR glass within 2% (v1) |
| 10 | Planck: float vs double, λ ∈ [0.1 nm, 1 m], T ∈ [3 K, 1e9 K] | relative error ≤ (2x + 8)·2⁻²³ where B ≥ 1e-30 of peak (DERIVED); no Inf or NaN; 6500 K chromaticity matches a colour-science oracle to 1e-3 |
| 11 | Unit anchor | an RGB white emitter of strength 1 gives display Y = 1 within 1e-3; `specgen`'s K = 683·Y_n to 1e-9; Blackbody node Y = 1 within 1e-4 at 1000, 6500 and 20000 K |
| 12 | Line lamps: sodium and mercury from line tables | xy within 0.005 of the 1931 2° oracle recomputed with colour-science from NIST lines |
| 13 | Layouts (`static_assert` at `Real` = float) | path state: unconditional fields 157 B, all fields 225 B; shadow item: all fields 124 B; equal to decisions 19-20; the work counters (0009 decision 18) report bytes per path and per item |
| 14 | Hero lane | χ² independence of (h, ⌊4u⌋) over 2¹⁶ paths passes the 0009 gate |
| 15 | Radiance scale | film bit-identical with the automatic s and with s = 1 on an in-range scene; a 1e9 K X-ray source and a 3 K radio source in one render give zero `INVALID` |
| 16 | Region branch cost: `intersect_closest` with the branch vs without, flat 1M-triangle scene | spills and stack identical in the capped build (ruling 17); ≤ 16 extra SASS instructions; runtime ratio within 0009 B-35 |
| 17 | Minkowski through the geodesic stage (Kerr-Schild, M = 0), same seed | per pixel within float conditioning, as 0006 V10 |
| 18 | Redshift | unit test: λ·g and beta·g⁵ to 1 ulp on all lanes, no lane terminated; a static emitter hit inside the region at r gets g = √(1−2M/r); recovered colour temperature g·T within 1% |
| 19 | Region flags | a hit after a chord has `LAST_SEGMENT_CURVED`; an escaped ray restarts at the exit point; a light path entering the region ends with `LIGHT_PATH_IN_REGION` and is counted |
| 20 | Large-world check | camera 1 pc from the origin with near clip 0.1 m warns; camera 1 m from it does not |
| 21 | Working space: ACEScg and Rec.2020 .blend with an emission and albedo chart | directly seen emitters return their RGB within 1e-3 |
| 22 | Fluorescence (with 0005's FLUORESCENT) | emission only at λ > excitation; re-emitted photons = quantum yield × absorbed; no-UV control stays black; NEE on/off agree through a rewrite vertex (0009 gate) |
| 23 | Out-of-range report | an RGB-only material in a 900-1700 nm band render is listed; its value equals s(830) |
| 24 | Sampler identity | CPU, GPU and f64 builds give bit-identical u over 10⁶ (id, index, stream, d) draws (0009 decision 17 (i)); seed 0 passes the same statistics as other seeds |
| 25 | Sampler quality | first 2^m samples of dims (0, 1) of one pixel form a (0, m, 2)-net; χ² uniformity of 16 cross-block dimension pairs passes the 0009 gate; independent mode: per-pixel sample-to-sample correlation within the gate's bound of 0 |
| 26 | Derived keys | the resolve's recomputed (λf, pf) equals `generate_camera`'s for every slot, tail passes included; capped vs uncapped medium tracking bit-identical on the CPU (0006 V6) |

## Risks and open items

1. **Spike E has not run.** 4-lane noise, the default distribution, the band and line weights, the floor and the mixture are PROVISIONAL.
2. **Upsampling cost in the shade kernel is unmeasured** (spike A omitted it; v1: 54% of instructions). Measure against 0002's register gate.
3. **State traffic is unmeasured** (157-225 B per path, up to 124 B per shadow item). Candidates if it hurts: fp16 `cone`, recomputing the hit record.
4. **AP1 saturated corners** lie partly outside the spectral locus; round-trip error unknown until generation.
5. **ACES white assumption**: Blender's OCIO config treats AP1 with the ACES white and applies a CAT only at display. Verify against Blender 5.3.
6. **Line emission through a varying redshift** may be too noisy with camera-side sampling; a deterministic line deposit is 0006's to decide by measurement.
7. **Fluorescence collapse vs per-lane reradiation.** The collapse is conservative; keeping 4 lanes needs a derivation from Mojzík et al. 2018 and Jung et al. 2019 first.
8. **GPU geodesic precision** (0006, S1).
9. **Repository size.** 28 MB of tables; switch to build-time generation if `specgen`'s total run time meets 0009 B-37.
10. **Large-world precision.** Geometry near a camera far from the origin is only detected (decision 26).
11. **Random hero variance** against pbrt's lane-0 hero (in un-terminated paths), measured at equal samples on the CPU (`tools/hero_variance.py`, 64 x 64, 16 seeds per mode, 95% bootstrap intervals over seeds): V_random / V_lane0 with the independent sampler at 256 spp (image mean of VAR) is 0.9996-1.0001 per channel on the chromatic absorbing slab `v1_slab` (intervals within [0.9990, 1.0004]), 1.0000-1.0004 on its chromatic scattering variant (albedo 0.8, g 0.5; within [0.9991, 1.0015]) and 0.9997-1.0001 on the surface-only control `a1_furnace` (within [0.9990, 1.0009]), the exact 1 that the lanes' exchangeability under decision 2's stratified wavelengths predicts; with the Sobol sampler at 128 spp (across-seed variance of the pixel means) it is 0.930-0.995, 0.955-1.019 and 0.975-1.008, every interval holding 1 except the absorbing slab's red channel ([0.864, 0.999]), so the random hero is no noisier; spike E repeats this on a grid medium.
12. **Region means.** On a view-filling constant emitter (`a5_emitter`, 256 x 256, 64 spp, 64 seeds, CPU, `tools/region_bias.py`) the independent sampler's half-image difference has seed-to-seed variance 0.73 to 1.28 times the one the VAR plane predicts; each of the six split-and-channel cells has its own 95% interval (the union is [0.53, 1.88]) and every one holds 1, so a many-pixel mean takes its error from VAR. Biases are within 2.3 seed-SEs, but their signs agree across the independent and Sobol samplers in all six cells (about 1 in 64 by chance), so a small chromatic positional effect of about 0.3 VAR-SE at 64 spp is not excluded. For Sobol a half-difference was measured, and per-pixel scrambling makes the pixels' samples independent, so a half difference is the difference of two region means; its variance is 90 to 200 times below VAR's prediction (VAR is conservative). The CUDA cross-check (8 seeds) reused the CPU's seeds 1 to 8 and is not independent evidence.

**Owner question 1.** Scene-linear 1.0 ≙ Y = 1 W·m⁻²·sr⁻¹·nm⁻¹ (72,983 cd/m²), so Blender watts pass through unchanged and 1 Blender W of white ≈ 470 physical watts in 360-830 nm. Keep this, or give Blender's 1.0 another physical meaning? And should science outputs from scenes lit by Blender-unit emitters be refused rather than flagged?

**Assumptions about other records.**
- **0002:** `Real`, the X-macro field storage with feature tags, uncompacted pools, `route_hit` holding the region branch, the register gate (ruling 17), a no-FMA test build.
- **0004:** permutation-invariant surface decisions; the MIS form; light paths in the main pool with `path_kind`; event-index increments as decision 23 states; counting termination with decision 22's enum.
- **0005:** a convention per slot; K as defined here; extension kinds that call `packet.h`; FLUORESCENT using `rewrite_wavelength` and decision 7's rule; the random-walk record's fields (ruling 27).
- **0006:** line components (D36); hero-lane media sampling; the 20 B medium state; the geodesic round trip, gate and redshift law against decisions 28-30; termination names per decision 22.
- **0007:** working-space and image interop ids; the origin translation and large-world warning at sync; Blender light values passed through; the seed.
- **0008:** film formulas; Y_w = Y_n; the 1/s removal; `PHYSUNIT`; the per-sample buffer, slot assignment and active-pixel list; flag bits 15-17.
- **0009:** tiers, the Šidák family policy, the agreement ladder and work counters.

## Sources

**Project documents.** Research [04](../research/04-integrator-architecture.md) (§3, §5, R2, R5), [05](../research/05-spectral-and-materials.md) Part A, [06](../research/06-astrophysics-core.md) (F3, F6-F8, R1-R6), [07](../research/07-blender-feature-surface.md), [17](../research/17-materials-and-custom-types.md). v1 lessons: [spectral](../v1-lessons/research/spectral.md), [gr-astro](../v1-lessons/research/gr-astro.md), [physics-and-rendering](../v1-lessons/physics-and-rendering.md), [lights](../v1-lessons/research/lights.md). Spikes [A](../spikes/A-kernel-budget/RESULTS.md) §1-3, §6; [B](../spikes/B-blender-end-to-end/RESULTS.md) §1.4. Records [SPINE](SPINE.md), [0000](0000-owner-decisions.md), [RULINGS](RULINGS.md), [0001](0001-product-shape.md), [0002](0002-kernel-architecture.md), [0004](0004-integrator.md), [0005](0005-material-model.md), [0006](0006-volumes-and-astrophysics.md), [0007](0007-blender-integration.md), [0008](0008-film-outputs-denoise.md), [0009](0009-build-test-process.md).

**Papers and standards.** Wilkie, Nawaz, Droske, Weidlich, Hanika 2014, CGF 33(4). Jakob, Hanika 2019, CGF 38(2). Pharr, Jakob, Humphreys 2023, *PBR* 4th ed. (§4.6; §8.7; §14.2). Miller, Georgiev, Jarosz 2019, ACM TOG 38(4). Veach 1997, PhD thesis (MIS; §9.2.4). Hesterberg 1995, Technometrics 37(2). Burley 2020, JCGT 9(4) (Owen-scrambled Sobol). Joe, Kuo 2008, SIAM J. Sci. Comput. 30(5) (direction numbers). Jarzynski, Olano 2020, JCGT 9(3) (pcg4d). Ahmed, Wonka 2020 (alternative only). Peters et al. 2019 (alternative only). Mojzík, Fichet, Wilkie 2018, EGSR (title unconfirmed). Jung et al. 2019, CGF 38(4). Rybicki, Lightman 1979. Chan, Medeiros, Özel, Psaltis 2018, ApJ 867, 59. White 2022, arXiv:2203.15963. Zhang, Chen 2026, arXiv:2609.26298. Moscibrodzka, Gammie 2018, MNRAS 475, 43. Mandal et al. 2021. Tiesinga et al. 2021 (CODATA 2018). CIE 015:2018; CIE 018:2019 (K_m = 683 lm/W); ITU-R BT.709-6, BT.2020-2; SMPTE ST 2065-1, Academy S-2014-004. Mitsuba 3 polarisation documentation.

**Reference implementation files.** pbrt-v4 (Apache-2.0): `util/spectrum.h`/`.cpp` (`SampledWavelengths::SampleVisible`, `TerminateSecondary`, `RGBAlbedo/Unbounded/IlluminantSpectrum`, `RGBToSpectrumTable`, CIE arrays), `util/lowdiscrepancy.h` (`SobolSample`, `FastOwenScrambler`), `util/hash.h` (`MixBits`), `samplers.h`, `util/sampling.h`, `cpu/integrators.cpp` (`VolPathIntegrator::Li`), `wavefront/workitems.h`, `film.h` (`PixelSensor`). rgb2spec `rgb2spec_opt.cpp` (BSD-3). Cycles (Apache-2.0): `kernel/integrator/state_template.h`, `kernel/sample/sobol_burley.h`. ipole (BSD-3), Blacklight (public domain): transfer and step control only.

History: docs/history/0003-review-and-reconciliation.md (review and reconciliation of this record; the Decision section above is current).
