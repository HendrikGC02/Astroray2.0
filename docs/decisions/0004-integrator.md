# 0004 — Integrator: transport algorithms, lights, sampling, light tracing and diagnostics
Status: proposed (2026-11-14), revised after independent review (2026-11-14)

## Context

The integrator turns the synced scene (geometry, OpenPBR records, media, lights, world) into film samples. The owner's decisions bind it: correctness against physics, not Cycles (0000 #1); one kernel source for CPU and GPU (#2); full breadth in the first release, including sky, world and lights (#7); a viewport at 24-60 updates/s with a first frame comparable to Cycles (#9, #10); heat maps of sample count, bounce count and similar statistics (#15); diffraction gratings in the first release, with the light-tracing path that makes them useful designed in from the first milestone (#17); fluorescence for all its use cases (#19).

The spine fixes the frame: wavefront stages written once; surface shading as one fused BSDF+NEE kernel under `__launch_bounds__` (148 registers uncapped, 128 capped, 22% faster than split: spike A, MEASURED); structurally different work, including the light-tracing pass and the extension-lobe stage, gets its own kernel and queue; every kernel within 0002's budgets; 4 stored, rewritable transport wavelengths; a counter-based sampler with no stored state; colour-aware adaptive sampling; per-pixel moments and statistics buffers. Research file 04 (R1-R12) is the starting design. This record turns it into concrete algorithms on 0002's pools, queues and kernel list, 0003's path state and sampler, and 0008's film.

Unidirectional path tracing cannot render the owner's named effects. A grating order and a smooth dispersive refraction are delta in direction for each wavelength. Point, spot, zero-angle sun and collimated area lights are delta in position or direction. So a path light → delta chain → diffuse surface → camera (Heckbert notation L S+ D E) has zero probability from the camera side, and with small non-delta lights it shows up as fireflies. v1 tried camera-side manifold methods on prisms and failed (no focusing on flat faces, chaotic Newton basins); forward photon transport is what shipped (`v1-lessons/research/transport.md`). The review of research file 17 records the same gap for gratings, CD scenes and Mie rainbows.

v1 also left lessons that constrain this record: one-sided MIS biased direct light; light-tree seams and a sun starved by mesh emitters; a colour-blind adaptive metric that left a blue variance floor; clamps that capped real caustic energy; light-energy conventions found one bug at a time; a general stochastic photon loop on a solid prism that looked like chromatic noise and passed metric gates (`v1-lessons/research/lights.md`, `transport.md`, `physics-and-rendering.md`, `testing-and-process.md`).

## Decision

**Challenges to the spine: none.** Two clarifications. (a) Radiance is accumulated per *sample* in a pass-lifetime buffer, not per path slot (D5), so shadow items stay self-contained as 0002 requires. (b) The light-tracing pass uses 0002's `generate_light` and `shade_light_path` kernels plus a light variant of each extension identity, so extension code stays out of the common light-path kernel (spine §2).

Number labels: MEASURED (spike or source measurement cited), SOURCE (a constant taken from a cited implementation), DERIVED (computed from cited numbers), PROVISIONAL (a budget to confirm by the named measurement).

### A. Transport core

