# Volumes: null-scattering and tracking, NanoVDB majorants, volume emission, world volumes, clouds, mesh-volume boundaries

Carried over from Astroray v1 research, written without reference to v1 code. "Cycles" is Blender's renderer, cited as a source of Blender semantics and reference algorithms, not a pixel target. Fixtures and numbers are v1 measurements (CPU unless stated); they show orders of magnitude and what mattered, not targets for 2.0. The source digests are thin on GPU variance and on anything not listed below; gaps are marked.

## Established findings

### World (infinite) volumes
- v1 once had a world-fog setting that rendered as vacuum on both backends: the anisotropy parameter was stored but read by nothing, and the transmittance helper had no callers. A refactor had deleted the integrator that consumed it. Lesson: a volume parameter with no consumer must fail loudly, and a test must render fog and see it.
- Absorption-only is a sound first stage. Colour is upsampled to a spectrum as a reflectance, then Beer-Lambert is applied per wavelength. Never upsample the already-attenuated product (the spectral upsample is nonlinear in magnitude).
- Absorption needs transmittance in three distinct roles: throughput on surface hit, shadow-ray Tr over the shadow distance, and Tr to a lamp hit when that lamp is MIS-weighted. The final camera-to-environment segment of infinite length is not attenuated.
- Scattering adds a single-scattering albedo alpha: sigma_s = alpha*sigma_t, sigma_a = (1-alpha)*sigma_t. With alpha = 0 the scatter path is off, which kept the absorption stage bit-identical and made staged landing safe.
- Homogeneous free flight follows the pbrt-v3 `HomogeneousMedium::Sample` pattern: pick a colour channel uniformly with `min(int(u*n), n-1)`, sample distance from that channel's sigma_t, pdf = average over channels. Henyey-Greenstein is sampled exactly (pbrt-v3 `HenyeyGreenstein::Sample_p`), so pdf equals phase value and the continuation weight is 1.
- After a scatter event the scatter point must be taken from the ray state before the continuation overwrites it; getting this wrong silently moves the scatter point.
- Direct and indirect volume contributions were kept as separate passes so the sum of passes equals the beauty in linear sRGB.

### Parameter mapping from Blender's Principled Volume (Blender semantics)
- Cycles default absorption is zero (lossless scatterer). An early mapping sigma_a = (1-colour)*D was wrong. Established mapping: sigma_s = colour*D; sigma_a = max(1-colour,0) * max(1-sqrt(absorption_color),0) * D. The Volume Absorption node maps to colour = 0 with absorption_color = Color^2.
- A grey collapse (extinction = D * max over channels of sigma_t; albedo_c = sigma_s,c / that max) was the first transport's approximation. It over-extincts sub-max channels and was to be replaced by per-wavelength extinction.
- Blender's volume emission does not scale with density. This matters for the majorant (see Tracking). For grids, constant emission was gated to voxels with density > 0 as an approximation of Cycles' mesh-bounds behaviour.

### Heterogeneous media, NanoVDB, majorants
- Transfer contract that worked: the Blender side sends a dense float block over the active bounding box in `[nz][ny][nx]` order, the bbox minimum ijk, the index-to-object affine and the object-to-world matrix. Dense is 512 MB at 512^3: fine for a CPU oracle, not as a long-term format. Temperature, colour and velocity grids are passthrough.
- The host-side NanoVDB build path needs no OpenVDB (macro `NANOVDB_USE_OPENVDB` left undefined). NanoVDB.h is Apache-2.0 and can be vendored; OpenVDB core is MPL-2.0 and drags in Boost/TBB/Blosc, so it was avoided.
- Majorant grid: a coarse supervoxel grid holding the maximum density over a fixed 16^3 fine voxels (resolution capped at 128), sigma_bar = maxDensity * coefficient, traversed by a 3D DDA. Pattern from pbrt-v4 section 11.4.2 (`MajorantGrid`, `DDAMajorantIterator`), written clean-room. Supervoxel edge 4, 8 or 16 made no measurable difference to variance.
- `.vdb` reached v1 through the Blender API; a standalone `.nvdb` reader was left as an open alternative.
- Never seed volume sampling from a hardware random device (a legacy medium did, making renders non-reproducible).

