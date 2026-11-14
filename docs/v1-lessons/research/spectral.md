# Spectral rendering: upsampling, hero wavelengths, spectral MIS, observers, emission spectra, spectral node system

Carried over from Astroray 1.0 research notes. Written so it makes sense without knowledge of v1's code. Where v1
compared against Cycles, that is kept as knowledge of Blender semantics and as reference algorithms, not as a parity
target. Numbers were measured in v1 on v1's engine; treat them as order-of-magnitude anchors and re-measure.

## Established findings

### Why spectral at all (v1 design rationale)
- Doppler shift, gravitational redshift and emission lines are wrong in RGB. Dispersion comes almost free once the
  renderer is spectral. Measured spectral reflectance databases (RGL, MERL-class) cannot be represented in RGB.
- Cost recorded by v1: 4 wavelengths per path cost about 33% more than RGB; Jakob-Hanika (JH) upsampling is about
  6 FLOPs per RGB-to-spectrum conversion (coefficients fetched once per RGB value, sigmoid evaluated per wavelength).
- v1 design: PBRT-v4 / Mitsuba 3 style `SampledSpectrum` + `SampledWavelengths`, 4 hero wavelengths, JH tables
  shipped as data, per-path lifecycle (sample wavelengths, trace, convert to XYZ, accumulate divided by the number
  of samples). Invariant: every radiance and reflectance value in the integrator is a sampled spectrum; no path
  collapses to RGB between bounces.
- v1 non-goals: more than 4 wavelengths, per-wavelength storage per pixel (use an AOV instead), metameric light
  sources (Todova-Wilkie 2025 noted as a possible future plugin).

### Observer and colour space (single most expensive bug class)
- The engine integrated spectra with the CIE 1964 10-degree CMF but converted XYZ to RGB with the CIE 1931
  2-degree sRGB matrix, and the JH table had been fit against the 1931 2-degree observer. Result: grey and
  low-chroma colour skew (neutral ground tinted, white blue channel -1.7%, chrome green +8%).
- Rule: the CMF used to integrate, the XYZ-to-RGB matrix, the white point and the observer used to fit the
  upsampling table must all be the same observer (CIE 1931 2-degree for sRGB/D65 primaries; sRGB primaries and D65
  are defined in CIE 1931 xy by IEC 61966-2-1:1999 and ITU-R BT.709-6 section 1).
- Sanity check that caught it: D65 through the CMF and matrix must return (1,1,1). 2-degree: (0.9999, 1.0001,
  0.9997); 10-degree: (1.0001, 1.0017, 0.9831). Also check the blackbody against an independent oracle
  (colour-science `sd_blackbody`).
- After the fix D65 white XYZ = (0.95047, 1, 1.08883). Saturated albedo under a coloured sky retains a physical
  residual up to ~9-10% versus an RGB renderer: that is metamerism, not a bug, and not a reason to tune.
- Several tests had encoded the broken colour behaviour as a baseline and had to be re-pinned; two comparison
  metrics were judged non-physical once the observer was right. Lesson: when a global colour fix lands, expect
  green tests that were measuring the bug.

### RGB-to-spectrum upsampling (Jakob and Hanika 2019)
- JH is a 3-coefficient sigmoid-of-quadratic spectrum per RGB, from a precomputed 3D table (v1: 64^3, three
  coefficients, 4D including the max-channel scale axis). The upsampling tables are exact enough: a 81-point
  unit probe (380-780 nm, 5 nm) showed albedo and illuminant mean ratio 1.000000 between two independent
  evaluators, overall mean relative error 2e-6 (albedo) and 1e-7 (illuminant).
- JH is nonlinear in magnitude. Scaling RGB moves the max-channel coordinate and selects a different sigmoid.
  Therefore: upsample the albedo (the reflectance-like quantity), then multiply by cos/pi and other scalar
  factors in spectral space. Never upsample a pre-scaled BSDF value. v1's GPU diffuse path did, and saturated
  colours were up to 2.5% per channel off; fixed it dropped to ~0.02%. Neutral albedo was unaffected, which is the
  diagnostic signature of this class of bug (chroma-driven error).
