# 0003 — Spectral model, units, path state, precision and spacetime hooks

Status: proposed (2026-11-14); revised after independent review (2026-11-14)

## Context

Astroray 2.0 renders spectra, not RGB. A path carries a small set of wavelengths. Materials, media and lights are evaluated at those wavelengths. Only the film turns radiance into display RGB, sensor bands or photon counts. Wavelengths can change partway along a path: dispersion and gratings make direction depend on wavelength, fluorescence moves energy from one wavelength to another, and Doppler and gravitational redshift scale wavelength. The first astrophysics target is a black hole inside an emission nebula (owner decision 8), so curved rays, redshift and line emission must fit into the core without being added on later. Fluorescence must cover all of its use cases (19). Gratings and the light-tracing pass they need are in the first release (17), and the first release has the full feature breadth (7).

Binding constraints:
- **Spine §2.** 4 wavelengths: 8 was measured at 197 registers, against a budget of 168. Runtime flags, not template matrices. Records are passed by reference.
- **Spine §3.** Physical units inside the engine. 4 stored, rewritable transport wavelengths, kept separate from the film wavelengths. The wavelength sampling distribution is data. One observer. SoA path state with a counter-based sampler and diagnostics. A compile-time spacetime policy whose flat case costs nothing.
- **Spine §6.** Output is scene-linear in the file's working space. **Spine §7.** A double-precision CPU reference for GR.
- **Owner priorities.** Viewport speed, short builds, simplicity, scientific accuracy.

Ownership boundaries used here: 0004 owns the sampler, its dimension map and seed policy (D13-D15) and the light-tracing method; 0006 owns line shapes, media, the geodesic integrator and the redshift law; 0008 owns film formulas (bands, photon counts, étendue). This record owns the wavelength table mechanism, the packet operations, the unit anchor, the path-state layout, the flag and termination enums, precision rules and the spacetime policy interface.