### Tracking and null scattering
- Estimators chosen: delta (Woodcock) tracking on the DDA majorant for free flight; ratio tracking for shadow-ray transmittance (Novak, Selle, Jarosz 2014); Henyey-Greenstein phase (Henyey and Greenstein 1941); equiangular distance sampling (Kulla and Fajardo 2012) for light along a segment. Homogeneous hulls keep the analytic truncated exponential, no tracking.
- For wavelength-dependent media the decision was pbrt-v4 hero-wavelength spectral MIS (the `r_u` rescaled-path-probability update in `VolPathIntegrator::Li`), not decomposition tracking. Reason: with a wavelength-independent majorant sigma_bar (valid because sigma_s + sigma_a <= sigma_bar), the majorant transmittance is identical across wavelength lanes, so the loop reduces to ratios of sigma values. Basis: Kutz et al. 2017, Miller et al. 2019, Wilkie et al. 2014.
- Emission in the loop: add beta * L_e / sigma_bar at each tentative collision. Because emission does not scale with density, an emission majorant floor of 8 / diag(AABB) ensures collisions occur in thin emissive media.
- Hero-lane channel pdf was the plain average over lanes, not Cycles' albedo-times-throughput weighting (a deliberate divergence). Limits: first heterogeneous design capped a flight at 1024 steps; later limits were 1,048,576 tentative collisions per flight and 131,072 per shadow segment per medium.
- RNG keying: `base + bounce*4096 + draw` aliased past 4096 draws per bounce; replaced with disjoint bit fields.

### Segment direct lighting
- Combine equiangular sampling and distance sampling (1/2 each, as Cycles does), a null-collision chain at the local majorant, a reservoir over scatter candidates, and power-heuristic MIS. This replaced a distance strategy using a global majorant that had 79x per-sample variance against Cycles on a smoke plume (21.6x on a later engine). v1 runs the reservoir at every bounce, whereas Cycles does so on the primary segment only (lead decision, deliberate divergence).
- Both parts are required: dropping segment direct light gives a 0.5x ROI mean (biased dark), distance-only explodes to 3.45e4x variance.
- In a geometry-bounded medium, a lamp hit must be weighted by Tr(lamp), not by path-survival probability. The wrong weighting gave a 17-20 % dark image with NEE off and was only caught by an unbiasedness test against a no-NEE render. Fix: evaluate the lamp pass before the bounded flight with explicit Tr.
- Light-tree picking for a segment needs a segment importance, not a point at the segment midpoint (which fails outside spot cones and leaves no equiangular anchor; a shaft rendered at 0.67-0.71 of the reference mean). Importance follows Conty Estevez and Kulla 2018 section 5.3 Eq. 4: energy * (theta_b - theta_a)/d. v1 used node-form emitters (bbox + cone), max-only bounds on inner nodes, and no triangle segment clip.
- Rectangle lights: a spherical-rectangle pdf and draw (Urena et al. 2013) for rectangles subtending at least 180 degrees reduced per-sample variance and brought means within |z| <= 1.3.
- Caveat: with a light tree, the light used as equiangular anchor and the light actually connected can differ. Still unbiased, less efficient.

### Multiple media on one segment
- Tracking only the first-entered medium skipped, e.g., fire behind smoke (shadow rays, which multiply all media, were already right).
- Working approach: sweep the segment by medium AABB boundaries. A piece with one medium uses the single-medium tracker (byte-identical for single-medium scenes); overlapping media use the summed majorant sigma_bar = sum of per-medium majorants (Novak et al. 2018 sections 3-4) and pick the interacting medium by its coefficient. The exponential restarts at piece boundaries (memoryless). Cap of 8 media per piece. Cycles' volume stack sums closures.