- **D1. Estimator.** Unidirectional wavefront path tracing. At every non-delta vertex (surface or medium scatter): one light sample (NEE) plus one BSDF or phase sample to continue. This is Veach's multi-sample MIS model with one sample per strategy, weighted by the balance heuristic in pbrt-v4 `VolPathIntegrator`'s rescaled-probability form: beta, r_u and r_l are 4-wide spectra, so the weights also combine wavelengths (Veach and Guibas 1995; pbr-book 4e §14.2; research 04 R6). It is the one formulation for surfaces and media; volume NEE uses it too (conflict with 0006 #11, Risks).
  - Delta lights: NEE weight 1; BSDF-strategy pdf 0.
  - Emitter hit by a BSDF ray: weighted with `LightSampler::pmf(ctx_prev, light) * light.pdf_li(ctx_prev, wi)`, using the *previous* vertex's context, the same selection pdf and the same solid-angle measure as NEE (v1 seam fix).
  - Convention: `f` excludes the cosine. The integrator multiplies |cos θ_i| against the shading normal exactly once.
- **D2. Russian roulette.** Rule from pbrt-v4 VolPath: at depth ≥ max(2, Blender min bounces), q = max(0, 1 − max_λ(beta·η_scale / avg(r_u))); survivors are weighted 1/(1−q) (Arvo and Kirk 1990). The max over lanes is lane-symmetric (0003 #6). η_scale undoes the radiance η² so refraction does not trigger termination. Light paths apply the same rule to power. The uniform is slot 6 of the event block (D14).
- **D3. Bounce semantics.** Per-type limits (total, diffuse, glossy, transmission, volume, transparent) are honoured as Blender defines them (`v1-lessons/research/transport.md`): an opacity pass-through is a null event that advances only the transparent counter and keeps the ray direction; a lamp crossing is a pass-through, not a vertex (D8); at the cap, a transmission sample keeps its weight. These counters are Blender's limits only; the sampler's event index is separate and also advances on opacity pass-throughs (D14). Each path end records a termination reason (D28).
- **D4. Shadow stage.**
  - The shade stage computes the unoccluded contribution (light sample × f × |cos| × MIS weight) and emits a self-contained shadow item (0002 D5): ray, tmax, contribution at the 4 lanes, sample slot (D5), light-group id and pass category (D27), plus 0003 #21's media fields. It never reads or writes its creating path.
  - The shadow stage follows 0002 D11: terminate on the first opaque hit; null (opacity) surfaces pass, evaluated with the shadow-ray program (Blender's Is Shadow Ray, path flags none), within the transparent-bounce budget; media contribute transmittance (0006). A surface whose shadow-ray evaluation has a non-null BSDF blocks, smooth glass included; the partition relies on this (D19).
  - Delivery adds the contribution to the sample slot (D5) and to enabled light-group and light-path planes (D27).
- **D5. Per-sample accumulation buffer.** The fix for path lifetime: a path may end, and 0002 may refill its slot, while its shadow items are still in flight.
  - Each camera sample of the current pass owns a slot in a sample buffer: `L[4]` fp32 at lane index i (transport lane i is weighted by film wavelength i) plus a u32 flags word, 20 B (PROVISIONAL). The buffer has one slot per pixel at render resolution (1080p 41 MB, 4K 166 MB); a pass never holds more samples than that (D16 tail rule). Slot = the sample's rank in the pass.
  - Every contribution (emission found by the path, a lamp crossing, a shadow delivery) goes through one function, `sample_add(slot, contribution)`, an fp32 atomic add (0002 D2: several shadow items of one sample can be delivered in one launch). At path end the path writes its final flags (`SECONDARIES_TERMINATED`, 0003 #5) into the slot.
  - 0002 drains every pass before the next starts (its end-of-pass rule). Then a resolve kernel calls 0008's `film_write_sample(pixel, L, flags)` once per pixel, looping over that pixel's slots (one writer per pixel, plain fp64 adds into S1, S2 and S1A), and clears the buffer.
  - Consequences: per-sample moments, the A/B split and the D17 clamps stay exact; 0003's `film_weights` still applies to the whole sample as in pbrt; the path state loses `L` (−16 B). The camera film is reproducible to the summation order of a sample's own contributions on the GPU (0002 D12) and bit-exact on the CPU with one thread.
  - Requirements: 0003 removes `L` (#20 field 13) and the shadow item's path index becomes the sample slot (#21); 0008 #3 is fed by the resolve kernel, not by the path-end stage (Risks).

### B. Lights

- **D6. Light types and energy conventions.** Internal quantities are spectral and in physical units. ŝ(λ) is the light's normalised emission spectrum from colour, temperature or a table (0003 upsampling as illuminant). 0007 owns the scale factor from Blender's watts to physical units; this table fixes the geometry, taken from v1's Cycles-semantics findings.

| Type | Blender input | Internal quantity | NEE sampling | Hit by BSDF rays |
|---|---|---|---|---|
| Point, radius 0 | power P | intensity I = P/(4π)·ŝ | delta, pdf 1 | no |
| Point/sphere, radius r > 0 | P | radiance L = P/(4π²r²)·ŝ (area-consistent) | visible-cone sampling (pbrt-v4 `Sphere::Sample`) | yes |
| Spot | P, angle, blend | as point or sphere; × smoothstep blend in cosine space; not renormalised to the cone | as point/sphere | if r > 0 |
| Area: rectangle, square, disk, ellipse | P, spread | L = P/(πA)·ŝ, one-sided; spread attenuation `max(tan h − tan a, 0)·N`, N = 1/(tan h − h) (Cycles soft-box, power-preserving); spread → 0 is collimated | rectangles: spherical rectangle (Ureña, Fajardo, King 2013, as in Cycles `light/area.h`); disk and ellipse: area sampling | yes |
| Sun, angle θ | strength E [W/m²] | irradiance E·ŝ, independent of θ; radiance E/Ω inside the cone, Ω = 2π(1 − cos θ/2); θ = 0 is delta | uniform cone | yes if θ > 0 |
| Emissive mesh | material emission | OpenPBR emission radiance (0005) | D9 | yes |
| World: HDRI, sky, constant | world node program | radiance | D11, D12 | escaped rays |

  - IES profiles are an angular multiplier on intensity with absolute candela scale (constant versus spectrum-based lumen scaling is 0005's or 0007's choice). The light's power estimate integrates the profile.
  - Blender's per-light ray-visibility flags become instance-mask bits on lamp primitives (D8) and are honoured by light paths (D19).
- **D7. Light interface (design level).** One definition, compiled for both targets. "Absent" is expressed in the type, never as a sentinel value; a distant light reports `is_infinite`, not a distance of 1e30.
  - `LightSampleContext { p, n_geom, n_shade, kind: point | segment(o, d, t_max), light_set }`. The segment form serves medium NEE (Conty Estevez and Kulla 2018 §5.3; 0006).
  - `sample_li(ctx, u2, λ[4]) → { wi, dist_or_infinite, pdf_solid_angle, is_delta, p_light }`, deterministic in (ctx, u2).
  - `pdf_li(ctx, wi, hit) → pdf`.
  - `sample_le(u2_pos, u2_dir, λ[4], target_sphere) → { ray, Le, pdf_pos, pdf_dir }`, for light paths (pbrt-v4 `Light::SampleLe` covers every type).
  - `power(weighting) → scalar` for D10's importance and radiometric power, and `bounds() → { bbox, θ_o, θ_e, two_sided }` for the tree.
- **D8. Lamp intersection.** One crossing per step, no cap.
  - Area and sphere lamps are primitives in the scene acceleration structure: rectangles as two triangles, spheres as OptiX built-in spheres, disks and ellipses as one custom primitive with a purely geometric intersection program (Embree: user geometry). A lamp instance carries the light's ray-visibility bits and never the shadow bit, so lamps never block shadow rays.
  - `route_hit` sends a lamp hit to a small `shade_light` stage. For camera paths it adds the MIS-weighted, front-facing Le (lamp emission programs, IES and node graphs, run by the VM as emission-only programs, 0005), weighted by transmittance in a medium (0006), via `sample_add`. It then re-queues `intersect_closest` from just past the lamp with the same direction; bounce counters and the event index do not change. Light paths pass through lamps with no contribution.
  - Cost: one extra step and one traversal per lamp crossed, only on rays that cross a lamp; crossings are counted (D29). This is Cycles' structure (`kernel/integrator/shade_light.h` re-queuing `intersect_closest`).
  - Requirements on 0002: the lamp primitive types (D11) and the `shade_light` kernel (D8).
- **D9. Emissive geometry.**
  - Every emissive triangle is a light, i.e. a leaf of the light sampler.
  - Power estimate = area × π × emission from the material compiler's constant or texture-average estimate (0005), weighted as in D10.
  - Sampling: spherical triangle (Arvo 1995) when the subtended solid angle is in pbrt-v4 `Triangle::Sample`'s range, otherwise uniform area.
  - A triangle with estimate 0 has selection pmf 0, also under the D10 floor, so its BSDF hits take MIS weight 1. Required for unbiasedness and tested.
  - Sidedness follows Blender as read by 0005: θ_e = π/2 for one-sided emitters and π for two-sided ones.
- **D10. Light selection.**
  - `LightSampler { sample(ctx, u) → (light, pmf); pmf(ctx, light) }`.
  - **Importance weighting.** w(λ) = R̂_film(λ) + R̂_exc(λ): R̂_film is the normalised sum of the active film responses (CMFs for display, band curves for science); R̂_exc is the normalised sum of the excitation spectra of the fluorescent materials in the scene (0005 FLUORESCENT), absent when there are none. A light's importance energy is ∫ S(λ) w(λ) dλ, never luminance. Fluorescence moves transport wavelengths into the excitation band (0003 #5), so a UV lamp lighting a brightener has importance even when only the display film is active.
  - **Floor.** pmf(ℓ) = (1 − ε)·p_imp(ℓ) + ε·p_rad(ℓ), where p_rad is proportional to radiometric power ∫ S(λ) dλ over all wavelengths and ε = 0.05 (PROVISIONAL; defensive mixture, Hesterberg 1995, as 0003 #3). `sample` draws from a radiometric alias table with probability ε and otherwise from the importance sampler; `pmf` returns the mixture. Every light with non-zero power therefore has a non-zero pmf, which delta lights need because NEE is their only strategy.
  - First milestone: the importance sampler is an alias table over *all* lights, environment and sun included (pbrt-v4 `AliasTable`, `PowerLightSampler`).
  - Before the first release: a light tree, on by default (Conty Estevez and Kulla 2018), following Cycles `kernel/light/tree.h` and `scene/light_tree.cpp`: distance clamp max(0.5·|centroid − bbox.max|, d); minimum/maximum importance blend; distant lights and the background inside the tree; stack-free descent with a bit trail for `pmf`; one tree per light set for light and shadow linking (research 07 F5).
  - The alias table stays as the test oracle. Fixed-ratio environment-versus-lamp splitting is not used (v1 rejected it).
- **D11. Environment importance.**
  - The table is built by evaluating world radiance, weighted by w(λ), on an equal-area square grid (pbrt-v4 `EqualAreaSquareToSphere`, after Clarberg 2008): 1024² by default (PROVISIONAL; Cycles' default world map resolution is 1024), evaluated on the GPU and rebuilt only on world edits.
  - Sampled with pbrt-v4 `PiecewiseConstant2D`. NEE uses the MIS-compensated distribution (Karlík et al. 2019; pbrt-v4 `allowIncompletePDF`); light-path emission uses the uncompensated one.
  - Radiance at a sampled direction always comes from the world program, never from the table.
- **D12. Sky and sun.** This record owns the sky model (0005 #36's `SKY` op; 0007 schedules Sky before the first release).
  - **Model.** Blender's Single Scattering (Nishita et al. 1993) and Multiple Scattering sky types, enumerated from RNA at implementation. Ported from Cycles `intern/sky/source/sky_single_scattering.cpp` (Apache-2.0) and `sky_multiple_scattering.cpp` (MIT), with shader-side evaluation and the sun-disc limb profile from `kernel/svm/sky.h` (Apache-2.0). `sky_nishita.h` and `sky_math.h` (GPL-2.0-or-later) are neither read nor copied (`v1-lessons/blender-integration.md`, `research/lights.md`). Preetham and Hosek-Wilkie come after the first release (0007); selecting them shows a visible error.
  - **Spectral evaluation.** On each world edit the host precomputes, in double, a sky radiance table over view direction relative to the sun × the source's own wavelength samples (table size PROVISIONAL), keeping the source's absolute units; the `SKY` op interpolates it at the transport wavelengths and clamps outside its range with a report (0003 #9). If a source turns out to compute only XYZ or RGB, that type is upsampled as an illuminant (0003 #17) and reported `approximated`. No fitted exposure constant (v1); Blender's strength and sun-intensity inputs multiply the result and 0007 owns any unit scale.
  - **Sun-disc split.** When the disc is enabled and the compiler (0005) proves that the world output radiance is G·Sky(ω) + R(ω), with G a direction-independent spectrum after constant folding (the chain from the `SKY` op to the output contains only ops linear in that input with constant factors: Background strength, Multiply, Add, Mix with a constant factor), the disc becomes a distant-light leaf: irradiance G·E_disc independent of disc size, the source's limb profile applied identically in NEE and in BSDF-hit evaluation, camera-visible (v1). The `SKY` op then evaluates without the disc, and the importance table excludes it, so sky and sun are two lights with separate pdfs (v1 measured a 1.3% low bias from a profile used only in NEE).
  - Otherwise the disc stays in the world program and is sampled only through the table, and a visible warning names the node that broke linearity (CLAUDE.md rule 7).

### C. Sampler

- **D13. Sampler.** The sampler is 0003's (#24): Owen-scrambled Sobol (Burley 2020), stateless and integer-exact on both targets, plus pcg4d for variable-count consumers. This record adds no sampler, no blue-noise mode and no reserved dimension.
- **D14. Dimension use on camera paths** (requirements on 0003 #24).
  - The per-bounce block index b is the path's **event index**: it advances at every vertex that makes a random decision, including opacity pass-throughs, which do not advance Blender's bounce counters (D3). Without this, stacked alpha cards reuse one uniform: 10 layers at α = 0.5 would pass together with probability 0.5 instead of 0.5^10. Lamp crossings make no decision and do not advance it.
  - Slot use per event, as 0003 lists it: 0 lobe or event (Mix Shader selection then lobe selection share it by rescaling, pbrt-v4 `SampleDiscrete` as 0005 #38; also the opacity decision); 1-2 direction; 3 light pick (the D10 floor branch rescales it); 4-5 light position; 6 Russian roulette; 7 extension (grating order, fluorescence λi).
- **D15. Light paths** draw every random number from 0003's pcg4d stream keyed on (light-sample index, pass, seed, depth·16 + stage, counter): light pick, position, direction, wavelengths, time, lobe, extension, Russian roulette, the connection's lens sample and the splat-pixel choice (D23).

### D. Adaptive sampling

- **D16. Stop metric and schedule.** The film keeps per pixel the full sum and the class-A sum S1A (0008 #2), so per channel c (X, Y, Z for display, or each sensor band) the mean I_c and the class-A estimate A_c = 2·S1A_c/n exist.
  - Error per channel: e_c = |I_c − A_c| / (ε + g(I_c)), g(x) = √x for x < 1 and x otherwise, ε = 1e-4. This is Cycles' `film/adaptive_sampling.h` form (SOURCE), applied per channel instead of summed over channels. Cycles scales by film exposure; here the inputs are pre-exposure values, because `film_exposure` is display-only (D25) and converged flags must not go stale. At exposure 1 the threshold means what it means in Cycles. Science bands are scaled by the band's image mean (PROVISIONAL).
  - A pixel has converged when max_c e_c < threshold. The summed form is rejected because a dim channel adds little to the numerator and nothing to the normaliser (research 04 §7; v1 blue floor; Dammertz et al. 2010 use colour noise).
  - Schedule (Cycles, SOURCE): check when (n + 1) mod 16 = 0, after min samples (Blender setting, 0 = Cycles' auto formula); dilate the unconverged mask by one pixel in x and then in y; `generate_camera` skips converged pixels.
  - **Tail rule.** When fewer than half the pixels are active, a pass gives each of the u active pixels k = min(⌊N_pix/u⌋, samples left until the next check) samples, so launches stay full (k·u ≤ N_pix keeps D5's buffer size) and no pass crosses a check.
  - With light tracing on, A and B include the light image split by pass parity (0008 #4).
  - Adaptive sampling is off in DLSS mode and in every variance or firefly measurement.

### E. Firefly and validity policy

- **D17. No biased estimator is applied by default.** No clamp, no Filter Glossy, no path regularisation (Kaplanyan and Dachsbacher 2013), no firefly reweighting (Zirr, Hanika, Dachsbacher 2018), no outlier rejection.
  - Allowed variance reduction is only unbiased: NEE, MIS, Russian roulette, light tree, environment importance, the light-tracing partition (D18) and adaptive stopping.
  - Biased controls exist only as explicit user settings that default to off in Astroray: direct and indirect clamp, applied per contribution on the maximum film channel with chroma preserved ("direct" = emission found at bounce index − 1 ≤ 0, per v1's Cycles finding); Filter Glossy, a roughness floor applied at exactly one point in the material stage (0005); the reflective and refractive caustics toggles, which also disable the light-tracing pass.
  - Every active biased control is listed in the per-render report and in output metadata. A render that asks for science outputs (0008) while a biased control is active fails with a visible error.
  - A non-finite sample is not written, is counted (`NUMERICAL`, 0003 #23) and reported. Any test tier fails on a non-zero count.

### F. Light tracing for gratings, prisms and caustics

- **D18. Method: light tracing with camera connections, restricted to caustic paths.** Particle tracing with direct computation of pixel values: Arvo 1986; Dutré, Lafortune, Willems 1993; Veach 1997 ch. 10; reference implementations pbrt-v4 `LightPathIntegrator` and Mitsuba 3 `ptracer.cpp`. Path space is partitioned as in Jensen's caustic photon map (Jensen 1996, 2001). Unbiased; handles medium scattering vertices (a beam in fog after a grating); no density-estimation grid.
- **D19. Exact partition rules.**
  - **One delta predicate.** `lobe_is_delta`, one function in 0005's lobe library: true for a smooth grating order, smooth dispersive transmission, and any microfacet lobe whose effective roughness after every remap (Filter Glossy included) satisfies pbrt-v4 `TrowbridgeReitzDistribution::EffectivelySmooth` (α < 1e-3, SOURCE; 0005's `MAY_BE_DELTA` uses the same threshold). Camera sampling, light-path sampling and the compiler all call it.
  - **Casters.** An instance is a caster when its material has `MAY_BE_DELTA` (0005, conservative) and its program reads no ray-type input (Light Path's Is Shadow Ray, Is Camera Ray and similar). Ray-type-dependent materials are not reciprocal, so no partition through them is exact: their delta interactions are path-traced only and the report lists them. This keeps Blender's "glass transparent to shadow rays" trick behaving as it does in Cycles, with no double counting from light paths. 0007 exposes a per-object opt-out (default automatic).
  - **Ownership.** Each vertex is labelled by the lobe that scattered there. The light pass owns exactly the labelled paths L δ+ X E in which the first δ vertex (from the light) is on a caster, X is a non-delta-labelled surface or medium vertex, and X is seen directly by the camera. The path tracer owns every other path.
  - **Light-path side.**
    - At v1 (the first vertex after emission) the light path must hit a caster and sample a delta lobe, otherwise it is discarded. It never connects at v1, because L X E is direct light, which the path tracer owns.
    - At every later vertex reached through delta samples only: if f there has a non-delta part (surface lobes or a medium phase function), connect to the camera with that part only. Then continue if that vertex samples a delta lobe, otherwise terminate. A mixed vertex (a roughness-0 coat over a diffuse base) therefore connects and may also continue, and the next non-delta vertex connects again.
    - Delta vertices after v1 may be on any object.
  - **Camera side.** The path is armed at v1 (its first vertex) when v1 sampled a non-delta lobe or phase. Each later delta sample sets `caster_bit = is_caster(object)`; a non-delta sample disarms, and the path never re-arms. Emission found while armed, after at least one delta vertex, with `caster_bit` set, is dropped (lamp crossing, emissive surface, environment or sun); the path continues. `caster_bit` holds the caster flag of the delta vertex nearest the light, which is the light path's v1. NEE is unaffected because casters block shadow rays (D4).
  - **Segment symmetry.** Every rule the camera path applies to a segment, the light path applies to the same segment:
    - lamp visibility: discard at v1 if the light is invisible to the ray type of the lobe sampled there (glossy for reflection, transmission for refraction);
    - object visibility: segments after a delta reflection or transmission trace with the glossy or transmission mask; no connection from an X that is invisible to camera rays; at X, the diffuse and glossy parts of f (the split 0008 already asks of 0005) are zeroed when the delta object on the next segment is invisible to that part's ray type;
    - holdout at X scales the connection by (1 − holdout weight); shadow-catcher receivers get no connections, and the report says so when casters exist;
    - light linking: a light path from light ℓ is discarded at v1 unless v1 is a receiver of ℓ, which is the vertex from which the camera path would reach ℓ; shadow linking: the segment ℓ → v1 is traced with ℓ's blocker set;
    - media: free flight uses the same transmittance estimator; a real scatter on a delta chain is a non-delta vertex (connect, terminate).
  - State: 3 path-flag bits, armed, chain non-empty and `caster_bit` (requirement on 0003 #22).
- **D20. Light-path emission.** A pass's light work is P light paths (0002 D7's work counter).
  - Light choice: an alias table over every light type (point, spot, sphere, area, sun, environment, emissive triangles) by flux toward the casters' bounding sphere, weighted with w(λ) and given the same ε radiometric floor as D10. Every light with non-zero power there gets a non-zero pmf; otherwise the camera-side cull would remove caustics with no replacement.
  - Direction: uniform in the cone subtending the casters' bounding sphere; for distant and environment lights, a position on the disc that covers the sphere (v1). This loses nothing because v1 must be a caster.
  - Wavelengths: 0003 #21, drawn from the chosen light's spectrum and stored with their pdf, so a UV lamp emits UV light paths.
  - Weights follow `sample_le`: beta₀ = Le·|cos| / (P·pmf·pdf_pos·pdf_dir).
  - P: a controller keeps the light pass at or under a share of pass time, 25% (PROVISIONAL; set by the equal-time sweep in Verification), clamped to [2^15, the main-pool capacity]. The floor is DERIVED as one full wave (70 SMs × 2 blocks × 256 threads = 35,840, spike A §1 and 0002 D9); the ceiling is 0002 D5's pool capacity. In final renders P equals the camera samples per pass (PROVISIONAL).
- **D21. Kernels and state.**
  - 0002's `generate_light` and `shade_light_path` (`ShadeKind::Light`), plus a light variant of each extension identity that 0005 defines; extension hits on light paths route there, so `shade_light_path` contains no extension code (spine §2; 0002 D8; 0005 #18's "two kernels with extension code" becomes `shade_surface_ext` and the light extension variant).
  - Light-path vertices call the same material, VM and lobe functions as camera paths in *importance* mode: no 1/η² radiance scaling, and the Veach 1997 ch. 5 shading-normal correction. No NEE.
  - Light paths are main-pool items with 0002's `path_kind` bit and 0003 #21's conditional λ and pdf fields. They share `intersect_closest`, `shade_light` (pass-through), the medium stage (0006 emits a connection item, not an NEE item, at a light-path scatter vertex) and the shadow stage, whose connection item carries a film position, the light-group id and λ, pdf in place of a sample slot.
- **D22. Wavelengths on light paths.** At a wavelength-dependent delta vertex (dispersion, grating order) the secondary lanes terminate (0003 #5, pbrt `TerminateSecondary`) and the hero direction is followed; heroes differ between light paths, so one pass already fills a continuous spectrum. A receiver is evaluated at the light path's own wavelengths, and splats convert to film channels with those wavelengths and their pdf, so nothing mismatches.
- **D23. Splat and normalisation.**
  - The connection computes beta·f·|cos|·We/pdf through the camera's `sample_wi` (pbrt-v4 `PerspectiveCamera::We`, `PDF_We`, `SampleWi`) with a lens sample and the light path's time.
  - **One pixel per splat.** For film position p, let f̂ be the pixel filter normalised to unit integral (the camera filter, 0008 #5), w_c = f̂(p − c) over the pixels c whose centres lie within the filter radius, and W = Σ w_c. The shadow stage picks one pixel c with probability w_c/W and deposits value·W there by fp32 atomic add into 0008's `SPLAT` plane. The expectation per pixel equals footprint splatting (pbrt-v4 `RGBFilm::AddSplat`), light-image pixels stay independent, and it costs one atomic instead of up to four. With Box 1 px (photon counts, 0008 #9) c is the containing pixel.
  - Light image value: the per-pass plane resolved by 0008 #4 (weights already include 1/P). Displayed result = path-traced mean + light-image mean. Light passes continue while any pixel is unconverged. The light image is not bit-reproducible; reproducibility tests exclude it.
- **D24. Coverage limits, loud.**
  - Perspective and thin-lens cameras only. Orthographic and panoramic cameras get a visible warning that delta-light caustics are path-traced only.
  - Two path families are path-traced only, so they are **missing for delta lights** (point, spot, zero-angle sun, collimated area): caustics seen through a delta chain (L δ+ D δ+ E: a pool floor through water, a caustic in a mirror) and caustic light that bounces again (L δ+ D D+ E). For other lights they are path-traced, unbiased but slow. The per-render report names both whenever casters and delta lights are present.
  - Direct light through smooth glass (windows) is carried only by the light pass: NEE is blocked (D4) and BSDF hits on the light are culled (D19). Window-lit interiors therefore depend on the light-pass share (D20) and are a benchmark scene (Verification). Their second bounce is the path-traced L δ+ D D E family above.

### G. Progressive accumulation and reset

- **D25. Passes and reset rules.**
  - A pass is one sample for every active pixel (k samples in the adaptive tail, D16) plus one light pass of P light paths if casters exist. The sample index per pixel is monotonic within an epoch, so the Sobol prefix holds.
  - Edits are classed by 0007: image-affecting edits (camera, transform, geometry, material, light, world, integrator settings, resolution) bump the epoch: film, light image, adaptive state and statistics are cleared and sample indices restart at 0; the light structures are rebuilt only if lights, emissive materials or the world changed. Display-only edits (film exposure, view transform, denoiser choice, diagnostic view) do not reset. Selection changes are ignored.
  - Writes carry the epoch and a stale epoch is discarded. Cancellation latency is one step (0002 D7).
  - Targets: reset to the start of the new pass ≤ 2 ms, excluding sync and rebuilds (PROVISIONAL; about 20% of the roughly 10 ms per frame left to the engine at 60/s after Blender's redraw, spike B via 0008). Light-structure build counts toward first frame: ≤ 50 ms per 10k lights (PROVISIONAL; about 4% of Cycles' 1,280 ms pre-first-sample on spike B S2, where our sync already takes 1,420 ms without lights or shaders); 1M emissive triangles to be measured.
  - DLSS mode (0008): the colour film resets every frame (statistics do not, 0008 #12); 1 spp per frame, one light pass per frame, adaptive off. The viewport sample limit and final renders use the same loop.

### H. Later features and film inputs

- **D26. No hooks for later reuse or guiding.** ReSTIR-class reuse (Bitterli et al. 2020; Lin et al. 2022) and path guiding (Müller, Gross, Novák 2017) are after-first-release candidates. No state, dimension, runtime flag or branch is reserved for them (CLAUDE.md rule 2). What they would need and already exists for other reasons: light samples deterministic in (ctx, u2) (D7), the D10 importance as a target function, `pdf` on every lobe, and 0008's guide buffers.
- **D27. Film inputs owed to 0008.**
  - Light groups: every contribution carries the u8 group id of its emitter (lamp, emissive mesh, world): shadow items the id of the sampled light, emission hits and lamp crossings the id of the emitter hit, splats the id of the emitting light. Enabled group planes receive it beside the sample slot.
  - Light-path passes: when enabled, each contribution carries its category, fixed at the first scattering vertex and split direct/indirect by bounce count (0008 #10). The per-category weights are 0008's feature-tagged state members.
  - Denoising surface: the path sets a `DENOISE_SURFACE_DONE` flag (a 0003 #22 bit) at its first non-delta-lobe sample; the surface stage writes 0008 #15's guides there.

### I. Statistics and diagnostics

- **D28. Per-sample statistics.** The path-end stage reports to 0008 #12's channels: bounce count (0008's definition), termination reason and, when the time map is on, per-path stage cycles (0003's conditional `stats_time`). Termination reasons are 0003 #23's enum plus `ZERO_THROUGHPUT` (a sample with f = 0 or pdf = 0), requested from 0003; per-type limits map to `MAX_BOUNCES`. Storage, defaults and widths are 0008's: u32 counters, written with u32 atomic adds because tail passes put k paths on one pixel. Nothing saturates.
- **D29. Global counters per pass (u64):** items per stage and per bounce; active paths; light paths emitted, connected and discarded at v1 (by reason: not a caster, non-delta at v1, visibility, linking); culled camera contributions; lamp crossings; shadow rays and transparent-layer re-queues; per-kernel GPU time (events). They feed the benchmark HTML report and the per-render report. The heat-map display belongs to 0007 and 0008.

## Alternatives considered

| Light-tracing candidate | Cost on 0002's wavefront | Wavelength-dependent delta | Covers | Verdict |
|---|---|---|---|---|
| **Light tracing + camera connection (chosen)** | 2 kernel identities plus light extension variants; light paths share the main pool; reuses intersect, medium and shadow stages | Per-path wavelengths; secondaries terminated; continuous spectrum per pass | L δ+ X E incl. media; unbiased | Smallest design that covers gratings, prisms and caustics as normally viewed |
| SPPM / probabilistic PPM (Hachisuka and Jensen 2009; Knaus and Zwicker 2011) | +3 stages (hash and sort, gather, visible points) plus grid and radius schedule | One wavelength set per round (pbrt-v4 SPPM pattern, to confirm) or a wavelength kernel (smears laser lines) | Also L δ+ D δ* E; biased but consistent; blurs laser spots | Deferred: the candidate for the D24 gaps, reusing D21's light paths |
| BDPT (Lafortune and Willems 1993; Veach and Guibas 1994) | Both subpaths stored per depth; O(s·t) shadow rays; full-path MIS pdf walk; pbrt-v4 kept it out of its wavefront integrator | Cannot connect at delta vertices; spectral MIS breaks after termination | Still cannot do L δ D δ E with delta lights | Rejected: cost and register load for little gain |
| VCM / UPS (Georgiev et al. 2012; Hachisuka, Pantaleoni, Jensen 2012) | BDPT + photon map | As both above | Most robust | Rejected for 2.0 on complexity |
| MNEE / SMS / specular polynomials (Hanika et al. 2015; Zeltner et al. 2020; Fan et al. 2024) | +1 stage; per-chain Newton state, 3-8 iterations | Constraint per wavelength (unpublished for SMS); grating constraint unpublished | Camera-side L S D E, curved refractors | Rejected now: v1 failed on flat prisms; possible later for the seen-through gap |
| Path guiding (Müller 2017; Vorba 2014) | Training buffers; OpenPGL CPU-only | Cannot sample delta | Area-light caustics only | Rejected as the caustic mechanism |
| Path tracing only | none | n/a | No delta-light caustics | Fails owner decision 17 |

- **Accumulation.** Per-contribution atomics straight into film moments give no per-sample second moment (0008). A per-path accumulator in the path state (the first draft, and 0003/0008 as written) loses or misroutes shadow results when a path ends or its slot is refilled first; a `shadow_pending` gate would keep waiting paths in the pool and still race when transparent shadows keep two items of one path in flight. The per-sample buffer (D5) has neither problem.
- **Power heuristic vs the pbrt volpath balance form.** The power heuristic can be slightly lower in variance, but a second form beside the r_u/r_l form that chromatic media need duplicates code. One formulation wins.
- **Capped lamp hits per segment** (v1's 4): silently drops crossings and needs repeated traversals without any-hit programs. D8's one crossing per step has no cap.
- **Importance over the film responses only** starves UV and other non-visible emitters whose light reaches the film through fluorescence; with no floor, such a delta light is never sampled.
- **Footprint splatting** (pbrt `AddSplat`): same expectation as D23, but it correlates neighbouring light-image pixels and costs up to four atomics per splat.
- **Separate environment strategy at a fixed ratio:** v1 measured higher variance with lamp plus HDRI, and a tree that carried only some light types starved the sun. Every light type goes into one selection.
- **Light tree in the first milestone:** deferred to the next milestone. The alias table is correct and simple; v1's largest noise source was a *faulty* tree, so the tree lands with its seam tests.
- **Blue-noise sample ordering** (Cycles `kernel/sample/pattern.h`, after Ahmed and Wonka 2020: Morton-ordered Owen scrambling). Some of its modes need the sample count up front, the reason ZSobol was rejected, and its benefit under OIDN in the viewport is unmeasured. The sampler is 0003's; adopting it would be a 0003 change backed by research 04 S6.
- **Adaptive metrics:** luminance-only (v1 blue floor) and Cycles' summed form (colour-blind by construction) are both rejected.
- **Default clamping (Cycles' practice):** it caps real focused energy (v1 capped a caustic focus) and contradicts "physically correct always".
- **Reserved hooks for ReSTIR and guiding:** rejected under CLAUDE.md rule 2 (D26).

## Consequences

- **Easy:** new light types, since each implements one interface and the tree, NEE, MIS and light-path emission follow; new extension lobes appear in light paths through their light variant; light-pass correctness is checked against path tracing with area lights by the partition tests; reset and resume bugs show as bit differences on the single-threaded CPU.
- **Hard:** the D24 gaps for delta lights; scenes with very large casters waste light paths (most v1 hits sample a non-delta lobe), so the per-object opt-out matters; window-lit interiors lean on the light pass; the light image is non-deterministic and outside per-pixel sampling control; every material must declare `MAY_BE_DELTA` correctly, or caustics vanish.
- **Cost when unused:** no casters: no light-path launches, the cull bits never set, one check per pass; no lamps crossed: no `shade_light` launches; one light set: no extra trees; no fluorescent materials: R̂_exc absent; adaptive off: no class-A sum; ReSTIR and guiding: zero; the sample buffer is always allocated (41 MB at 1080p).

## What lands when

| Milestone | Contents |
|---|---|
| First milestone | D1-D5 path tracing with NEE+MIS, Russian roulette and the sample buffer; point/sphere, spot, area (rectangle, disk), sun, constant world and HDRI with compensated importance, emissive meshes (D6-D9, D11); lamp primitives and `shade_light` (D8); alias selection with the floor (D10); sampler use (D13-D15); NaN policy (D17); passes and epoch reset (D25); statistics inputs (D28-D29). Light-pass interfaces, built and unit-tested without the pass: `sample_le` for every M1 light type, camera `We`/`PDF_We`/`sample_wi`, the 3 partition bits, the connection item, and 0002's stub `generate_light` scheduler test |
| Before first release | The light pass D18-D24 with glass, mirror, coat and grating casters, every light type and medium connections; light tree with segment importance and light and shadow linking (D10); sky with split sun disc (D12); IES, spot blend, area spread and ellipse; per-type bounce limits; extension lobes in camera and light paths; colour-aware adaptive sampling with the tail rule and light-image A/B (D16); explicit biased controls and reporting (D17); per-object caster opt-out; film inputs (D27) |
| After first release | A fix for the D24 gaps, chosen by measurement between a probabilistic PPM gather on D21's light paths and MNEE; Preetham and Hosek-Wilkie skies; ReSTIR DI, GI, PT and GPU path guiding, each only with a measured gain; Wilkie hero MIS for rough dispersive lobes (0003) |

## Verification

| Decision | Check (tier per 0009) | Pass criterion |
|---|---|---|
| D1 MIS | NEE on/off and BSDF-only renders per light type, lit-furnace scene with a small emitter | Means equal within 3 SE; max_depth = 1 matches L_r = ρ·L_e·(R/d)² within 0.5% |
| D1/D9 | Emitter with pmf 0 (zero estimate) lit through BSDF hits | Mean equal to the pmf > 0 render within 3 SE |
| D2 | Closed furnace, albedo 0.8, Russian roulette on/off | L = L_e/(1−ρ) within 3 SE both ways |
| D3/D14 | 10 stacked alpha cards (α = 0.5) between the camera and an emitter; N transparent sheets under a sun at several transparent limits | Transmittance 0.5^10 within 3 SE; lit or black per limit as v1 measured in Blender 5.2 |
| D4/D5 lifetime | Pool forced to 2^10, 8 non-opaque shadow layers, Russian roulette at every bounce, so paths end and slots refill with shadow items in flight | Mean equals the analytic value within 3 SE; equals the large-pool run within 1e-6 relative; CPU threads=1 bit-exact |
| D6 | Analytic irradiance (albedo·E/π) for point, sphere, spot, area, sun, collimated area; sun at 0.545° vs 2.2° | Within 0.5%; size invariance within 0.5% |
| D7/D11 | `pdf_li(sample.wi) == sample.pdf`, `pmf(ctx, sampled) == pmf` incl. the floor (T0); environment chi-squared vs table pdf; constant world 0.5 | 1e-4 relative; chi-squared p > 0.01 (Šidák); 0.5 within 3 SE |
| D8 | Two stacked area lamps facing the camera, NEE on/off; 64-lamp scene | Means within 3 SE; crossings per ray and steps per pass reported against 0002's budgets |
| D10 tree | Tree vs alias-table means per tile (Welch, Jung et al. 2020); 64 area lights at 256 spp; importance at points inside cluster bounds | No tile rejected after Holm; ≥ 2x variance reduction (v1 gate); finite everywhere |
| D10 UV | 365 nm point lamp on a brightener plane, display film only (0003 V19) | Re-emission = quantum yield × absorbed within 3 SE; the rejected rule (film-only weighting, no floor) renders black, so the test discriminates |
| D12 | Sky table vs direct evaluation of the ported routine at random (direction, λ); disc irradiance with the limb profile, NEE on/off; Sky × 0.5 split vs forced table sampling; Sky × direction-dependent ramp | 1e-3 relative; 0.5%; means within 3 SE; visible warning |
| D16 | Blue-dominant chromatic scene, per-channel vs summed metric; e on synthetic sums vs Cycles' formula | Residual Z relative error at stop ≤ 1.5 × threshold and the summed metric shows the floor; adaptive vs fixed-spp means pass Welch; e equals Cycles' to 1e-6 |
| D16 tail | 1080p scene with 0.5% unconverged pixels, tail rule vs k = 1 | Launch sizes and time to threshold reported (measurement); means equal (Welch) |
| D17 | Injected NaN contribution; clamp set with a science output requested | Film unchanged and counter = 1; visible error |
| D19 partition | Area-light scenes, path tracing alone (light pass and cull off) vs path tracing + light pass: glass sphere over a floor; roughness-0 coat over diffuse receiving a caustic; mirror → opted-out glass → floor and opted-out glass → mirror → floor; glass sphere into fog (medium vertex); light-linked caster; receiver invisible to camera; holdout receiver; glass with a ray-type trick | No tile rejected after Holm (Welch); deliberately broken rules (no reconnection after a mixed vertex; `caster_bit` from the wrong end) fail |
| D20-D23 | Point lamp through a ball lens vs numpy flux (v1: 1.7246 vs 1.7409); slab with an area light, light pass vs path tracing (catches the η² error); prism deviation vs Sellmeier; sodium line through BK7; laser through grating | Within 1%; equal within 3 SE; within 5%; narrow band; spot centroids at grating-equation angles for m = ±1, ±2 within 0.1°, efficiency sum ≤ 1 |
| Prism hue | SF11 prism under a collimated area light vs a numpy two-face Sellmeier + Fresnel oracle of floor spectral irradiance per bin; P = 2^16 to 2^22 | Per-bin xy within 0.01 at the largest P; chroma RMS error falls with P at slope −0.5 ± 0.1; image inspected (v1's metric passed on noise) |
| D23 | Chosen splat pixels vs normalised filter weights; Box 1 px | χ² p > 0.01; always the containing pixel |
| D24 window | Interior lit by a sun through a glazed window; light-pass share 10/25/50% at equal time | relMSE against a high-sample reference reported per share; sets the D20 share |
| D21 budgets | `generate_light`, `shade_light_path`, light extension variants, `shade_light` | 0002 D10 limits |
| D25 | Render n passes, edit and revert, compare with a fresh render of the same seed | CPU threads=1 bit-identical; GPU within 1e-6 relative; reset ≤ 2 ms |
| D28 | Σ sample counts = launched camera samples; Σ termination counts = sample count; closed box, Russian roulette off: bounces = n·max_depth | Exact |
| Viewport | 1080p reference scenes on the 5070 Ti | Light-pass share as set; statistics overhead < 2% |

Every rendered check is also looked at as an image (CLAUDE.md rule 4).

## Risks and open items

- **Conflicts with other records (lead to settle):**
  - 0003 #20 keeps `L` in the path state, #21 gives shadow items a path index, and 0008 #3 writes the sample at path end from `L_film`; D5 replaces both with the per-sample buffer, because the path-state form has the lifetime hazard.
  - 0006 #11 weights volume NEE with the power heuristic; D1 requires the balance form for every vertex, and 0006's equiangular/distance MIS must be expressed in the same r_u/r_l terms.
  - 0008 #4 describes footprint splatting; D23 deposits one pixel per splat with the same expectation.
  - 0002 must add the lamp primitive types (custom intersection for disks and ellipses) and the `shade_light` kernel (D8).
  - 0003 must define b as the event index (D14), add `ZERO_THROUGHPUT` (D28) and bits for D19 and D27.
  - 0005 must supply `lobe_is_delta`, a "reads a ray-type input" flag, the `SKY` linearity query with its gain, excitation spectra for R̂_exc, and rename its light-path extension kernel (#18).
  - 0007's table puts emissive meshes and HDRI importance before the first release; D6-D11 build them in M1 because 0002 front-loads environment sampling and emission MIS into `shade_surface`. The roadmap picks one.
- **D24 gaps for delta lights** (seen-through and indirect caustics): the most visible gap in pools and mirrors. Probabilistic PPM needs a wavelength-matching rule; MNEE needs per-wavelength constraints. Measure both after the first release. Whether the first release may ship with the gap is an owner question.
- **Window-lit interiors:** direct sun through glass is light-pass only; if no fixed share is within 2x of the best share on both the caustic scenes and the window scene, a share driven by the light image's fraction of measured variance is the candidate.
- **Light-path efficiency:** one cone toward the union of casters wastes light paths when casters are spread out or large. Measure light paths connected per light path emitted on the canonical scenes; per-caster-cluster cones are the remedy.
- **Early viewport look:** unbiased splats sparkle at first. Check by eye with OIDN at 1-16 passes.
- **Texture filtering on light paths:** with no ray cone, the receiver texture is filtered differently from path tracing. Use the camera-projected footprint at the connection vertex; measure the difference on a textured receiver.
- **Light-tree build time** for 1M emissive triangles against the first-frame target; GPU or LBVH-style builds if over budget.
- **Importance weighting** with sun plus mesh emitters, infrared sources and fluorescent scenes (research 04 S8): measure selection rates and tune ε.
- **Sample-buffer and resolve cost:** 166 MB at 4K, one extra kernel per pass, fp32 atomics per contribution. Measure in M1 with 0008's fp64 cost.
- **Lamp crossings:** many large visible lamps add steps; measure on the 64-lamp scene against 0002's step budget.
- **Sky sources:** whether the Multiple Scattering source is spectral, and its table size, are confirmed when it is read.
- **Spectral MIS after a wavelength rewrite** (fluorescence, redshift), on camera paths and on forward light paths, is 0003's and must exist before fluorescence lands. The grating efficiency model still lacks a citable source (0005); the grating test waits on it.

**Assumptions about other records.**
- 0001: milestones as in the spine's release structure; a CLI host runs the tests.
- 0002: pools with `path_kind`, self-contained shadow items, drain-per-pass scheduling and its work counter; the shadow any-hit; `generate_light`, `shade_light_path` and light extension identities; the lamp primitives and `shade_light` (requested); per-kernel gates.
- 0003: SoA state with beta, r_u, r_l, transport wavelengths and flag bits; no `L` (requested); the sampler, event index and pcg4d keys; light-path λ/pdf fields; `film_weights`, `TerminateSecondary` and the termination enum.
- 0005: VM and OpenPBR record; `lobe_is_delta` and `MAY_BE_DELTA`; importance-mode evaluation; diffuse/glossy split of f; emission-only programs for lamps and world; emission estimates per triangle; excitation spectra; Filter Glossy at one site; the `SKY` op and linearity query.
- 0006: medium stage with a light-path connection item, ratio-tracked shadow transmittance, segment light contexts, the balance form for volume MIS.
- 0007: unit scale for Blender lights and sky inputs; visibility flags; light sets and linking; edit classification; settings mapping; caster opt-out UI; heat-map display.
- 0008: `film_write_sample` called from the D5 resolve; class-A sums; `SPLAT` per-pass plane; light-group and light-path planes; guide buffers; statistics storage; DLSS mode.
- 0009: tiers, Welch and chi-squared harnesses, image inspection.

## Sources

- **Research and evidence:** `docs/research/04-integrator-architecture.md` (R1-R12, S1-S11); `17-materials-and-custom-types.md`; `14-reference-implementations.md` §2.2-2.3; `05-spectral-and-materials.md` A1; `07-blender-feature-surface.md` F5-F6; `09-test-validation.md`; `10-denoise-upscale.md`; `docs/v1-lessons/research/transport.md`, `lights.md`, `volumes.md`, `spectral.md`, `misc.md`; `docs/v1-lessons/physics-and-rendering.md`, `blender-integration.md`, `testing-and-process.md`; `spikes/A-kernel-budget/RESULTS.md` (148/128 registers, 70 SMs); `spikes/B-blender-end-to-end/RESULTS.md` (first frame 1,420 ms vs Cycles 1,280 ms; Blender's ~6 ms redraw).
- **Transport papers:** Veach and Guibas 1995 (MIS); Veach 1997 thesis (ch. 5, ch. 9, ch. 10); Arvo 1986; Arvo and Kirk 1990; Dutré, Lafortune, Willems 1993 (background); Heckbert 1990; Jensen 1996 and 2001; Hachisuka, Ogaki, Jensen 2008; Hachisuka and Jensen 2009; Knaus and Zwicker 2011; Lafortune and Willems 1993, Veach and Guibas 1994 (BDPT); Georgiev et al. 2012 and Hachisuka, Pantaleoni, Jensen 2012 (background).
- **Specular-chain papers:** Hanika, Droske, Manakov 2015 (MNEE); Zeltner, Georgiev, Jakob 2020 (SMS); Fan et al. 2024.
- **Light and sky papers:** Conty Estevez and Kulla 2018; Karlík et al. 2019; Clarberg 2008 (background); Arvo 1995; Ureña, Fajardo, King 2013; Nishita, Sirai, Tadamura, Nakamae 1993 (single-scattering sky); Hesterberg 1995 (defensive mixtures).
- **Sampling, adaptive, firefly and later-feature papers:** Burley 2020; Ahmed and Wonka 2020 (blue-noise ordering, background); Wilkie et al. 2014; Dammertz et al. 2010; Zirr, Hanika, Dachsbacher 2018; Kaplanyan and Dachsbacher 2013 (background); Bitterli et al. 2020; Lin et al. 2022; Müller, Gross, Novák 2017; Vorba et al. 2014; Jung, Hanika, Dachsbacher 2020 (Welch test).
- **pbrt-v4 (Apache-2.0):** `cpu/integrators.cpp` (`VolPathIntegrator::Li`, `SampleLd`, `LightPathIntegrator`, `SPPMIntegrator`); `lights.h`/`lights.cpp` (`SampleLe`, `ImageInfiniteLight`); `lightsamplers.h`; `util/sampling.h` (`AliasTable`, `PiecewiseConstant2D`, `SampleDiscrete`, `SampleSphericalTriangle`, `SampleSphericalRectangle`); `shapes.h`; `cameras.h` (`We`, `PDF_We`, `SampleWi`); `film.cpp` (`AddSplat`); `util/scattering.h` (`TrowbridgeReitzDistribution::EffectivelySmooth`); `util/spectrum.h` (`TerminateSecondary`). Pin a commit when reading.
- **Cycles (Apache-2.0 unless noted; per-file licence check):** `kernel/light/tree.h`, `scene/light_tree.cpp`, `kernel/light/area.h`, `kernel/light/background.h`, `kernel/integrator/shade_light.h`, `shade_surface.h`, `intersect_closest.h`, `kernel/sample/mis.h` (BSD-3), `kernel/sample/pattern.h`, `kernel/film/adaptive_sampling.h`, `kernel/svm/sky.h`, `intern/sky/source/sky_single_scattering.cpp`, `sky_multiple_scattering.cpp` (MIT); not `sky_nishita.h`, `sky_math.h` (GPL).
- **Other references:** Mitsuba 3 (BSD-3): `src/integrators/ptracer.cpp`, `path.cpp`. Falcor `Source/Modules/ReSTIRPathTracing` (BSD-3); OpenPGL (Apache-2.0, CPU-only v0.8).

## Review and resolution

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