Evidence used: spike A (registers, CPU/GPU agreement, timing noise); spike B (scene-linear present path under Blender's view transform); research 04, 05 Part A, 06 (R1-R6) and 17; v1 lessons (observer bug, upsampling nonlinearity, hero collapse, band window, line SPDs, 116x anchor error, GR redshift); pbrt-v4 source read 2026-11-14 (`util/spectrum.h` `TerminateSecondary`; `cpu/integrators.cpp` `VolPathIntegrator::Li`).

## Decision

### Wavelengths

1. **Lane count.** `kLanes = 4` is a compile-time constant in `core/spectrum.h`, the only place that defines `Spec` (`Real v[4]`, radiometric values) and `Lambda` (`Real nm[4]`, nm). MEASURED: spike A §2-3; with 8 lanes the fused kernel uses 197 registers and throughput falls 32% uncapped, 41% capped.
2. **Film wavelengths are sampled once and never stored.** One sampler dimension (0004 D14) gives `u`. Lane `i` uses `u_i = frac(u + i/4)`, `λf_i = F⁻¹(u_i)`, `pf_i = p(λf_i)`: stratification in CDF space (pbrt-v4 `SampleVisible`, after Wilkie et al. 2014; v1 measured that offsetting wavelengths themselves is biased under a non-uniform pdf). `(λf, pf)` is recomputed from the path's or photon's sampler key and the table wherever needed: 0 bytes of state.
3. **The wavelength distribution is a table** (`WavelengthTable`): K+1 bin edges (float, nm, non-uniform) and K+1 CDF values; pdf constant per bin; binary search. Built by the host in double once per render epoch, immutable within it, shared by camera paths and photons (0004 D22).
   - **Support** = the union of the supports of the active film channels (0008 §7-8). Display channels are always active, so the support always contains [360, 830] nm and the visible component is never empty. Channel curves are accepted on [0.1 nm, 1e9 nm], the range the Planck test covers (check 10); a curve outside it is refused with an error. There is no separate user-set render domain.
   - **Components, selected by the host from the film and the scene (no user setting):**
     - `visible`, always: pbrt's sech² pdf on [360, 830], 1024 uniform bins. PROVISIONAL until spike E (v1 found no proposal that wins everywhere).
     - `response`, when band or cube channels are active: p ∝ Σ R_c over those channels, mixed with `visible` at 0008 §8's weight (PROVISIONAL, spike E).
     - `lines`, when the scene has line emitters: components `(λ_l, σ_l, weight)` supplied by their owners. Media lines follow 0006 D31 (σ = √(σ_l² + (λ_l·v_max/c)²), weight = scene-integrated line power); lamp line tables supply their own widths and line powers. Bins near a line are at most FWHM/8 wide over ±4 FWHM (FWHM = 2.3548σ). Mixture (1−α)·rest + α·lines, α = 0.5 PROVISIONAL (spike E).
   - **Defensive floor.** A 5% uniform component over the support (PROVISIONAL; Hesterberg 1995). The build asserts pdf > 0 wherever any channel responds.
   - **Bin cap.** K ≤ 4096 (PROVISIONAL). If line components need more, the builder merges overlapping line windows, then widens line bins uniformly until K fits. The table is still the exact pdf, so only variance changes; the per-render report says so.
4. **Transport wavelengths.** `λt[4]` is stored (16 B) and set to λf at path start. Values may lie anywhere in (0, ∞).
5. **Packet operations**, in `core/packet.h`, their only owner (v1: the collapse rescale needs one guarded owner):
   - `terminate_secondaries(λt, beta, flags)`: copies lane 0 of `λt` and `beta` into lanes 1-3 and sets `SECONDARIES_TERMINATED`; `r_u`, `r_l` are untouched. Afterwards every lane evaluates at λt[0], so every lane reduction (mean, max) equals lane 0 and later per-lane pdf ratios are 1. Used by dispersion, gratings and any event whose direction depends on wavelength.
   - `rewrite_wavelength(λt, beta, λi, ratio, flags)`: fluorescence. The caller has terminated secondaries and drawn λi from the excitation distribution for λt[0]; this sets `beta *= ratio` (= M(λi→λt[0])/p(λi), per-nm reradiation), all lanes of λt to λi, and `WAVELENGTHS_REWRITTEN`. The film still weights by λf[0].
   - `apply_frequency_shift(λt, beta, g, flags)` (0006 D26's name): for a change of frame with `g = ν_from/ν_to = (k·u_from)/(k·u_to)`, `from` on the camera side. Sets `λt_i *= g` on every lane, `beta *= g⁵`, `WAVELENGTHS_REWRITTEN`. It never terminates lanes: geodesics are achromatic. g⁵ is the wavelength form of the invariant I_ν/ν³, namely I_λλ⁵ (Rybicki and Lightman 1979 §1.5).
   - `film_weights(flags, pf) -> Spec`: `(1/pf_0, 0, 0, 0)` if `SECONDARIES_TERMINATED`, else `w_i = 1/(4pf_i)`, applied to the whole accumulated L. This is pbrt-v4's arithmetic (`TerminateSecondary` sets pdf[1..3] = 0 and pdf[0] /= 4; the sensor averages L/pdf over 4 lanes).
   - `hero(flags)`: the path's hero lane (decision 6).
6. **Every lane's estimator is unbiased on its own**, so dropping or rewriting lanes never biases the result.
   - **Surface decisions** (Mix select, lobe, direction, light choice, Russian roulette) use permutation-invariant reductions over lanes (mean or max; 0004 D2's max_λ qualifies), never one lane. Their pdf is then the same for every lane, so surfaces need no per-lane pdf bookkeeping.
   - **Hero-sampled decisions** (null-scattering event and distance, wavelength-dependent phase) use the path's hero lane `h`, drawn uniformly from {0..3} once per path from its own sampler dimension, independent of the wavelength dimension (requirement on 0004 D14), and stored in 2 flag bits. Then `beta = f/p_h` (f over the actual sampling pdf) and `r_u,j *= p_j/p_h`, so `beta/avg_j(r_u,j) = f/((1/4)Σ_j p_j)`: the one-sample balance heuristic over 4 techniques, each chosen with probability 1/4 (Veach 1997 §9.2.4). It is unbiased for each lane separately and needs no exchangeability argument.
   - **Why not pbrt literally.** pbrt-v4 fixes the hero at lane 0 (`pdf = T_maj[0] * mp.sigma_s[0]` in `VolPathIntegrator::Li`) and keeps lane 0 at termination. Its 4-lane average is correct only while all lanes reach the film. Counterexample (DERIVED): 2 lanes, uniform λ pdf, a discrete event x ∈ {1, 2}, p_a = (0.9, 0.1), p_b = (0.1, 0.9), f_a = (1, 0), f_b = (0, 1), target 1. Keeping the hero lane gives an expectation of 1.8; a random hero gives 1.
7. **MIS across wavelengths.** pbrt-v4's volpath formulation: `beta`, `r_u`, `r_l` are each a `Spec`; NEE and BSDF weights use lane averages of r_u and r_l; chromatic media follow Miller, Georgiev and Jarosz 2019 as in pbrt (149x lower MSE on pbrt's chromatic scene, as reported), with `h` in place of lane 0.
   - **After termination**, averages still run over 4 lanes: lanes 1-3 keep the pre-termination ratios that the earlier hero decisions need, and post-termination ratios are 1 because the lanes are identical.
   - **After a rewrite** (fluorescence draw, frequency shift): the event is shared by every strategy that can produce the path (NEE and BSDF sampling both continue from the rewritten vertex), so it scales the numerator and denominator of every ratio alike. `beta` absorbs M/p(λi) or g⁵; `r_u` and `r_l` are unchanged. This is the convention 0005 item 26 and 0004 (Risks) require before fluorescence lands.
   - Hero-wavelength MIS for rough dispersive transmission (Wilkie 2014) is not built; it may be added only with a measured variance reduction of at least 10% at equal time (v1's park criterion).

### Ranges

8. **Outside a source's range: clamp and report.** A spectral source evaluated outside its defined range returns its edge value and is listed once in the per-render report with the requested range.
   - RGB reflectance and unbounded sources: the sigmoid at clamp(λ, 360, 830). RGB illuminant: 0 outside [360, 830], so fluorescent brighteners need a spectral light that contains UV.
   - Tables: their end values. Cauchy/Abbe: defined on [360, 830], so it clamps. Planck and Gaussian lines: analytic everywhere.
   - When the table support extends past [360, 830], the report lists every RGB-only material and light.

### Units and colour

9. **Internal units.** Spectral radiance L_λ in W·m⁻²·sr⁻¹·nm⁻¹ in the frame of the static observer at the vertex where the current segment starts. Wavelength in nm (vacuum), length in scene metres (0007 applies Blender's unit scale), time in seconds relative to the frame's shutter centre (decision 26). No radiance is RGB anywhere in transport.
10. **Radiance scale.** Per render the host picks `s = 2^k` so that the largest emitter spectral radiance over the table support (from the emitter power estimates of 0004 D9-D10) lies in [1, 2). `s` is folded into every emitter's strength on the host (lights, surface emission, world, medium j); the film multiplies by 1/s in double (requirement on 0008). Float then stays in range across [0.1 nm, 1e9 nm] × [3 K, 1e9 K] (research 06 R3). A power of two is exact, so the output does not depend on s.
11. **Display and the unit anchor.**
   - **Definition.** `Y(s) := ∫ s(λ) ȳ(λ) dλ / Y_n` over 360-830 nm, with `Y_n = ∫ ȳ dλ` from the shipped CMF table (106.856895 nm). Display: `XYZ_k = Σ_i w_i L_i k̄(λf_i) / Y_n`, so display Y = Y(L). In 0008 §7's notation, `Y_w = Y_n × 1 W·m⁻²·sr⁻¹·nm⁻¹ = 106.856895 W·m⁻²·sr⁻¹`. Working RGB = `M_ws · XYZ`, no chromatic adaptation. "Normalised to Y = 1" anywhere in this record means Y(s) = 1.
   - **Anchor.** Scene-linear 1.0 ≙ Y(L) = 1 W·m⁻²·sr⁻¹·nm⁻¹ ≙ luminance 683 lm/W × Y_n = **72,983 cd/m²** (K_m = 683 lm/W, CIE 018:2019).
   - **K for 0005.** `emission_luminance = Strength × K` with `K = 683·Y_n cd/m² = 72,983` (generated by `specgen` from the shipped table, never typed); the engine's radiance is `L(λ) = (emission_luminance / K) · S_ill(colour; λ)`.
   - **Blender lights.** One Blender "watt" is one Y-normalised unit: the spectral quantity is 0004 D6's geometric form × P × S_ill(colour; λ), and for the sun E × S_ill. The factor from Blender's numbers is exactly 1; 0007 passes them through. Consequence (derived): 1 Blender W of equal-energy white radiates 470 W in 360-830 nm, i.e. 72,983 lm.
   - **Blackbody node** → Planck(T)/Y(Planck(T)), so "colour × strength" keeps its Blender meaning.
   - **Physical-unit inputs** (Astroray spectral lights in W or lm, astrophysical tables) enter unscaled.
   - **Science outputs.** Blender-unit emitters are physical only through this convention. When a band, cube or photon channel is requested and any Blender-unit emitter (RGB or Blender-watt light, RGB emission, RGB world) is enabled, the per-render report lists them as a warning and the science file carries `PHYSUNIT = F` (requirement on 0008). Not refused (owner question 1).
12. **RGB and Blender inputs.** RGB radiances from Blender (emission colour × strength, light colour, world colour, environment textures) use the illuminant convention (decision 16), so they render back as the same working-space RGB to within table error.
13. **Bands and photons** are 0008's formulas (§7 response channels; §9 photon counts with pixel étendue A∫cosθ dω). This record requires only that film conversion uses λf (observed), never λt, and that h and c are the exact CODATA 2018 values, folded in double on the host.
14. **One observer.** CIE 1931 2°, 1 nm steps, 360-830 nm (471 samples).
   - **Source.** pbrt-v4 `src/pbrt/util/spectrum.cpp` (Apache-2.0), arrays `CIE_X/Y/Z`, `CIE_Illum_D6500`, `ACES_Illum_D60`, at a pinned commit; provenance and the CIE dataset DOI in `data/MANIFEST.toml` (0001 D8.2). The 1964 10° observer is not in the repository.
   - **Generated together.** `tools/specgen` produces from that one input set: Y_n and K; D65 and ACES-white SPDs with Y = 1; `M_ws` for `lin_rec709_scene` (BT.709-6, D65), `lin_rec2020_scene` (BT.2020-2, D65) and `lin_ap1_scene` (AP1, ACES white x 0.32168, y 0.33767); the upsampling tables.
   - **Consistency.** Every data file carries the same `observer_hash`; the engine refuses a set whose hashes differ.
15. **Working space.** At sync, 0007 reads `bpy.data.colorspace.working_space_interop_id` and selects `(M_ws, illuminant, table)`. Any other value refuses the render with a visible error. All RGB from Blender is linear in that space; images decode by `Image.colorspace_settings.interop_id`, and `data` images skip upsampling. Film output is scene-linear in the same space and Blender applies the view transform (spike B §1.4: AgX applied once). A change of working space resets the epoch and resyncs materials.

### RGB-to-spectrum upsampling

16. **Method.** Jakob and Hanika 2019: `s(λ) = ½ + x/(2√(1+x²))`, `x = c0λ² + c1λ + c2`, λ in nm.
   - **Table.** 64³ (PROVISIONAL), pbrt's layout `float[3][64][64][64][3]` = 9.4 MB per space; scale axis `smoothstep(smoothstep(k/63))`. Lookup: bisection on the scale axis (pbrt `FindInterval`), trilinear coefficients. CPU and GPU share the layout; only the active space is uploaded.
   - **Conventions** (pbrt-v4): reflectance clamps RGB to [0,1] (reported); unbounded uses m = max(r,g,b), `s = 2m·S(rgb/2m)`; illuminant = unbounded × I_ws(λ) with Y(I_ws) = 1. Zero RGB gives an exact zero spectrum; negative components clamp to 0 and are reported. 0005 fixes a convention per slot: [0,1] colours reflectance, radiances illuminant, everything else unbounded (answers research 17's review finding).
17. **Where and how often.** Once per shading point, after texture filtering, in material evaluation, at λt; all nonlinear material maths per lane afterwards. Scalar factors above 1 (η², strength) are flat multipliers after upsampling (v1: 2.5% chroma error otherwise). Textures store RGB, never coefficients.
18. **Table generation.** `tools/specgen` is one C++ file adapted from `rgb2spec_opt.cpp` (BSD-3, in `THIRD_PARTY.md`), built on demand, not in the default target. Outputs `data/spectral/{rec709,rec2020,ap1}.jh` (header: res, observer_hash, illuminant id, generator version) plus `MANIFEST.toml` entries, committed (~28 MB) and embedded as a binary resource (0001). Regenerated only when inputs change; the merge gate checks hashes and round trips and never regenerates.

### Path state

19. **Main path state.** SoA, one array per field; 0004 sets the pool size. Unconditional fields total **180 B per path** (PROVISIONAL: computed, traffic not measured; 180 MB for a 1M-path pool):

| # | field | type | B | meaning |
|---|---|---|---|---|
| 1 | `pixel` | u32 | 4 | film index |
| 2 | `sample` | u32 | 4 | sample number within the pixel |
| 3 | `bounces` | u16[6] | 12 | total, diffuse, glossy, transmission, volume, transparent (Blender allows up to 1024) |
| 4 | `term`, `ray_type`, `prev_light_set` | u8, u8, u16 | 4 | termination reason (decision 22); Blender ray type for visibility and Light Path; light-link set of the previous scattering vertex (0004 D7, D10) |
| 5 | `flags` | u32 | 4 | decision 21 |
| 6-8 | `ray_o`, `ray_time`, `ray_d` | Real3, Real, Real3 | 28 | origin, time (decision 26), unit direction |
| 9 | `cone` | Real2 | 8 | ray-cone width and spread (0005 items 34, 37) |
| 10 | `lambda_t` | Spec | 16 | transport wavelengths, nm |
| 11-13 | `beta`, `r_u`, `r_l` | Spec ×3 | 48 | throughput and MIS ratios |
| 14 | `L` | Spec | 16 | radiance per lane, deposited once at termination via `film_weights` (0004 D5) |
| 15 | `eta_scale` | Real | 4 | Russian-roulette correction (pbrt) |
| 16 | `prev_ng`, `prev_ns` | u32 ×2 | 8 | octahedral geometric and shading normals of the previous scattering vertex. The shade stage builds its own light-sample context from these quantised values, so NEE and the later BSDF-hit pmf see identical inputs and MIS weights sum to 1 |
| 17 | `prev_dist` | Real | 4 | distance since the last scattering vertex, so `ctx_prev.p = ray_o − prev_dist·ray_d` across opacity pass-throughs and null collisions |
| 18-21 | `hit_t`, `hit_prim`, `hit_inst`, `hit_uv` | Real, u32, u32, Real2 | 20 | intersection record |

20. **Conditional and stage-owned state.**
   - **Conditional fields**, allocated only when needed; kernels test a launch-uniform flag: `medium_stack` u32[4], 16 B, when volumes exist (0006 D15); `stats_time` and `stats_rays` u32 each, when that statistic is requested (0008 §12); `guide_tint` Real3, 12 B, while denoiser guides are produced (0008 §15); light-pass category weights, size set by 0008, when light passes are enabled (0008 §10).
   - **Stage-owned records**, never in the main state:
     - shadow item: 56 B = o, d, tmax, time, contribution `Spec`, path index or splat position, `shadow_set` u16, `transparent_left` u8 (0004 D4), `light_group` u8 (0008 §10); plus 52 B with media = λt, r_u, r_l, medium;
     - photon state (0004 D21). Photons draw wavelengths from the same table as camera paths (0004 D22), so their film wavelengths are recomputed from the photon key and the table like a camera path's; a splat converts with `film_weights` at the photon's own λf and pf, with beta₀ = Le(λ)/(P·pmf·pdf_pos·pdf_dir) per lane. Textured emitters therefore need no per-light SPD;
     - geodesic state (0006 D36); random-walk subsurface state (0005).
21. **Flags (u32).** Bit allocation:

| bits | owner | meaning |
|---|---|---|
| 0-6 | 0003 | `SECONDARIES_TERMINATED`, `WAVELENGTHS_REWRITTEN`, `LAST_BOUNCE_SPECULAR`, `ANY_NON_SPECULAR`, `AOV_WRITTEN`, `LAST_SEGMENT_CURVED`, `IN_CURVED_REGION` |
| 7-8 | 0003 | hero lane (decision 6) |
| 9-11 | 0004 D19 | partition: armed, chain non-empty, caster |
| 12-14 | 0006 D15 | medium-stack depth |
| 15-18 | 0008 | denoising surface found (1 bit), light-pass category (3 bits) |
| 19-31 | reserved | allocated only by amending this record |

22. **Termination reason (u8)**, the single enum that 0004 D28, 0006 D8/D15/D23-24 and 0008 §12 count: `ACTIVE, ESCAPED, RUSSIAN_ROULETTE, ZERO_THROUGHPUT, MAX_BOUNCE, TRANSPARENT_LIMIT, ABSORBED, VOLUME_LIMIT, STACK_OVERFLOW, INVALID, CAPTURED, GEODESIC_STEP_LIMIT, GEODESIC_NUMERICAL, LIGHT_PATH_IN_REGION, CANCELLED`. A NaN or Inf in beta or L sets `INVALID` and is counted, never dropped silently. Step exhaustion is never folded into `CAPTURED` (v1).

### Sampler

23. **The sampler is 0004's** (D13-D15: generator, dimension map, photon keys, variable-count streams, seed policy). This record requires from it: one wavelength dimension per camera path and per photon; one per-path hero-lane dimension independent of the wavelength dimension (decision 6); one per-bounce dimension for a wavelength rewrite (the fluorescence λi draw); integer arithmetic, so CPU and GPU draw identical numbers.

### Precision

24. **Real.** `Real` = float on the GPU; the CPU builds float and double (spine §2). Kernel arithmetic and path state use `Real`; tables are stored as float and widened when read. Double regardless of `Real`: CPU geodesic state (`GeoReal`; GPU per 0006 D35); host construction of tables and CDFs; unit and photon constant folding; film accumulation for final, test and science renders (requirement: < 1e-6 relative error added at 2²⁴ samples per pixel; mechanism 0008 §3).
25. **Planck** in `Real` with λ in µm: `c1L λ⁻⁵ · e^{−x}/(−expm1(−x))`, `x = c2/(λT)`, CODATA 2018 `c1L = 1.191042972e8 W µm⁴ m⁻² sr⁻¹`, `c2 = 14387.7688 µm K`; ×10⁻³ per nm. No overflow over [0.1 nm, 1 m] × [3 K, 1e9 K]. Float accuracy is limited by the condition number x of e^{−x}, hence check 10's tolerance.
26. **Positions and time.**
   - Positions are float32 in scene metres relative to a per-scene origin: the centre of the curved region when one exists, otherwise the centre of the scene bounds. 0007 folds the translation into every transform and the camera at sync. Inside the region the geodesic state is hole-centred in units of r_g (0006 D21).
   - **Large-world check.** The angular error of a camera ray to the nearest visible point is at most ulp(|camera − origin|)/near_clip. Sync warns when it exceeds 1e-4 rad (≈ 0.15 px at 1080p, 40° field of view; DERIVED).
   - `ray_time` is seconds relative to the frame's shutter centre (|t| ≤ shutter/2), so float resolves it. The geodesic stage keeps coordinate time in its own `GeoReal` state, relative to its entry point; slow-light write-back (0006, later) writes a difference, never an absolute time.

### Ray and spacetime

27. **Time along a path.** In flat transport `ray_time` is fixed for the whole path (instantaneous light; motion blur samples time once). Only the geodesic stage advances coordinate time.
28. **Compile-time policy**, a type with no virtual calls, instantiated only in the geodesic stage (its own kernel and queue, spine §2). The flat intersect stage contains no policy code: when a curved region exists it takes one launch-uniform branch that clamps `tmax` at the region's bounding sphere and queues entering paths; with no region the branch is skipped. A policy `ST` provides `kFlat`; `Sphere region()`; `State`; `State enter(o, d, time)`; `rhs(s, ds)` in Hamiltonian form `H = ½ g^{μν} p_μ p_ν`; `hamiltonian(s)`; `max_step(s)`; `captured(s)`, `escaped(s)`; `to_world(s, pos, dir, time)`; `redshift(s, u_from, u_to)`; `tetrad(s, u, frame)`.
   - `enter` accepts o on the sphere (a flat ray entering) or inside it (a camera inside the region: ray generation then queues camera rays directly to the geodesic stage), normalised so that the static observer at o measures frequency 1. A camera inside the ergosphere is refused (0006).
   - `FlatSpacetime` (0006 D20) is the no-region case and compiles to nothing. The first real policy is Cartesian Kerr-Schild (Chan et al. 2018; 0006 D20), whose M = 0 case is the Minkowski check. At most one curved region per scene (0006 D21).
29. **Segment loop** (geodesic stage), until a limit is reached:
   - take an integrator step (0006 D22); trace the chord between `to_world` of the two states against the ordinary acceleration structure (Gröller 1995); media on the chord are marched by 0006 D9/D25;
   - on a surface hit: place it on the chord, call `apply_frequency_shift` with g from `redshift` between the static observer at the entry point (or camera) and the static observer at the hit, set `LAST_SEGMENT_CURVED`, and return the path to shading;
   - on `captured`: terminate with `CAPTURED`;
   - on `escaped`: write the exit point, direction and time (the exit, never the entry: v1's dark ring), call `apply_frequency_shift` with g between entry and exit observers, set `LAST_SEGMENT_CURVED`, return the path to the flat intersect queue;
   - when steps run out: `GEODESIC_STEP_LIMIT`; on Hamiltonian drift or NaN: `GEODESIC_NUMERICAL` (0006 D23).
30. **Connections across a curved region** (0006 D24).
   - A straight NEE or light-tracing connection whose segment meets the region's sphere contributes 0. A path with `LAST_SEGMENT_CURVED` gives NEE pdf 0 in the MIS weight of the emitter it hits, so the strategies that can generate the path keep weights summing to 1 (Veach 1997).
   - Delta lights (radius-0 point and spot, zero-angle sun) inside the region are refused with a visible error at sync: no strategy can reach them.
   - Delta-light NEE samples zeroed by a region crossing (a light behind the region) are counted per pass; the per-render report gives the count and the lights involved (rule 7).
   - Light-tracing paths that enter the region terminate with `LIGHT_PATH_IN_REGION`, counted and reported.

### Polarisation

31. **Not designed into the path state or BSDF returns.** Kept at zero cost: an explicit orthonormal shading frame per vertex; Fresnel helpers returning s and p terms; `Spec` in one header. Polarised surface transport needs a Mueller form of every lobe, a second implementation at 1.5-2x time (Mitsuba 3 docs); not planned for 2.0. Polarised astrophysical transfer would live only in the geodesic and medium stages' own state (KPolaris, ipole).

### Challenges to the spine

None backed by evidence. Clarifications:
- The spacetime policy is instantiated only in the geodesic stage; the flat intersect stage pays one launch-uniform branch when a region exists, not zero instructions.
- Stage-time and ray statistics are conditional, so they cost nothing when off.
- The per-path hero lane (decision 6) is drawn from the counter-based sampler and stored in 2 flag bits; no sampler state is stored.

## Alternatives considered

- **8 lanes.** 197 registers, 32-41% slower (spike A).
- **Hero seed only, wavelengths recomputed** (research 04 R2). Cannot express fluorescence or redshift; the spine requires stored wavelengths (16 B).
- **RGB or hybrid transport.** v1 measured about 33% for 4 lanes against RGB; redshift, lines, dispersion and science bands are wrong in RGB.
- **A fixed analytic proposal** (pbrt sech², Cycles' D65 logistic). v1: no single proposal wins; line sources need a scene-built mixture. A table keeps one code path at about 12 bisection steps per lane per sample (unmeasured).
- **pbrt-v4's fixed lane-0 hero kept literally.** Biased once a lane-0 hero decision precedes termination (decision 6 counterexample). **Keeping a uniformly random lane at termination** is also unbiased but needs a lane rotation carried into the film's wavelength recomputation; the per-path hero lane is simpler.
- **Per-lane pdf bookkeeping on surfaces** (decisions from lane 0 only). Permutation-invariant decisions avoid it.
- **A user-set render domain.** Derived from the active channels instead: one fewer setting, and the visible component can never be empty.
- **A hard error above 4096 bins.** Widening bins keeps the estimator exact and only costs variance.
- **Camera-relative origin.** Rewrites every instance transform and the TLAS on each camera move, against the viewport budget. Kept as the remedy if the large-world check fires on real scenes.
- **Peters et al. 2019 moment upsampling.** No head-to-head numbers, licence unchecked (research 05 A5). Jakob-Hanika has BSD-3 tooling and v1 measured its round trip at 9.9e-4.
- **Photon radiance internally.** The CMFs and Blender expect energy; the photon factor is one multiplication at the film.
- **r_u and r_l only when media exist.** Two MIS code paths; rejected until state traffic is measured.
- **Atomic film deposit per contribution.** Rejected for a per-path L with one deposit (0004 D5).
- **Generating tables at build time.** Adds generator time to every clean build (5-minute budget); committing costs ~28 MB once.
- **Virtual metric** (v1): CPU-only. **Policy code in every intersect step**: puts curved cost on flat rays. **Exit-ray teleport only**: fails with geometry inside the region.
- **A polarised build now.** 1.5-2x time and a second BSDF implementation, with no scheduled use.
- **1964 10° observer.** Source of v1's most expensive colour bug.

## Consequences

**Easy.** A new band or instrument is a response curve. Fluorescence, gratings and redshift need no state change. Any of Blender's three working spaces works. Lanes can be dropped anywhere without bias analysis. The sampler is bit-identical on CPU and GPU. Flat scenes never run GR code.

**Hard.** After dispersion, a grating or fluorescence a path carries one wavelength, so colour noise rises. Line emission through a varying redshift cannot be targeted by a camera-side table (risk 6). Polarised surfaces would be a rewrite of every lobe. RGB-only content outside 360-830 nm is a guess, reported. 180 B per path of state traffic. Blender-unit scenes give science numbers only by convention (flagged).

**Cost when unused.** Curved region: nothing until one exists, then one uniform branch per intersect launch. Volumes, statistics, guides and light passes: no memory or reads when off. Line and response components: host time per epoch. Unused working-space tables: disk only. Polarisation hooks: 0. Fluorescence and redshift: the 16 B λt every path carries. Hero lane: 2 flag bits and one sampler draw per path.

## What lands when

**First milestone.**
- `spectrum.h`; `packet.h` with `terminate_secondaries`, `film_weights`, `hero` and `apply_frequency_shift` (0006 D26 requires its unit test now).
- `WavelengthTable` with the `visible` component, the floor and stratified sampling; the XYZ→working-space film for all three spaces; the unit anchor, K and the radiance scale.
- `specgen`, CMF and illuminant data, the three Jakob-Hanika tables.
- The full unconditional path state, the flag allocation and the termination enum; the medium-stack, statistics and guide-tint conditional fields (0006, 0004 D28 and 0008 land their producers in this milestone); the hero lane with media r_u bookkeeping (homogeneous media are in this milestone, 0006).
- `Real` float and double builds; Planck; `ray_time`; the origin rule (bounds centre until regions exist).

**Before the first release.** Dispersion termination and grating collapse; the `response` and `lines` components (lamp line tables) and the bin cap; band channels and photon counts (0008) with the `PHYSUNIT` flag; photon splat conversion; light-pass weights.

**After the first release** (the astro milestone). The geodesic stage, the intersect-stage region branch and the segment loop with `apply_frequency_shift` at hits and exits; camera-inside entry; the connection rules and reports of decision 30; Kerr-Schild with a double CPU reference; GPU precision by S1; media line components (0006 D31). `rewrite_wavelength` lands with 0005's FLUORESCENT kind, which 0005 schedules after the first release (owner question 2).

**Not scheduled.** Polarisation; hero MIS for rough dispersion.

## Verification

Statistical checks use 0009's gate: Šidák-derived per-test thresholds for the suite's declared family size (0009 §15-16), never a fixed kσ.

| # | Check | Pass criterion |
|---|---|---|
| 1 | Each working-space illuminant through CMF and `M_ws` (unit test) | (1, 1, 1) within 5e-4 (v1 found 3e-4 at 2°) |
| 2 | All `data/spectral` files against `Y_n` | identical `observer_hash`; `Y_n` = 106.856895 ± 1e-6 relative |
| 3 | Jakob-Hanika round trip, Rec.709, 7³ grid on [0.05, 0.95] | max abs error ≤ 1e-3 (v1: 9.9e-4) |
| 4 | Round trip, Rec.2020 and AP1 | max error recorded at generation; gate at recorded + 10% |
| 5 | White reflectance (1, 1, 1) | spectrum = 1 ± 1e-4 at all 471 CMF wavelengths |
| 6 | Illuminant convention: `film_weights` and XYZ by deterministic quadrature over λ | an RGB emitter returns its working-space RGB within 1e-3 relative |
| 7 | Wavelength estimator per component set (display only; display + 100 nm-5 µm band; + 9 lines), 2²⁰ samples; a 200-line lamp table | mean passes the 0009 gate against the analytic integral of a known spectrum × response; pdf > 0 on the support; the 200-line table builds with K ≤ 4096 and reports the widening |
| 8 | Stratification (unit test) | lane CDF positions are exactly `frac(u + i/4)` |
| 9 | Lane dropping: (a) glass with dispersion scale 1e-6 vs 0; (b) a chromatic homogeneous medium (σ_s spanning 10x across lanes) in front of glass with dispersion scale 1e-6 vs 0; (c) scene (b) with the hero fixed at lane 0 | (a), (b) pass the 0009 gate; (c) fails it, so the test discriminates; dispersive vs flat-IOR glass within 2% (v1) |
| 10 | Planck: float vs double, λ ∈ [0.1 nm, 1 m], T ∈ [3 K, 1e9 K] | relative error ≤ (2x + 8)·2⁻²³ where B ≥ 1e-30 of peak (DERIVED: condition number x of e^{−x} plus the λ⁻⁵ product); no Inf or NaN; 6500 K chromaticity matches a colour-science oracle to 1e-3 |
| 11 | Unit anchor | an RGB white emitter of strength 1 gives display Y = 1 within 1e-3; `specgen`'s K = 683·Y_n to 1e-9; Blackbody node Y = 1 within 1e-4 at 1000, 6500 and 20000 K |
| 12 | Line lamps: sodium and mercury from line tables | xy within 0.005 of the 1931 2° oracle recomputed with colour-science from NIST lines |
| 13 | Path-state layout | `static_assert`: unconditional sizes sum to 180 B at `Real` = float, equal to decision 19's table |
| 14 | Hero lane | χ² independence of (h, ⌊4u⌋) over 2¹⁶ paths passes the 0009 gate |
| 15 | Radiance scale | film bit-identical with the automatic s and with s = 1 on an in-range scene; a 1e9 K X-ray source and a 3 K radio source in one render give zero `INVALID` |
| 16 | Region branch cost (astro milestone): intersect stage with the branch vs without, flat 1M-triangle scene | registers and spills identical; ≤ 16 extra SASS instructions; median-of-3 runtime ratio ≤ 1.05 (spike A §1: differences under 5% are noise) |
| 17 | Minkowski through the geodesic stage (Kerr-Schild, M = 0), same seed | per pixel within float conditioning, as 0006 V10 (spike A §6 tolerances) |
| 18 | Redshift (astro milestone) | a static emitter hit inside the region at r gets g = √(1−2M/r) through `apply_frequency_shift`; recovered colour temperature g·T within 1%; unit test (M1): λ·g and beta·g⁵ to 1 ulp on all lanes, no lane terminated |
| 19 | Region connections (astro milestone) | a delta light inside the region is refused; a sun behind the region gives a non-zero blocked count in the report; a camera inside the region reproduces 0006 V11's shadow edge within 0.5 px |
| 20 | Large-world check | camera 1 pc from the origin with near clip 0.1 m warns; camera 1 m from it does not |
| 21 | Working space: ACEScg and Rec.2020 .blend with an emission and albedo chart | directly seen emitters return their RGB within 1e-3 |
| 22 | Fluorescence (A12, with 0005's FLUORESCENT) | emission only at λ > excitation; re-emitted photons = quantum yield × absorbed; no-UV control stays black; NEE on/off agree through a rewrite vertex (0009 gate) |
| 23 | Out-of-range report | an RGB-only material in a 900-1700 nm band render is listed; its value equals s(830) |

## Risks and open items

1. **Spike E has not run.** 4-lane noise, the default distribution, α, the floor and the mixture weights are PROVISIONAL.
2. **Upsampling cost in the shade kernel is unmeasured** (spike A omitted it; v1: 54% of instructions). Measure at the first milestone against the 168-register budget.
3. **180 B per path of traffic is unmeasured.** Candidates if it hurts: fp16 `r_u`/`r_l`/`cone`, recomputing the hit record.
4. **AP1 saturated corners** lie partly outside the spectral locus; round-trip error unknown until generation.
5. **ACES white assumption**: Blender's OCIO config treats AP1 with the ACES white and applies a CAT only at display. Verify against Blender 5.3 at the first milestone.
6. **Line emission through a varying redshift** may be too noisy with camera-side sampling; a deterministic line deposit is 0006's to decide by measurement.
7. **Fluorescence collapse vs per-lane reradiation.** The collapse is conservative; keeping 4 lanes needs a derivation from Mojzík et al. 2018 and Jung et al. 2019 first.
8. **GPU geodesic precision** (0006, S1).
9. **Unit anchor** (owner question 1).
10. **Repository size.** 28 MB of tables; switch to build-time generation if `specgen` runs in under 20 s total.
11. **Large-world precision.** The origin rule fixes the hole's neighbourhood; geometry near a camera far from the origin is only detected (decision 26). Camera-relative origin is the remedy.
12. **Random hero variance.** Unbiased by construction; its variance against pbrt's lane-0 hero (in un-terminated paths) is unmeasured. Spike E's chromatic medium scene should record it.
13. **Conflicts with other records, for the lead:**
    - 0004 D13 says no seed value is special; 0009 §16 maps seeds to non-zero because "0003 rejects 0". This record no longer owns seeds; one of the two must change.
    - 0004 D14 must allocate the hero-lane dimension (a reserved slot in dims 4-7) and a per-bounce rewrite dimension; D15 likewise for photons.
    - 0006 D8 and D12 say "lane 0" for event probabilities and phase sampling; under decision 6 they read "the path's hero lane".
    - 0004 D28 counts 8 termination reasons in `u16[8]` and 0006 names `OUT_OF_STEPS` and `NUMERICAL`; decision 22's enum has 15 values and replaces both lists; 0008 §12's list must match it.
    - 0004 D6 gives the Blender-watt factor to 0007; decision 11 fixes it at 1, so 0007 passes Blender values through.
    - 0006 D20 lands `FlatSpacetime` "in every ordinary stage" in the first milestone; here the intersect stage holds no policy code and the region branch lands with the geodesic stage (rule 7: no feature lands switched off).
    - 0006 D23 describes entry only at the sphere; decision 28 adds entry at a camera inside the region and decision 30 refuses delta lights inside it.

**Owner questions.**
1. **Unit anchor.** Scene-linear 1.0 ≙ Y = 1 W·m⁻²·sr⁻¹·nm⁻¹ (72,983 cd/m²), so Blender watts pass through unchanged and 1 Blender W of white ≈ 470 physical watts in 360-830 nm. Keep this, or give Blender's 1.0 another physical meaning? And should science outputs from scenes lit by Blender-unit emitters be refused rather than flagged?
2. **Fluorescence timing.** 0005 schedules FLUORESCENT after the first release. Is that acceptable under owner decisions 7 and 19, or must fluorescence ship in the first release?

**Assumptions about other records.**
- **0002:** `Real`, SoA accessors, the 168-register gate, a no-FMA test build.
- **0004:** pool size; sampler, dimension map and seeds, including the dimensions decision 23 requires; stage order (shadow before film deposit); the MIS heuristic; light tracing with photon wavelengths from the shared table (D22); permutation-invariant surface decisions; counting termination reasons with decision 22's enum.
- **0005:** a convention per slot; K as defined here; Cauchy dispersion; extension kinds that call `packet.h`; FLUORESCENT using `rewrite_wavelength` and decision 7's MIS rule.
- **0006:** line components for the table (D31); hero-lane media sampling; medium-stack contents; the geodesic integrator and Kerr-Schild policy against decision 28; the redshift law and marcher; termination names per decision 22.
- **0007:** working-space and image interop ids; the origin translation and large-world warning at sync; Blender light values passed through; the per-render report.
- **0008:** film formulas for bands and photon counts; Y_w = Y_n; the 1/s radiance-scale removal; `PHYSUNIT`; accumulation mechanism; flag bits 15-18; termination list per decision 22.
- **0009:** tiers, the Šidák family policy, the A-scenes cited above.

## Sources

**Project documents.** Research [04](../research/04-integrator-architecture.md) (§3, §5, R2, R5), [05](../research/05-spectral-and-materials.md) Part A, [06](../research/06-astrophysics-core.md) (F3, F6-F8, R1-R6), [07](../research/07-blender-feature-surface.md), [17](../research/17-materials-and-custom-types.md). v1 lessons: [spectral](../v1-lessons/research/spectral.md), [gr-astro](../v1-lessons/research/gr-astro.md), [physics-and-rendering](../v1-lessons/physics-and-rendering.md), [lights](../v1-lessons/research/lights.md). Spikes [A](../../spikes/A-kernel-budget/RESULTS.md) §1-3, §6; [B](../../spikes/B-blender-end-to-end/RESULTS.md) §1.4. Records [SPINE](SPINE.md), [0000](0000-owner-decisions.md), [0001](0001-product-shape.md) D8, [0004](0004-integrator.md), [0005](0005-material-model.md), [0006](0006-volumes-and-astrophysics.md), [0008](0008-film-outputs-denoise.md), [0009](0009-build-test-process.md).

**Papers and standards.** Wilkie, Nawaz, Droske, Weidlich, Hanika 2014, CGF 33(4). Jakob, Hanika 2019, CGF 38(2). Pharr, Jakob, Humphreys 2023, *PBR* 4th ed. (§4.6; §14.2). Miller, Georgiev, Jarosz 2019, ACM TOG 38(4). Veach 1997, PhD thesis (MIS; §9.2.4 one-sample model). Hesterberg 1995, Technometrics 37(2). Burley 2020, JCGT 9(4). Peters et al. 2019 (alternative only). Mojzík, Fichet, Wilkie 2018, EGSR (title unconfirmed). Jung et al. 2019, CGF 38(4). Rybicki, Lightman 1979. Chan, Medeiros, Özel, Psaltis 2018, ApJ 867, 59. White 2022, arXiv:2203.15963. Zhang, Chen 2026, arXiv:2609.26298. Moscibrodzka, Gammie 2018, MNRAS 475, 43. Gröller 1995, Visual Computer 11(5). Mandal et al. 2021. Tiesinga et al. 2021 (CODATA 2018). CIE 015:2018; CIE 018:2019 (K_m = 683 lm/W); ITU-R BT.709-6, BT.2020-2; SMPTE ST 2065-1, Academy S-2014-004. Mitsuba 3 polarisation documentation.

**Reference implementation files.** pbrt-v4 (Apache-2.0): `util/spectrum.h`/`.cpp` (`SampledWavelengths::SampleVisible`, `TerminateSecondary`, `RGBAlbedo/Unbounded/IlluminantSpectrum`, `RGBToSpectrumTable`, CIE arrays), `util/sampling.h`, `cpu/integrators.cpp` (`VolPathIntegrator::Li`), `wavefront/workitems.h`, `film.h` (`PixelSensor`). rgb2spec `rgb2spec_opt.cpp` (BSD-3). Cycles (Apache-2.0): `kernel/integrator/state_template.h`. ipole (BSD-3), Blacklight (public domain): transfer and step control only.

## Review and resolution

1. Sampler conflicts with 0004 — ACCEPTED: dimension map, pcg4d keying and seed policy deleted; 0004 owns them; decision 23 lists only what this record needs (wavelength, hero-lane and rewrite dimensions). The seed-0 conflict between 0004 and 0009 is listed for the lead.
2. Unit anchor unowned and ambiguous — ACCEPTED: Y(s) := ∫sȳdλ/Y_n defined; Y_w = Y_n stated for 0008; K = 683·Y_n = 72,983 cd/m² for 0005; Blender-light factor fixed at 1; science channels from Blender-unit emitters are flagged (`PHYSUNIT = F`, report warning). Refusing instead is owner question 1.
3. MIS after termination or rewrite undefined — ACCEPTED in substance, fix REJECTED as proposed: copying pbrt-v4 literally (hero fixed at lane 0, lane 0 kept) is biased when chromatic hero sampling precedes termination (two-lane counterexample: 1.8 vs 1). Decision 6 now draws a per-path random hero so every lane is unbiased alone; decision 7 fixes the rules after termination and rewrites (beta = f/p_actual; shared events leave r_u, r_l unchanged); check 9 adds the reviewer's chromatic-medium-then-dispersive-glass test plus a lane-0-hero variant that must fail; fluorescence now lands with 0005's kind, after the first release. The max_λ point is rejected: max is permutation-invariant, as decision 6 now states.
4. No redshift for hits inside the region — ACCEPTED: renamed to `apply_frequency_shift`, called at every hit and exit; it lands in the first milestone with its unit test; check 18 covers the static emitter.
5. Duplicated formulas — ACCEPTED: photon and band formulas deferred to 0008 (étendue); line components take 0006 D31's Doppler-broadened width; this record owns one termination enum that replaces the 0004/0006/0008 lists (conflict listed).
6. Astrophysical scale in float32 — ACCEPTED: origin at the curved-region centre; ray time relative to the shutter centre, geodesic time region-local; radiance scale s = 2^k restored (decision 10); channel range [0.1 nm, 1e9 nm] matches the Planck test; a large-world warning added.
7. Silent energy loss across the region — ACCEPTED: delta lights inside the region refused, blocked delta-light samples counted and reported, a camera inside the region handled by `enter` (camera inside the ergosphere refused, per 0006).
8. Incomplete state — ACCEPTED: second normal, previous light-link set, ray type, `prev_dist`, u16 bounce counters, shadow-link set, transparent counter and light group in the shadow item, flag bits for 0004/0006/0008, conditional light-pass weights and guide tint; total 180 B, and the `static_assert` checks that number.
9. Film conversion for light paths — ACCEPTED: photons use the shared table (0004 D22), so `film_weights` applies with the photon's own λf; the emitter-SPD state (32 B) is removed and textured emitters need no per-light SPD.
10. Thresholds — ACCEPTED: all statistical checks use 0009's Šidák gate; check 16 is ≤ 1.05 plus deterministic register and instruction checks; the Minkowski check is deterministic per 0006 V10; the Planck tolerance is (2x + 8)·2⁻²³.
11. Hooks switched off, builder selection — ACCEPTED: the region branch lands with the geodesic stage; components are selected automatically from the film channels and scene; the support always contains the visible band; over 4096 bins, line bins widen with a report (the pdf stays exact).