### Mesh-volume boundaries
- Semantics that matched Blender: a point is inside medium k iff the nearest boundary crossing along the ray after t is back-facing. For closed, consistently oriented meshes this equals the Cycles stack, detects camera-inside without an initialisation pass, and deduplicates on object plus shader. Cycles instead shoots a probe ray along +Z in an init pass and enters/exits stack entries at boundary hits.
- Boundary intersection used a watertight triangle test (Woop, Benthin, Wald 2013, as ported in pbrt-v4 `shapes.cpp`) over a binned-SAH BVH.
- Camera rays must start at the clipped entry of the medium; tracking from the wrong t was a 3 % camera-inside brightness deficit.
- Semantic divergences shared with Cycles: open meshes count a hole entry as inside; inconsistent winding inverts inside/outside; a glass shell carrying both surface and volume reports "inside" after refraction. Validation ROIs kept away from holes.
- GPU design noted (not built when digested): query the boundary by repeated closest-hit calls instead of a cached list (the CPU list was 16 (float,bool) entries); expected +20-30 registers in the grid-volume shading path.
- Earlier pre-existing findings reported but not gated: a red deficit at volume_bounces 4 (control 0.955) and a +7.5 % smooth-glass rim without volume.
- CPU/GPU bit-identity: only ulp-level differences (<= 2.1e-7) from compiler inlining/FMA, not logic.

### Volume emission and blackbody
- Cycles Principled Volume (Blender semantics): emission independent of density; blackbody intensity = sigma_SB * 1e-6 / pi * mix(1, T^4, intensity); colour from a Rec.709 polynomial, luminance-normalised, T below 800 K clamped red. Blackbody Intensity is not clamped; a fire showcase uses 3.0, so a [0,1] clamp in v1 made fire 3x dim.
- v1 used Planck spectra instead: no 800 K clamp, CIE 1964 10-degree observer where Cycles uses a CIE 1931 fit. Luminance per unit length matched Cycles exactly.
- A float-typed normaliser gave NaN at T = 25, 30, 50, 80, 100 K and 0 at T <= 20 K. A per-kelvin memo at the rounded temperature mis-normalised non-integer T (luminance/Cycles 0.095 at 60.5 K, 1.035 at 500.4 K; a 7 % step across 500.49 -> 500.51 K). Fix that held: one log-domain lookup table (2048 entries, T in [30 K, 1e6 K], linear interpolation) shared by every target, within 0.05 % of Cycles intensity at 60.5-6543.2 K.
- A uniform-blackbody furnace test checks emission normalisation.

### Path termination for volume bounces
- Cycles semantics adopted: volume_bounces counts scatter events and uses terminate-after (internally max_volume_bounce + 1). The alternative (drop the continuation at the limit, as for surface bounces) was rejected: with volume_bounces = 0, smoke lit only by embedded fire rendered black.
- Recorded divergences: Cycles' terminate-after continues through transparent surfaces while v1 ended at the first non-emissive surface; Cycles' total max_bounces also triggers terminate-after.

### Clouds (research for a spectral cloud material; never implemented)
- sigma_ext / LWC = 3 / (2 * rho_w * r_eff) is near-exact (Q_ext about 2, large-particle limit x >~ 30). It fails for r_eff <~ 2 um, in the thermal IR and for absorbing particles.
- Liquid droplets are essentially conservative in 360-830 nm (1 - omega_0 <= 1e-5 at r_eff 10 um), so the visible-band spectral payoff is in phase function and illumination, not extinction.
- Ice tiers: roughened bulk ice (Baum 2014 general habit mixture), a smooth-habit pristine preset, oriented crystals (out of scope). Mixed phase: per-voxel blend weighted by extinction.
- Henyey-Greenstein and double-HG cannot give fogbow, glory or corona. Blender's Principled Volume stays HG-only; its Volume Scatter node has a Mie phase (Blender PR #123532; kernel mapping not re-read), the right cross-check.
- Acceptance checks proposed: BHMIE/miepython agreement, fogbow at 138-140 degrees for r_eff 10 um at 550 nm, glory at 180 degrees, integral of p over solid angle = 1, comparison with Cycles Volume Scatter + Mie.
- Design from that research: tabulated bulk phase per (r_eff, v_eff, lambda), 1024-2048 angle bins, inverse-CDF sampling, hero-wavelength MIS; optional HG switch after scatter order k (off by default); water table cached to a file.