- Same rule for every lobe: Fresnel and any energy-compensation term must be per-wavelength together, never one
  spectral and one RGB. Mixed treatment gave GPU/CPU chromatic albedo divergence (blue ratio 1.0722 at r=0.9 on
  metal) in v1; resolution was making metal fully per-wavelength on both targets. v1's rule of thumb: conductor
  per-lambda, Disney-style dielectric/diffuse mixes per-RGB, glass scalar-neutral. Dropping to RGB was rejected as
  it lowers spectral accuracy.
- Per-wavelength conductor thin film is equivalent to an RGB upsample in practice (noted only; no number recorded).
- Scale-axis lookup: the table's scale axis is generated as scale[k] = smoothstep(smoothstep(k/(res-1))). Direct
  index inversion has float-rounding issues; bisection (6 steps for res=64) is exact by construction and gives the
  same interval as a linear scan for any monotone table. Pbrt-v4 `FindInterval` is the pattern.
- Illuminant vs albedo upsampling (RGB emission colours): see the emission section.

### Emission colour from RGB
- Question was whether RGB emission colour should use the illuminant-style upsample (RGB sigmoid times D65) or the
  reflectance-style unbounded upsample. Final decision: keep the illuminant-style (as pbrt-v4 DiffuseAreaLight does
  with SpectrumType::Illuminant and a photometric scale; as Mitsuba's `srgb_d65` emitter does).
- The D65 factor does three jobs: a units anchor (1/CIE_Y integral, about 1/116.66), E-to-D65 white adaptation
  (removes a ~30% red excess), and a small tilt. A flat sigmoid with XYZ(1,1,1) maps to sRGB (1.205, 0.948, 0.909):
  an equal-energy white through a D65 matrix is pink (R/G about 1.27). So a "white" emitter needs the D65
  adaptation or an explicit white-point choice.
- Cycles stores light strength as linear RGB, so any spectral engine must choose its own RGB-to-emission mapping;
  there is no Cycles-defined spectrum to match.
- Method lesson: decompose a spectral factor into shape x magnitude before changing it; validate emission changes on
  a neutral-white render, not a scalar brightness ratio. Swapping to the reflectance-style upsample without the
  photometric anchor made everything about 116x brighter (+11,600%).

### Hero wavelengths and their pdf
- Algorithm: Wilkie et al. 2014. Draw one hero wavelength, place companions by stratifying the uniform number in
  the proposal's CDF space (not in wavelength space), and give every lane its own pdf at its own wavelength. Placing
  companions by offsetting wavelength is biased under a non-uniform proposal. One uniform per sample also keeps CPU
  and GPU random-dimension counters aligned.
- Luminance-weighted hero proposal (as Cycles samples wavelengths from a fixed D65-luminance-shaped logistic CDF,
  and pbrt-v4 `SampleVisibleWavelengths` does): v1 fit a logistic pdf to D65 x luminance against its own tables.
  Result vs uniform sampling: -42% RMSE, -38% chroma noise (the baseline cited later as the bar for spectral MIS).
- The proposal must be windowed to the rendered band [lambda_min, lambda_max] at runtime (renormalise the logistic
  CDF on that interval). Using full-band constants put ~1.2% of draws outside 380-780 nm, clamping those broke f/p,
  and the blue channel was biased about 5x. Windowed: blue error 0.8%.
- Collapse on a wavelength-dependent event (dispersive refraction/reflection): only the hero lane survives
  (terminate secondary lanes). The spectral-to-XYZ conversion divides by N lanes, so the surviving hero's pdf must
  be divided by N once (guarded by a "secondary terminated" flag) to cancel it. Missing it made dispersive glass
  about 4x too dark. v1 audited every consumer of the collapsed pdf (XYZ conversion, regeneration, Russian roulette
  luminance, clamps, reservoir target functions, volume averaging, redshift, buffer write-backs, environment shadow
  parking) and none double-compensated. Source: pbrt-v4 `SampledWavelengths::TerminateSecondary`.
  Unbiasedness test: collapsed estimator against a non-collapsed reference; dispersive glass vs matched flat IOR
  glass within 2%.
- Cycles also collapses to a single wavelength on dispersive events, the same idea.

### Spectral multiple importance sampling (what exists, what does not)
- No canonical "per-bounce spectral MIS" paper exists. v1's spec had three mis-citations that are corrected here:
  Petitjean, Bauszat and Eisemann 2018 is spectral gradient sampling; Pediredla et al. 2020 is path-tracing
  estimators for refractive radiative transfer. Neither is spectral MIS.
- Prior art that is real: Radziszewski, Boryczko and Alda 2009 (per-strategy MIS over wavelengths); Evans and McCool
  1999 (cluster split at dispersive surfaces); Wilkie 2014 (per-vertex joint MIS weights with one hero per path;
  at a Dirac dispersive interface it degrades to single-wavelength behaviour, their Fig. 6).
- Re-selecting the hero at every dispersive bounce is novel and unproven. v1's park criterion: under 10% relative
  variance/chroma reduction vs the luminance-weighted hero at equal spp, or any unbiasedness failure, means park.
  Prior probability of park was judged high. It was never prototyped.
- Spectral ray differentials: Igehy 1999 and Elek et al. 2014. No public code was found for either.

### Dispersion (material side)
- Blender's Principled Abbe number + dispersion scale map to a two-term Cauchy IOR n(lambda) = A + B/lambda^2,
  exactly as OpenPBR v1.1.1 Eqs. 55-56 and Cycles' glass IOR code do. With Fraunhofer lines in micrometres
  (d 0.5876, C 0.6563, F 0.4861): fac = 1/(1/lF^2 - 1/lC^2); B = (n_d - 1) * inv_abbe * fac; A = n_d - B/ld^2;
  inv_abbe = dispersion_scale / abbe (safe divide; Cycles default Abbe 20). v1 matched the merged Cycles source
  character for character. The OpenPBR spec permits Cauchy or a reduced Sellmeier; Cauchy chosen to match Cycles.
  Sellmeier (e.g. Schott BK7 datasheet) remains the better physical form for named glasses.
- Blender semantics: the Principled BSDF dispersion socket is not present in Blender 4.3.2, 4.5.0, 5.1.0 or 5.2.0;
  the Cycles change was merged 2026-08-18 (commit f15daf81bf7c, Cycles PR 162041) and ships in 5.3 alpha. A
  forward-compatible add-on probes socket names ('Dispersion Scale', 'Dispersion Abbe Number', 'Dispersion') and
  no-ops when absent. Re-check against the actual 5.3 release.
- Physics an oracle should assert: a narrow-line source (sodium D, 589 nm) through a prism gives a single-hue band;
  a broadband source gives a full spread. Cycles hero-samples from a fixed D65 CDF and never carries the light's
  SPD to the dispersive event, so it cannot tell the two cases apart; a spectrally correct renderer must. The
  PR author acknowledged this limitation. Hecht, Optics 5th ed., ch. 3 and 5 (section 5.5), is the physics reference.

### Emission spectra and importance sampling of lights
- Light SPD normalization: peak-normalizing (peak bin = 1) couples continuum level to line peak. After
  adding area-conserving line broadening the peak dropped and the continuum rose: stored-SPD sum 4.71 -> 30.60
  (about 6.5x) and mercury rendered 4.5-8.6x too bright. Fix: unit-integral normalization r <- r / sum_j r_j. Total
  power then survives area-conserving broadening, every bin <= 1 (largest, sodium, ~0.19), and absolute magnitude
  lives in the light's power parameter. Reference: Wyszecki and Stiles, Color Science, 2nd ed., 1982.
- Photon/caustic pre-pass and any light-tracing step must sample wavelength from the emitter, not uniformly. A
  uniform draw over [380, 720] nm gave a narrow-line lamp a physically impossible rainbow caustic and starved the
  emission-line wavelengths. Algorithm: p(lambda) = S(lambda)/I with I = sum S * dlambda (1 nm bins, K = 341);
  the weight S/p collapses to the constant I. Inverse CDF by binary search + linear interpolation. Flat SPD gives
  uniform up to the constant. Sources: PBR 4th ed. 4.5.4 and 2.4; Devroye 1986 ch. 2. If the SPD is all zero
  (environment-only), keep the uniform path. For multi-light scenes v1 used only the dominant light's SPD: a
  documented approximation.
- v1 inconsistency, not a recommended design: GPU emissive geometry used an RGB approximation while lamps were
  spectral.

### Spectral hair absorption (Chiang 2016 model)
- Replace RGB -> sigma_a -> upsample with per-wavelength melanin absorption. Exponents only, anchored at 550 nm to
  the Blender-compatible green coefficients: kappa_eu(l) = 0.841 (l/550)^-3.33, kappa_ph(l) = 0.733 (l/550)^-4.75.
  The anchor reproduces the Cycles RGB triple within ~10-24% at the primary wavelengths.
- Rejected: absolute melanosome units, concentration modelling, JH upsampling on this path, tint in spectral mode.
- Acceptance as planned (not recorded as executed): absorption ratio at 500/600/700 nm within 10% of l^-3.33;
  spectral dark hair shows a steeper red/blue spread than RGB.

## Sources and reference implementations

| Source | Used for | Notes as recorded |
|---|---|---|
| Jakob and Hanika 2019, CGF (Eurographics), DOI 10.1111/cgf.13626 | RGB to spectrum | Tables on Zenodo; reader pattern from `simple-spectral` (geometrian) |
| mitsuba-renderer/rgb2spec (`rgb2spec_opt.cpp`, `details/cie1931.h`), BSD-3-Clause | JH table fit, 1931 2-degree observer | Read, not vendored |
| Wilkie et al. 2014, "Hero Wavelength Spectral Sampling", CGF 33(4), DOI 10.1111/cgf.12419 | Hero wavelengths, joint MIS weights | Companions need per-lane pdf at their own wavelength |
| pbrt-v4 (https://github.com/mmp/pbrt-v4, Apache-2.0): `src/pbrt/util/spectrum.h` (`SampledWavelengths::TerminateSecondary`, `SampleVisibleWavelengths`, `RGBIlluminantSpectrum`), `lights.cpp` (DiffuseAreaLight, SpectrumToPhotometric), `FindInterval` (BSD-2) | Hero collapse, visible-wavelength pdf, illuminant emission | Port design, not code |
| Cycles (https://github.com/blender/cycles, Apache-2.0): `src/kernel/light/area.h`, `src/scene/light.cpp`, `intern/cycles/kernel/util/colorspace.h` (`sample_wavelength`), `cie_d65_luminance_fit.py` | Light strength as RGB; D65-luminance logistic hero CDF; hero collapse on dispersion | Logistic CDF with a constant added to the luminance CMF |
| Cycles `intern/cycles/kernel/closure/bsdf_microfacet.h` `microfacet_ggx_preserve_energy` (BSD-3-Clause, E/Eavg tables) | Metal multiple-scatter compensation | Kulla and Conty form below |
| Cycles PR 162041 `bsdf_glass_ior`, commit f15daf81bf7c | Abbe/dispersion mapping | Merged 2026-08-18 |
| OpenPBR v1.1.1 Eqs. 55-56 | Cauchy dispersion | Spec permits Cauchy or reduced Sellmeier |
| Mitsuba 3 (BSD-3-Clause; mitsuba.readthedocs.io; `src/librender/spectrum.cpp`) | Emitter/spectra docs, `srgb_d65` emitter | Does not ship hero sampling by default (discussion 1040) |
| Kulla and Conty 2017, SIGGRAPH course | Multiple-scatter compensation 1 + Fms(1-E)/E | Cited as recorded |
| Radziszewski, Boryczko and Alda 2009, J. WSCG 17(1), pp. 9-16 | Per-strategy wavelength MIS; non-uniform sampling | Cited in pbrt |
| Evans and McCool 1999, Graphics Interface | Cluster split at dispersive surfaces | |
| Petitjean, Bauszat, Eisemann 2018, DOI 10.1111/cgf.13474 | Spectral gradient sampling (NOT spectral MIS) | |
| Pediredla et al. 2020, DOI 10.1145/3414685.3417793 | Refractive radiative transfer estimators (NOT spectral MIS) | |
| Igehy 1999, SIGGRAPH pp. 179-186, DOI 10.1145/311535.311555; Elek et al. 2014, "Spectral Ray Differentials", CGF 33(4), DOI 10.1111/cgf.12418 | Ray differentials, spectral | No public code found |
| MitsubaER (github cmd-ci-lab/MitsubaER) | Looked at, off-topic | |
| IEC 61966-2-1:1999; ITU-R BT.709-6 section 1 | sRGB primaries, D65 in CIE 1931 xy | |
| colour-science 0.4.7 (MSDS_CMFS from cvrl.ucl.ac.uk, public domain; `sd_blackbody`; `normalise`, BSD-3-Clause) | Observer data, blackbody oracle | Conceptual use of normalise only |
| PBR Book 4th ed. (https://pbr-book.org/4ed/) 2.4, 4.5, 4.5.4 | Inversion sampling, normalized spectral density | |
| Devroye 1986, Non-Uniform Random Variate Generation, ch. 2 | Inverse CDF | |
| Wyszecki and Stiles, Color Science, 2nd ed., Wiley 1982 | Relative SPD normalization | |
| Jacques 2013, "Optical properties of biological tissues: a review", Phys. Med. Biol. 58(11) R37-R61, DOI 10.1088/0031-9155/58/11/R37 | Melanin absorption (lumped fit 1.70e12 l^-3.48) | v1 kept -3.33 |
| Jacques 1998, "Skin Optics Summary" (OMLC) | Eumelanin mua = 6.6e11 l^-3.33 cm^-1 (verbatim) | |
| Donner and Jensen 2006, "A Spectral BSSRDF for Shading Human Skin", EGSR | Pheomelanin ~2.9e14 l^-4.75 (mm^-1) | Web-confirmed exponent only; verbatim fetch failed |
| Alaluf et al. 2002 | Motivates eu/pheo split only | |
| Hecht, Optics, 5th ed., ch. 3 and 5 | Dispersion physics | |
| Schott BK7 datasheet | Sellmeier reference | |
| "Spectral rendering, part 3: Spectral vs. RGB" (momentsingraphics.de) | RGB vs spectral emission discussion | |
| RGL material database (rgl.epfl.ch) | Measured spectral BRDFs | |
| Tódová and Wilkie 2025 | Metameric light sources | Future idea only |

## Measured numbers worth keeping
- D65 through CMF + XYZ-to-sRGB matrix: 2-degree (0.9999, 1.0001, 0.9997), 10-degree (1.0001, 1.0017, 0.9831).
- JH table round-trip on a 7^3 grid in [0.05, 0.95] under D65: max abs error 9.9e-4 with matching 2-degree
  observer, 0.117 with the mismatched 10-degree one.
- Observer fix effect: coloured-sky ground G/B spread 5.4 percentage points -> 0.6 pp; chrome green +8% -> +0.1%.
  Blackbody 6500 K (R,G,B normalised to R): oracle 1/0.943/0.992, fixed 1/0.944/0.991, broken 1/0.946/0.983.
- Upsampling tables vs independent evaluator, 81 wavelengths: albedo mean relative error 2.06e-6, illuminant
  1.24e-7; single blip at saturated green, one edge wavelength, max relative error 2e-4, no effect on band integrals.
- Nonlinearity bug: saturated diffuse up to 2.5% per channel off; ~0.02% after upsampling the albedo first.
- Metal chromatic divergence before fix: B ratio 1.0722 at albedo 0.9.
- Illuminant-style emission: flat sigmoid XYZ(1,1,1) -> sRGB (1.205, 0.948, 0.909); D65 role removes ~30% red excess;
  1/CIE_Y integral ~116.66.
- Hero collapse missing pdf rescale: ~4x too dark; with the rescale, dispersive vs flat glass within 2%.
- Luminance hero proposal vs uniform: -42% RMSE, -38% chroma. Logistic fit against v1 tables: a = 0.0221679280,
  x0 = 552.040271 nm, max CDF residual 2.77e-2, pdf integral 0.999999 (this fit used a mismatched observer; re-fit
  in the 2-degree observer: kHeroA +0.8%, x0 552.0 -> 555.7 nm).
- Per-channel variance of a grey D65 floor (R, G, B, luminance): uniform 0.606/0.173/0.669/0.0219; first fit
  0.083/0.020/0.943/0.0012; plain 2-degree refit 0.073/0.022/1.103/0.0010; minimax refit (a = 0.0170,
  x0 = 522.5 nm) 0.121/0.007/0.193/0.0011; pbrt visible-wavelength 0.023/0.001/0.340/0.0013.
- Band-window bug: ~1.2% of draws outside 380-780 nm, blue bias ~5x, fixed to 0.8% blue error.
- Light SPD normalization: stored-SPD sum 4.71 -> 30.60 (6.5x) after broadening with peak normalization; mercury
  4.5-8.6x too bright. Unit-integral: all bins <= 1, largest ~0.19 (sodium).
- Photon pre-pass wavelength pdf: 341 bins, 1 nm; weight collapses to the constant I.
- Scale-axis bisection: 6 steps for resolution 64.
- Melanin: exponents -3.33 (eu), -4.75 (pheo), anchor 550 nm, coefficients 0.841 and 0.733; matches Cycles RGB triple
  within ~10-24% at primaries.
- Mercury lamp chromaticity under the corrected observer: xy (0.334, 0.368) -> (0.317, 0.396).
- Residual GPU/CPU transport bias on triangles after the spectral upsampling fix: sphere [0.99988, 1.00004, 1.00004],
  floor [1.01043, 1.00583, 1.01015], neutral floor ~1.006 uniform; constant across depths 2-4, independent of
  background brightness. Not an upsampling effect.
- Spectral vs RGB cost: ~33% more for 4 wavelengths.

## Dead ends and negative results
- Reflectance-style upsample for emission without the photometric anchor: 116x too bright; the apparent +7-16%
  residual offset that motivated it was a misattribution (the reference rows were already 0.96-1.01).
- Offsetting companion wavelengths linearly under a non-uniform hero proposal: biased. Use CDF stratification.
- Full-band logistic constants with clamping: blue bias ~5x.
- Plain re-fit of the hero logistic to the corrected observer: blue variance got worse (1.10 vs 0.94). A tabulated
  proposal D65*(alpha*xbar + beta*ybar + gamma*zbar) was no better than the minimax logistic (max normalised
  variance 0.197 vs 0.193). Equalising channel variance costs ~24x luminance variance, so no table on the GPU. The
  minimax fit makes red-filtered D65 and 3000 K worse in green (0.65 -> 3.3 and 0.10 -> 0.37). No single hero
  proposal wins everywhere.
- Hypothesis "GPU/CPU render mismatch is born in the upsampling tables": false. Tables, evaluator, resolution,
  interpolation order, fp32 quantisation and illuminant normalisation were all ruled out. The gap was call structure:
  a pre-scaled value upsampled on one side only. Only appears from the second bounce onward, when throughput is
  multiplied by next-bounce factors; unit probes and background-only renders were clean and hid it.
- Dropping CPU metal to RGB to match the GPU: rejected on accuracy.
- A dedicated-light test that never measured the light (emitter faced away, world black): silently passed.
- Peak-normalised SPDs with broadening: see normalization above.
- Pheomelanin constant and the units reconciliation of eumelanin (cm^-1) vs pheomelanin (mm^-1) were not reconciled
  in the notes.
- Per-bounce spectral MIS: no source, never prototyped (see open questions).

## Design ideas worth reconsidering in the rewrite
- One observer, one white point, declared once and consumed by the CMF integrator, the XYZ-to-RGB matrix and the
  upsampling-table fit. Add a standing D65-to-(1,1,1) test and a blackbody oracle test.
- Upsample albedo, not BSDF values. Make "spectral through the whole lobe evaluation" the rule: per-wavelength
  Fresnel, compensation, thin film, tint. Do not mix RGB and spectral halves inside one lobe. With a single kernel
  source for CPU and GPU this class of bug disappears by construction; still worth a render-level neutral vs
  saturated-albedo test.
- Keep a hero proposal that is windowed to the rendered band and stratified in CDF space. Consider a per-light or
  per-scene proposal (light SPD times luminance) rather than a fixed D65 fit, given the line-lamp and astrophysical
  emission targets; Cycles' fixed D65 CDF cannot represent narrow lines. Expect a trade: any single logistic hurts
  coloured illumination somewhere.
- Emission: store light SPD unit-integral normalised with an explicit absolute-power parameter. Use the
  illuminant-style RGB upsample with the D65 adaptation for RGB-specified lights, and take astrophysical emitters
  (lines, blackbody, redshifted spectra) directly as SPDs.
- Importance-sample wavelength from the emitter SPD for light tracing and caustic-style passes from the start.
- Treat the "collapse on dispersive event" pdf rescale as a single guarded operation with one owner, and audit
  every consumer of the pdf (roulette, regeneration, reservoirs, volumes, redshift).
- Wavelength-dependent IOR: Cauchy form matching OpenPBR Eqs. 55-56 as the default Principled-compatible mapping;
  allow Sellmeier for named glasses.
- Spectral hair absorption per wavelength from eumelanin/pheomelanin power laws (anchored to Blender-compatible
  coefficients) is a cheap upgrade over RGB sigma_a.
- Metameric emission and other spectral features (Todova-Wilkie 2025) were considered as a plugin; revisit if
  observer-accurate instrument modelling needs them.
- Per-pixel wavelength storage as an optional AOV rather than a core buffer layout.

## What no longer applies
- Matching Cycles pixel-for-pixel (including oracle gates in [0.97, 1.03]): replaced by physics and OpenPBR criteria.
- Cycles' D65 hero CDF as the default: a reference algorithm only.
- v1's CPU/GPU twin implementations and per-lane parity tests (the two sides disagreeing on where upsampling occurs).
- Register-pressure limits on a shade kernel and template-axis gating of new spectral state: v1 code structure.
- A JH re-fit forced to run in CPU+GPU lockstep: gone with one kernel source.
- The "RGBUnbounded plus explicit anchor" intermediate fix: abandoned, kept only as a lesson.
- The build/stale-binary workflow, test-baseline re-pinning, and hardware-gate scheduling recorded alongside these
  notes.
- The Blender 4.2+ "Dispersion socket exists" premise: factually wrong for 4.3.2-5.2.0; see dispersion section.

## Open questions
- Whether re-selecting the hero at each dispersive bounce beats the luminance-weighted proposal at all (bar: >=10%
  variance/chroma reduction at equal spp). No prototype exists.
- Best hero proposal for arbitrary lighting, including line emitters, redshifted spectra and coloured illumination:
  a fixed logistic loses somewhere; the digests record no alternative that wins universally.
- The mercury lamp chromaticity shift under the corrected observer (0.334, 0.368) -> (0.317, 0.396) means the earlier
  claim that the SPD data differ by about 0.01 in xy needs re-checking.
- Unpigmented hair rendered about 11% brighter on the GPU: cause not recorded in the digests.
- Reconcile the eumelanin (cm^-1) and pheomelanin (mm^-1) constants; check the pheomelanin constant against the
  Donner and Jensen paper directly. Jacques 2013's later lumped fit (-3.48) vs -3.33 is undecided.
- Residual ~0.6-1.0% GPU/CPU transport bias on triangles (shading vs geometric normal, cosine normalisation, an
  f/(pdf+1e-3) epsilon were the candidates). Cause was localised, not found.
- What Blender 5.3's released Principled BSDF dispersion socket names and defaults actually are.
- The absolute-unit link between the emission anchor and photometric light units (SpectrumToPhotometric analogue)
  for astrophysical sources: the digests give the RGB-light case only.
- Performance of spectral rendering relative to other renderers: tracked in v1 as an open acceptance item; no data.
- Spectral accuracy of RGB-specified albedo under strongly coloured illumination: the ~9-10% metameric residual is
  understood but not bounded for general scenes.