## Sources and reference implementations

| Source | Authors, year | Recorded use |
|---|---|---|
| Spectral and decomposition tracking, ACM TOG 36(4) art. 111, DOI 10.1145/3072959.3073665 | Kutz, Habel, Li, Novak, 2017 | Spectral tracking basis (cloud notes also cite Kutz et al. 2017 ACM TOG, as recorded) |
| Hero wavelength spectral sampling, DOI 10.1111/cgf.12419 | Wilkie et al., 2014 | Hero-lambda MIS |
| Null-scattering path integral, ACM TOG 38(4), DOI 10.1145/3306346.3323025 | Miller, Georgiev, Jarosz, 2019 | Null scattering, spectral MIS |
| Ratio tracking for NEE, SIGGRAPH Asia | Novak, Selle, Jarosz, 2014 | Shadow-ray transmittance |
| Monte Carlo methods for volumetric light transport, CGF 37(2) | Novak et al., 2018 | Combined majorant, sections 3-4 |
| Equiangular / segment direct light, EGSR, CGF 31(4), DOI 10.1111/j.1467-8659.2012.03145.x | Kulla and Fajardo, 2012 | Segment direct light |
| Light tree, Proc. ACM CGIT 1(2), DOI 10.1145/3233305, section 5.3 Eq. 4 | Conty Estevez and Kulla, 2018 | Segment importance |
| NanoVDB, SIGGRAPH Talks | Museth, 2021 | Sparse grid |
| Watertight ray/triangle test, JCGT 2(1) | Woop, Benthin, Wald, 2013 | Mesh boundary (ported from pbrt-v4 `shapes.cpp`) |
| Spherical rectangle sampling | Urena et al., 2013 | Rectangle light segment draw |
| Henyey-Greenstein phase | Henyey and Greenstein, 1941 | Phase function |
| Lorenz 1890 / Mie 1908; Bohren and Huffman 1983 App. A | | Mie theory, BHMIE |
| MIEV0, Appl. Opt. 19:1505 | Wiscombe, 1980 | Mie code (licence unverified) |
| Mie bulk media, ACM TOG 26(3), DOI 10.1145/1276377.1276452 | Frisvad, Christensen, Jensen, 2007 | Mie phase (DTU code has no licence) |
| Hansen and Travis 1974; Deirmendjian 1969 (C1 model alpha=6, gamma=1, b=1.5) | | Droplet size distributions |
| J. Climate 6:728 | Hu and Stamnes, 1993 | Cloud microphysics (as cited in cloud notes) |
| Hale and Querry 1973; Segelstein 1981; Pope and Fry 1997 | | Water optical constants (as cited) |
| JGR 113 D14220 | Warren and Brandt, 2008 | Ice optical constants (as cited) |
| JAS 70:330, Zenodo 5348402, CC-BY-4.0 | Yang et al., 2013 | Ice optical properties |
| JQSRT 189 | Bi and Yang, 2017 | Ice optical properties |
| JQSRT 146:123 | Baum et al., 2014 | General habit mixture ice |
| DOI 10.1145/3587421.3595409 | Jendersie and d'Eon, 2023 | Cloud-related, as cited |
| Bouthors et al. 2008 I3D; Schneider and Vos 2015; Hillaire 2016, 2020; Burley et al. 2018 | | Real-time and production cloud/volume references, as cited |
| ACM TOG 36(6), arXiv:1709.05418 | Kallweit et al., 2017 | Deep scattering in clouds |
| pbrt-v4 section 11.3 (Beer-Lambert), 11.4.2 (`MajorantGrid`, `DDAMajorantIterator`), `integrators.cpp` (`VolPathIntegrator::Li`, `SampleLd`), `MediumInterface`, `shapes.cpp` (Apache-2.0) | Pharr, Jakob, Humphreys | Majorants, spectral MIS, boundary |
| pbrt-v3 `HomogeneousMedium::Sample`, `HenyeyGreenstein::Sample_p` | Pharr, Jakob, Humphreys | Homogeneous free flight, phase |
| OpenVDB v12.0.0 `nanovdb/NanoVDB.h`, `tools/GridBuilder.h`, `tools/CreateNanoGrid.h` (Apache-2.0; OpenVDB core MPL-2.0) | | NanoVDB build |
| Cycles `src/scene/integrator.cpp` (`device_update`, max_volume_bounce + 1); `src/kernel/integrator/path_state.h` (`LABEL_VOLUME_SCATTER`); `src/kernel/types.h` (`PATH_RAY_TERMINATE_AFTER_TRANSPARENT`) | Blender Foundation | Volume bounce semantics |
| Cycles `src/kernel/integrator/shade_volume.h` (v5.2.0; read near lines 908, 1011, 1315, 1411, 1491, 1559, 1595): `integrate_volume_sample_direct_light`, `volume_valid_direct_ray_segment`, `volume_direct_scatter_mis`, `integrate_volume_direct_light`, `volume_emission_integrate` (Apache-2.0) | Blender Foundation | Segment NEE, emission |
| Cycles `volume_stack.h` (`volume_stack_enter_exit`), `intersect_volume_stack.h` (+Z probe ray), `volume.h` | Blender Foundation | Medium stack |
| Cycles main a456b761 `src/kernel/light/tree.h`, `sample.h` (Apache-2.0) | Blender Foundation | Light tree segment importance |
| Cycles `svm/closure.h`; `node_shader_volume_scatter.cc` | Blender Foundation | Principled Volume mapping, blackbody, Mie phase |
| miepython (MIT), PyMieScatt (MIT), hyperion-rt/bhmie (BSD-2) | | Mie reference codes |

Licence notes as recorded: Cycles and pbrt-v4 are Apache-2.0; the Blender `bpy` API is GPL at runtime and adds no obligation for a separate renderer; OpenVDB core is MPL-2.0; Wiscombe MIEV0 licence unverified; the DTU code for Frisvad et al. has no licence.

## Measured numbers worth keeping

Ratios "x" are per-sample variance versus a Cycles twin scene unless stated. CPU, 64 spp unless stated.

- Segment direct light (bounded fog, chromatic spot, 12 seeds) normalised variance R/G/B: 0.53/0.50/0.88 (about 100x/100x/180x Cycles) before, 0.0144/0.0114/0.0328 (2.8x/2.3x/6.8x) after. World fog 0.41/0.36/0.73 -> 0.0082/0.0054/0.024, means agreeing to 0.2 % at 2048 spp.
- Chromatic-noise ladder before the fix (64x64 spot colour 0.6/0.75/1.0, 12 kW, fog grey 0.5, density 0.08, g 0.5): 2.02/1.88/3.25 at 16 spp, 0.53/0.50/0.88 at 64, 0.17/0.15/0.27 at 256 (slope about -0.9); Cycles 0.0052/0.0050/0.0049. Bright ROI about 500x. Floor-only 0.035/0.034/0.073 vs 0.0022. Mechanism: rare medium NEE (absorption terminates the path with pAbsorb 0.5) plus hero-wavelength pdf under-sampling blue by 2x; a delta-lit floor with no medium already carried 0.0024/0.0005/0.0155 from that under-sampling.
- Tracked-segment direct (20 seeds): smoke plume 21.6x -> 2.05x, light shaft 14.9x -> 2.14x, full scene 3.28x -> 2.98x, trimmed scene 8.62x -> 3.40x. Separate light sample counts only moved 7.25x -> 5.15x.
- Light-tree segment (shaft): mean 0.121-0.134 -> 0.184-0.193 (reference 0.187-0.189); relVar 7.21 -> 0.517 (reference 0.112); N*relVar at 16/64/256 spp 69/461/115 -> 34.9/33.1/30.5 (reference 12.8/7.2/4.9). Smoke VDB N*relVar about 90 vs 1.2 before the majorant fix.
- Lamp-variance ablation (scatter cube density 4, g 0.55, 900 W rectangle backlight, 3 seeds): spherical-rectangle draw 0.80-0.92x at 64 spp and NEE-only equiangular 1.31x -> 0.98x; after the full change per-sample relVar 0.91/0.38/0.67/0.42 over four variants. Equal-spp excess is mostly sampler: Cycles N*relVar falls 4.5x from 1 to 64 spp (Sobol/blue noise); a Mersenne-Twister CPU stays flat. Equal-time: 2.10x, 1.48x, 1.38x slower per sample on three variants. Chroma noise floor on a directly viewed white emitter: R/B relVar about 0.002.
- Blackbody LUT within 0.05 % of Cycles at 60.5-6543.2 K (float memo errors up to 7 % at 500 K).
- World-fog god-ray CPU/GPU mean ratio per channel about [1.004, 0.997, 0.998] with independent RNG streams.
- GPU cost of a world-scatter decision in the intersect stage: +3 registers (127 -> 130) halved occupancy (2 -> 1 blocks/SM), 3.3 % fleet regression (116.4-116.9 ms vs 120.6 ms); four shave attempts failed. Resolved by a compile-time flag keeping fog-free scenes on the lean kernel. Mesh-medium inside tests on the shade path projected at +20-30 registers.
- Sizes: dense 512^3 float grid 512 MB; a 16x4x32 Mie table (radius x variance x wavelength, with 1024-2048 angle bins) about 33 MB.
- Cloud magnitudes recorded: stratus sigma_ext 0.01-0.15 per m; cumulus 0.02-0.4; cirrus 1e-5 to 5e-3. Liquid bulk absorption about 0.05 per m at 550 nm and about 0.6 at 700 nm (factor-2 uncertain).
- Multiple scattering at omega_0 about 1 and optical depth 20-100 needs hundreds to thousands of events per path.
- Cycles artifact: a box whose bottom face is coplanar with the floor makes Cycles darken the floor to 0.53x; not a physics reference.

## Dead ends and negative results

- Distance sampling with a global majorant on heterogeneous media (79x, later 21.6x on a smoke plume): replaced, not tuned.
- Distance-only free flight with no segment direct light: 3.45e4x variance.
- Supervoxel edge 4/8/16: no effect on variance.
- Separate light sample counts: 7.25x -> 5.15x, far from reference, not a Cycles mechanism.
- Medium-only clamp, white lamp, chromatic vs grey fog, clamp off, world fog vs bounded fog: each ruled out as the cause of chromatic noise.
- Lamp hit-site pdf change (0.96-0.98x) and light-tree on/off: acquitted as causes of lamp-variance excess.
- A tilted rectangular lamp's floor footprint was 17.6 vs Cycles 3.6 (4.9x): a lamp-sampling problem, not a volume one, flagged as a likely blocker for later work.
- Per-kelvin integer memo and float-typed normaliser for blackbody (see Findings).
- Dropping the continuation at the volume bounce limit (black smoke).
- Chasing bit-identity between targets: only ulp differences survive compiler inlining and FMA.
- HG and double-HG for clouds: no fogbow, glory or corona.
- Putting volume sampling inside a register-saturated shading kernel: occupancy halved, fleet slowed.
- A cached boundary list for GPU mesh media: rejected for register cost.
- Volume path guiding (VSPG): deferred as the remaining lever; nothing built or measured.

## Design ideas worth reconsidering in the rewrite

- One null-scattering free-flight formulation written once for every target: delta tracking with a spectrally constant majorant, ratio tracking for shadows, hero-wavelength `r_u` update. The wavelength-independent majorant is what keeps it simple, so make sigma_bar a property of the medium.
- Per-wavelength extinction native from day one, with sigma_s and sigma_a derived from OpenPBR volume parameters; Cycles' mapping (above) becomes a Blender-side import adapter. The grey collapse is not worth repeating.
- Emission as an additive term at tentative collisions with a density-independent emission majorant floor. Spectral emission is central to the astrophysical goal; evaluate Planck from one shared table so all targets agree.
- One medium-membership mechanism: the "next boundary crossing is back-facing" rule plus the summed-majorant sweep, covering camera-inside, nesting and overlapping media (fire behind smoke) without a probe-ray init pass.
- Segment sampling baseline: equiangular + distance + null-collision chain + reservoir over scatter candidates at every bounce, with light-tree segment importance (Eq. 4). The largest variance win v1 found; do not treat it as an afterthought.
- A low-discrepancy or blue-noise sampler in volumes: the biggest remaining excess after algorithmic fixes was a flat Mersenne-Twister N*relVar versus Cycles' falling one.
- Upload sparse NanoVDB structure rather than a dense block; load `.vdb` through the DCC integration, keep a standalone `.nvdb` reader. Keep volume code in its own stages so shading registers are not charged for it.
- Counter-based RNG keyed by bit fields (pixel, sample, bounce, dimension). With one kernel source CPU and GPU can share the stream, allowing exact comparison instead of mean-ratio gates.
- Cloud material (only when a real consumer exists): tabulated bulk phase with inverse-CDF sampling and hero MIS, per-voxel mixed-phase blend.
- Every accepted volume parameter must be consumed or must raise.
- Volume path guiding as the next lever if a dense scene misses its variance target.

## What no longer applies

- Matching Cycles' grey-collapse sigma mapping as ground truth: keep only as an importer.
- Matching Cycles' blackbody Rec.709 polynomial and 800 K clamp: 2.0 uses Planck spectra.
- The list of terminate-after divergences from Cycles: Blender-semantics knowledge, not a parity task.
- The Cycles coplanar-floor darkening artifact.
- Variance "x Cycles" as acceptance gates: use analytic transmittance, furnace and brute-force reference tests.
- Mean-ratio CPU/GPU gates for independent RNG streams: moot with one kernel source.
- Register, stage and occupancy tuning of v1's GPU kernels (stage splits, constant-memory flags, side tables).
- GPU twins of v1's mesh-boundary, segment-NEE and grid-binding code.
- Light-tree changes tied to v1's node structure.
- Direct/indirect volume pass layout keyed to v1's pass system.
- Legacy constant-medium shape, orphaned isotropic phase plugin, Lambertian-phase RGB volume: dead v1 code.
- v1's silent dropping of the volume-bounces setting in the addon: a v1 bug.
- v1's package ordering for volume work.

## Open questions

- Which wavelength-dependent volume behaviour does OpenPBR specify and how does Blender 5.3 expose it? The digests do not say.
- Is a spectrally constant majorant tight enough for strongly chromatic media (emission-line nebulae, thick dust), or does decomposition tracking pay off? v1 chose hero MIS and did not measure this.
- What should 2.0 do for open or inconsistently wound meshes? v1 inherited Cycles' failure modes.
- Best sparse-grid transfer from Blender: dense block over the bbox, native grid via the Blender API, or reading `.vdb` directly (MPL-2.0 concern).
- Cloud simulation export: LWC + N + r_eff versus bin-resolved spectrum; mixed-phase policy; whether rain/graupel are separate populations; whether a Cycles fallback node tree is needed.
- Is the multi-scatter cost (hundreds to thousands of events at omega_0 about 1, tau 20-100) acceptable, or does dense cloud need a different approach? The digests list Bouthors, Kallweit, Hillaire and Schneider and Vos but record no conclusion.
- Motion-blurred media versus multi-scatter as an optional follow-up was never decided.
- Recorded open in v1: more than one medium along a segment in the full nested case, a transparent-bounce limit that works with media, and Cycles' terminate-after continuation through transparent surfaces.
- Whether volume guiding is needed once sampler and segment baseline exist; no measurement.
- No GPU variance figures exist in the digests; all ladders are CPU on a handful of fixtures.
