# Spectral and material model for Astroray 2.0

Research date 2026-11-14. Tags: **[V]** VERIFIED = I read the primary source (spec, source code, release notes, official docs); **[R]** REPORTED = secondary source, search snippet, or a source I could only partly read; **[U]** UNVERIFIED = background knowledge or my own derivation, not checked this session.
Caveat on [V]: web pages were read through a summarising fetch tool, not byte-for-byte. Formulas marked [V] must be re-read from the cited source before they go into code (project rule 5).

## Question

Define the spectral model (Part A) and the material model (Part B) for Astroray 2.0.
- A: wavelength sampling (count, stratification, MIS, dispersion), wavelength range and extension beyond the visible, units, sensor/CMF/Blender colour management, RGB-to-spectrum upsampling, emission and measured data, fluorescence and polarisation cost.
- B: the OpenPBR Surface spec (current version and changelog), the MaterialX reference graph, every lobe/parameter, the layering scheme, what must be spectral, furnace tests, the Blender 5.3 Principled mapping, Blender 5.4 OpenPBR status, and the internal material representation.

Fixed by the owner: physics + OpenPBR spec is the target (no Cycles pixel parity); one kernel source for CPU and GPU; Blender 5.3 now, 5.4 next.

## Findings

### A1. Hero-wavelength sampling as shipped in pbrt-v4

- **[V]** pbrt-v4 traces `NSpectrumSamples = 4` wavelengths per path over `Lambda_min = 360` to `Lambda_max = 830` nm. https://raw.githubusercontent.com/mmp/pbrt-v4/master/src/pbrt/util/spectrum.h
- **[V]** Stratification uses one random number. `SampleUniform(u)` sets lambda[0] from u and places the others at steps of (max-min)/4, wrapping past max. `SampleVisible(u)` does the same in u-space: sample i uses `u + i/4` wrapped into [0,1], then maps through the inverse CDF. Source: spectrum.h above; text at https://pbr-book.org/4ed/Radiometry,_Spectra,_and_Color/Representing_Spectral_Distributions
- **[V]** Visible-band importance density (Apache-2.0): `pdf(l) = 0.0039398042 / cosh^2(0.0072 (l - 538))` on [360,830], sampled by `l = 538 - 138.888889 * atanh(0.85691062 - 1.82750197 u)`. https://raw.githubusercontent.com/mmp/pbrt-v4/master/src/pbrt/util/sampling.h
- **[V]** The film estimator divides each sample by its wavelength pdf (`ToXYZ`: mean of s_i X(l_i)/p(l_i), divided by `CIE_Y_integral = 106.856895`). https://pbr-book.org/4ed/Radiometry,_Spectra,_and_Color/Color
- **[V]** pbrt's own test: stratified beats independent wavelengths by a large margin. Error gains level off past about 6-8 wavelengths in a complex scene; 32 only pays in a simple scene (8 to 32 cuts error ~500x but images look alike). Same page.
- **[V]** Dispersion: `DielectricMaterial::GetBxDF` takes `sampledEta = eta(lambda[0])` and, if `eta` is not a `ConstantSpectrum`, calls `lambda.TerminateSecondary()`. `TerminateSecondary` zeroes pdf[1..3] and divides pdf[0] by 4 (Russian-roulette-like; code must ignore zero-pdf samples). https://raw.githubusercontent.com/mmp/pbrt-v4/master/src/pbrt/materials.h
- **[V]** Rule: the same wavelengths are used for the whole path; mixing spectra from different packets is a bug. pbr-book page above.
- **[V]** pbrt `ConductorMaterial` takes spectral eta/k; if only a reflectance is given it sets eta=1 and k = 2 sqrt(r)/sqrt(1-r) (clamped). materials.h above.

### A2. MIS across wavelengths (Wilkie et al. 2014, Manuka, Mitsuba)

- **[R]** Hero Wavelength Spectral Sampling (Wilkie, Nawaz, Droske, Weidlich, Hanika, EGSR 2014): one hero wavelength drives path sampling; the other C-1 are deterministic shifts; the estimator averages C wavelengths with the balance heuristic w_j = p(X|l_H) p(l_H) / sum_k p(l_k) p(X|l_k). If the path pdf depends on wavelength (dispersion, delta refraction) only the hero keeps non-zero weight. Source: a student slide deck (https://graphics.cg.uni-saarland.de/courses/ris-2021/slides/Spectral%20Raytracing.pdf) and the paper's landing page https://cgg.mff.cuni.cz/~wilkie/Website/EGSR_14.html. The paper PDF exceeded my fetch size cap, so I did not read it.
- Reading of the above: pbrt's `TerminateSecondary` is the same idea in its cheapest form (hero only, pdf/4). The full balance-heuristic MIS only matters when secondary wavelengths still have non-zero pdf (rough dispersive transmission). pbrt does not exploit that. This is my inference [U].
- **[R]** Manuka (Fascione et al., ACM TOG 2018) uses continuous wavelengths with hero sampling; the wavelength count is not stated in any source I could open. https://www.fxguide.com/?p=51887 (search snippets only).
- **[V]** Mitsuba 3 spectral mode covers (360...830) nm and treats wavelength as one more path dimension. https://mitsuba.readthedocs.io/en/stable/src/key_topics/variants.html. Its sample count per path was not on the page; I believe 4 [U].
- **[R]** Open issue from the same slides: shift-by-fixed-stride is agnostic to wavelength probability; continuous MIS (West et al. 2020) fixes it. Fluorescent vertices sample a new wavelength and fold into the path pdf.

### A3. Beyond the visible

- **[V]** Nothing in pbrt-v4 is tied to 360-830 except the constants and the CMF tables (471 samples, 1 nm). Wavelength sampling is an inverse CDF over `u`, with the stratification rotation done in `u`. So a wider domain or a band mixture is a change of CDF, not of structure (follows from sampling.h and spectrum.h above).
- **[V]** Reflectance spectra from the sigmoid-polynomial table are fit only against the CIE 1931 CMFs on 360-830 (pbr-book 4.6.6 above). Outside that range the polynomial extrapolation is not constrained by any data: UV/IR reflectance of an RGB-specified material is whatever the sigmoid does. This needs an explicit policy (clamp-to-edge, or measured spectra).
- **[V]** Free measured data: refractiveindex.info database is CC0 (copyright waived, commercial use allowed, cite the 2024 Scientific Data article). https://raw.githubusercontent.com/polyanskiy/refractiveindex.info-database/main/README.md. pbrt-v4 ships tabulated eta/k for Ag, Al, Au, Cu, CuZn, MgO, TiO2 and seven glasses (BK7 etc.; glass data from refractiveindex.info, CC0), irregular 5-22 nm spacing. https://raw.githubusercontent.com/mmp/pbrt-v4/master/src/pbrt/util/spectrum.cpp
- **[V]** X-ray optical constants: CXRO/Henke tables, E = 50-30000 eV, Z = 1-92 (Atomic Data and Nuclear Data Tables 54, 1993). https://henke.lbl.gov/optical_constants/. Licence terms are in an LBNL "Notice to Users" I did not read [U].
- **[U]** In X-ray, n = 1 - delta + i beta, so Fresnel reflection is only relevant at grazing angles and surface shading as in OpenPBR is the wrong model. Science output beyond the near-IR is almost entirely volume emission/absorption (see 00-recon astro-science section), not surfaces.
- **[U]** float32 hazard for wide domains: Planck's law needs exp(c2/(l T)) with c2 = 1.4388e7 nm K. For l = 0.1 nm and T = 5000 K the exponent is ~2.9e4, far past float32 overflow (~88). Planck must be evaluated with a cutoff or in log space; a decade-wide domain makes this routine.

### A4. Units, sensor, CMFs and Blender colour management

- **[V]** pbrt normalises `BlackbodySpectrum` to peak 1 (Wien peak, `normalizationFactor`), and offers `SpectrumToXYZ`, `SpectrumToPhotometric`. Scale is carried separately. spectrum.h / spectrum.cpp above. pbrt also ships the Weta PhysLight camera data (Canon EOS 100D curves), so absolute-unit film is established practice there.
- **[V]** Spectral to display: integrate against CIE XYZ, then transform to the working colour space; the display view transform (AgX etc.) belongs to the host. Photon energy is hc/lambda, so photon-count output is an extra factor lambda/(hc) applied at the film, not in transport (slides above [R]; arithmetic [U]).
- **[V]** Blender 5.0 lets each .blend choose a working colour space: Linear Rec.709 (default), Linear Rec.2020, ACEScg. `bpy.data.colorspace.working_space_interop_id` returns `lin_rec709_scene`, `lin_rec2020_scene` or `lin_ap1_scene`; `mathutils.Color.from_scene_linear_to_rec709_linear` and `from_rec709_linear_to_scene_linear` convert material and light colours. https://developer.blender.org/docs/release_notes/5.0/python_api/ ; default and options also in https://www.cgchannel.com/2025/11/blender-5-0-is-out-check-out-its-5-key-features [R].
- **[V]** Blender 5.3: `Image.colorspace_settings.interop_id` (PR #164213); values come from the Color Interop Forum, e.g. `data`, `srgb_rec709_display`, `srgb_rec709_scene`; blend-file load updates names from interop ids. https://developer.blender.org/docs/release_notes/5.3/python_api/
- **[V]** OpenPBR metadata default colour space for colour parameters is ACEScg (following MaterialX); default length unit is centimetres, thin-film thickness in micrometres. https://academysoftwarefoundation.github.io/OpenPBR/ (Formalism: Metadata).
- **[V]** The white point matters: pbrt's illuminant spectrum for ACES is D60 (`ACES_Illum_D60`); sRGB/Rec.709 is D65. A flat (1,1,1) RGB maps to a constant spectrum for reflectance (pbr-book 4.6.6); for emission the colour space's own illuminant must multiply in (`RGBIlluminantSpectrum`).
- **[V]** pbrt ships upsampling tables for sRGB, DCI-P3, Rec2020 and ACES2065-1 only. There is no AP1 (ACEScg) table, so Blender's `lin_ap1_scene` working space needs a table generated by us (pbr-book 4.6.6 above; rgb2spec tooling below).

### A5. RGB to spectrum

- **[V]** Jakob-Hanika 2019: spectrum = sigmoid(c0 l^2 + c1 l + c2), coefficients from a 3D table; round-trip exact over the sRGB gamut; storage like an RGB texture; "as few as six floating point instructions" per wavelength [R from paper abstract via search]. Reference tool `rgb2spec` is BSD-3-Clause. https://raw.githubusercontent.com/mitsuba-renderer/rgb2spec/master/LICENSE
- **[V]** pbrt layout: `float[3][64][64][64][3]` per colour space, split by largest RGB component, trilinear lookup. That is 3 x 64^3 x 3 floats = 9.4 MB per space (my arithmetic from the dims). Three wrapper types: `RGBAlbedoSpectrum` (bounded [0,1]), `RGBUnboundedSpectrum` (scale = 2 max component, rgb normalised to max 1/2), `RGBIlluminantSpectrum` (same, times the space's illuminant). pbr-book 4.6.6 and spectrum.h above.
- **[R]** Known limits (slides): interpolating coefficients is not the same as interpolating RGB; black needs a special case; the table is per colour space; out-of-gamut RGB (outside [0,1]^3 in that space) cannot be a reflectance.
- **[R]** Peters, Merzbach, Hanika, Dachsbacher, "Using Moments to Represent Bounded Signals for Spectral Rendering", SIGGRAPH 2019: moment-based bounded spectra; follow-up "Fourier sRGB" lookup (MAM 2019) maps sRGB to three Fourier coefficients. Wetā FX has used the Fourier sRGB uplift since Avatar: The Way of Water per Peters' blog. https://cg.ivd.kit.edu/compact_spectra.php ; https://momentsingraphics.de/SpectralRendering3Results.html. I found no head-to-head numbers against Jakob-Hanika, and did not check Peters' code licence [U].
- **[R]** Emission uplift: Tódová and Wilkie, "Constrained Spectral Uplifting for HDR Environment Maps", CGF 2025: current practice for image-based emission is to scale reflectance uplifts; their method constrains the result with measured emission spectra. https://onlinelibrary.wiley.com/doi/10.1111/cgf.15280 (abstract page returned 403; from search snippet). Moment-based constrained uplift is Tódová, Wilkie, Fascione, EGSR 2021 [U].
- **[U]** Otsu et al. 2018 (data-driven reflectance from tristimulus values) exists; I did not read it. I found no 2024-2026 paper that replaces Jakob-Hanika for ordinary reflectance; my one search for it returned nothing conclusive. "Picture Perfect RGB Rendering..." is a 2002 paper, not 2025.

### A6. Fluorescence and polarisation

- **[V]** Mitsuba 3 polarised variants (Stokes 4-vector, Mueller 4x4) cost "roughly a 1.5-2X increase in rendering time"; frame rotations need manual care and are the main source of sign bugs. https://mitsuba.readthedocs.io/en/stable/src/key_topics/variants.html ; https://mitsuba.readthedocs.io/en/stable/src/key_topics/polarization.html
- **[R]** Fluorescence = wavelength change at a vertex, sampled from a re-radiation matrix; extends the integral over input wavelength; handle as a special case, not general (slides above).
- **[U]** Inference for design: fluorescence and dispersion need the same mutable wavelength-packet-with-termination primitive in the path state. Polarisation needs a per-wavelength 4-vector and Mueller algebra at every vertex, plus explicit local frames. Frames are needed anyway for anisotropy and tangent-space normals.

### A7. Blender spectral status

- **[R]** Cycles 5.3 dispersion is RGB-based, not spectral (80.lv, which credits Weizhen Huang). https://80.lv/articles/principled-bsdf-in-blender-s-cycles-now-supports-dispersion. The Cycles source passes `inv_abbe = dispersion_scale / abbe_number` to `bsdf_glass_ior`; the per-channel mechanics are in a file I did not read. **[V]** for the former: https://raw.githubusercontent.com/blender/blender/main/intern/cycles/kernel/svm/closure.h
- **[V]** The Cycles developer's weekly log for 2026-07-20: "Tried spectral uplifting, but it gave no clear benefit with dispersion, so it wasn't used." https://projects.blender.org/weizhen/.profile/raw/branch/main/reports/2026.md
- Recon already found no shipped spectral Cycles; I found nothing newer.

### B1. OpenPBR Surface versions

- **[V]** Releases (GitHub API): v1.0 2024-06-04; v1.1 2024-06-28; **v1.1.1 2026-04-17**; **v1.1.2 2026-11-05**. https://api.github.com/repos/AcademySoftwareFoundation/OpenPBR/releases. This resolves the recon conflict: 2024-06-28 is the v1.1 date; the arXiv paper's "current as of August 2025" refers to 1.1.
- **[V]** CHANGELOG 1.1.2: thin-walled subsurface colour was applied twice (fix); scatter-anisotropy UI names renamed to say they are phase-function anisotropy; coat transmittance notation standardised and BRDF layering formulas corrected to use **squared** coat transmittance (#304); length default unit centimetres for transmission depth and subsurface radius; example colours > 1 fixed. 1.1.1: metal edge tint no longer multiplied by specular weight; `geometry_thin_walled` connected to the shader; HDR `emission_color` allowed; thin-film section revised; section on approximations ("implementation flexibility"). 1.1: Zeltner sheen enabled; thin-film IOR default changed; `subsurface_radius_scale` is color3. https://raw.githubusercontent.com/AcademySoftwareFoundation/OpenPBR/main/CHANGELOG.md
- **[R]** v1.2 work exists on a `dev_1.2` branch: `specular_retroreflectivity` (PR #255, merged 2026-02-23) and a "Specular haze" section (commit 87e3ecb). github.com/AcademySoftwareFoundation/OpenPBR/pull/255 (read through a mirror in search results). No 1.2 release date found. The arXiv companion paper lists hazy specular and retro-reflection as planned. https://arxiv.org/html/2512.23696v2
- **[V]** MaterialX: 1.39.0 (2024-07-05) added OpenPBR v1.1; 1.39.5 (2026-05-22) moved to v1.1.1 and added an Airy Fresnel iteration option for thin film; 1.39.6 (release candidate) moved to v1.1.2. No spectral changes. https://raw.githubusercontent.com/AcademySoftwareFoundation/MaterialX/main/CHANGELOG.md. The OpenUSD 25.05 claim in recon is still [U].

### B2. OpenPBR parameters (41 inputs in the MaterialX nodedef) [V]

Source: https://raw.githubusercontent.com/AcademySoftwareFoundation/OpenPBR/main/reference/open_pbr_surface.mtlx and the spec https://academysoftwarefoundation.github.io/OpenPBR/.

| Group | Parameters (default) |
|---|---|
| base | base_weight 1, base_color (0.8)^3, base_diffuse_roughness 0, base_metalness 0 |
| specular | specular_weight 1, specular_color (1)^3, specular_roughness 0.3, specular_roughness_anisotropy 0, specular_ior 1.5 |
| transmission | transmission_weight 0, transmission_color 1^3, transmission_depth 0 (cm), transmission_scatter 0^3, transmission_scatter_anisotropy 0, transmission_dispersion_scale 0, transmission_dispersion_abbe_number 20 |
| subsurface | subsurface_weight 0, subsurface_color 0.8^3, subsurface_radius 1 (cm), subsurface_radius_scale (1, 0.5, 0.25), subsurface_scatter_anisotropy 0 |
| fuzz | fuzz_weight 0, fuzz_color 1^3, fuzz_roughness 0.5 |
| coat | coat_weight 0, coat_color 1^3, coat_roughness 0, coat_roughness_anisotropy 0, coat_ior 1.6, coat_darkening 1 |
| thin film | thin_film_weight 0, thin_film_thickness 0.5 (um), thin_film_ior 1.4 |
| emission | emission_luminance 0 (nits), emission_color 1^3 (HDR allowed) |
| geometry | geometry_opacity 1, geometry_thin_walled false, geometry_normal, geometry_coat_normal, geometry_tangent, geometry_coat_tangent |

### B3. Layering and lobes [V]

- Structure: surface = layer(coated base, fuzz); coated base = layer(base substrate, coat); base substrate = mix(dielectric base, metal, metalness); dielectric base = mix(opaque base, translucent base, transmission_weight); opaque base = mix(glossy-diffuse, subsurface, subsurface_weight); glossy-diffuse = layer(diffuse, gloss); top = mix(ambient, surface, opacity).
- Layering is **albedo scaling**: f = f_coat + T_coat^2 (1 - E_coat) f_sub, with E_coat tabulated or Monte Carlo estimated. Weighted: f = w f_coat + lerp(1, T^2 (1 - E_coat), w) f_sub. Ignores inter-layer multiple bounces. Mixing is a linear blend of BSDFs.
- Reduction to 9 primitive lobes: transparent, coat, emission, conductor, specular reflection, specular transmission, fuzz, subsurface, diffuse. Entering and exiting rays are treated the same, which the spec flags as an approximation.
- Microfacet: GGX, Smith masking, alpha = r^2; anisotropy alpha_t = r^2 sqrt(2/(1+(1-a)^2)), alpha_b = (1-a) alpha_t. Multiple-scattering compensation is required but the method is free (Heitz 2016, Kulla-Conty 2017, Turquin 2019).
- Dielectric: `specular_weight` modulates the IOR ratio (eps = sgn(eta-1) sqrt(xi F0), eta' = (1+eps)/(1-eps)), equal to scaling F0; `specular_color` tints only the first reflection (spec admits non-physical). Metal: F82-tint Schlick, F0 = base_weight base_color, edge tint specular_color, mu_bar = 1/7.
- Diffuse: EON (energy-preserving Oren-Nayar, Fujii form) with analytic compensation; colour defined so albedo = (1 - E_spec) C. Reciprocal Kelemen-style alternative allowed.
- Coat: GGX dielectric, `coat_color` is the square of normal-incidence transmittance; absorption path factor T^(1/mu_i' + 1/mu_o'); darkening Delta = (1-K)/(1-E_b K) blended by `coat_darkening`; base roughened r' = lerp(r, min(1, r^4 + 2 r_c^4)^(1/4), C); TIR fix for coat_ior > specular_ior (Kutz 2021).
- Fuzz: Zeltner et al. 2022 SGGX microflakes via LTC; unit optical thickness, grey transmittance, no Fresnel.
- Subsurface: volumetric (random walk) or BSSRDF; mean free path r = radius x radius_scale per channel (channels stand for ~650/550/450 nm), van de Hulst albedo inversion, HG phase. Translucent base: mu_t = -ln(T)/depth, scatter S/depth, absorption by subtraction (negative components shifted).
- Thin-walled mode: two-sided sheet; subsurface becomes a scattering sheet; translucent becomes absorbing sheet.
- Emission lies under coat and fuzz: effective emission lerp(1, T_coat, C) E.

### B4. What must be spectral [V from the spec; classification mine]

- Spec wording: dispersion "can" use n(lambda) by stochastic wavelength sampling; thin-film fringes depend on wavelength and a spectral renderer "should" evaluate Fresnel amplitude and phase per wavelength (Airy, Belcour-Barla 2017 Eq. 3); the spec never mandates spectral rendering and defines colours in a colour space.
- Natively per-wavelength in our engine: dispersion (Cauchy from Abbe number: n = A + B/l^2, with specular_ior = n_d at 587.6 nm); thin film (arXiv paper: the locally spectral variant converges to grey for thick films and avoids RGB banding, https://arxiv.org/html/2512.23696v2); conductor Fresnel (F0/F82 spectra or complex IOR); coat absorption (Beer exponent applied to an upsampled spectrum); transmission and subsurface coefficients; emission.
- Semantic gap: the spec's formulas (sqrt, -ln T, powers, F82 clamp) are defined on RGB values. Applying them per wavelength to upsampled spectra gives results that agree with an RGB evaluation only to metameric-class tolerance. The correctness target therefore needs a stated tolerance for "spectral vs OpenPBR-RGB" per parameter set.
- **[V]** Adobe's reference BSDF (Apache-2.0, https://github.com/adobe/openpbr-bsdf): RGB by default but `openpbr_prepare()` takes per-channel wavelengths, so spectral use is by design; for dispersion/thin film it recommends one wavelength per channel drawn from camera curves. Targets OpenPBR 1.1.1; 1.2 not implemented. Fixed nested lobe structs (fuzz, coat, base) with common eval/sample/pdf, seven microfacet LUTs plus one LTC LUT, compiles for C++, GLSL, CUDA, MSL, Slang; its CUDA backend "hasn't been used in production". Non-reciprocal for transmission, coat, fuzz by default. Thin film not visible on transmission when thin-walled. `geometry_opacity` left to the host.

### B5. Furnace and energy tests [V]

The spec names the furnace configurations that must vanish under uniform white light: metal (metalness 1, white base and specular colour); dielectric with white specular colour including diffuse, subsurface (weight 1, white colour) and translucent (white colour if depth 0, white scatter if depth > 0); any mix of these with or without coat or fuzz (white colour). It must still pass with thin film, any roughness or anisotropy, volume anisotropy, normal/bump maps, and opacity. https://academysoftwarefoundation.github.io/OpenPBR/ (Energy conservation and white furnace tests).

### B6. Ground truth is weak [V/R]

- **[V]** The MaterialX reference graph does not read `transmission_dispersion_*` (declared, unwired), uses color3 only, has no `conductor_bsdf` (metals via `generalized_schlick_bsdf`), and is RGB. Dispersion is therefore not defined by the reference graph at all.
- **[V]** Blender's own OpenPBR work found: spec wording sometimes ambiguous, offers several options for layering, iridescence and coat darkening; the MaterialX graph does not fully cover the spec; OSLCodeGen cannot translate it fully; Cycles SVM (clean-room from the v1.1.1 text), Cycles OSL (from MaterialX) and Adobe's PBRT-based library give visibly different renders for Car Paint, Coffee, Velvet, Gold, Sapphire, Sand; "But which one is correct?" Community agreed to fix; a validation pipeline (Cycles, Adobe PBRT, Arnold, Karma, VRay, MoonRay and Glimpse WIP) is in progress, test package "planned for September (2026)". Slides at https://hosted-files.sched.co/osd2026/94/Adopting%20Open%20Standards%20Blenders%20Journey%20on%20Supporting%20OpenPBR-Sebastian_Herholz_Francesco_Siddi%281%29.pdf (text extracted locally).
- Consequence: the recon suggestion to use the MaterialX reference graph as ground truth is not safe. Use analytic and furnace oracles first; use other implementations as cross-checks, not truth.

### B7. Blender 5.3 Principled BSDF

- **[V]** The node docs call it "based on the OpenPBR model"; `distribution` is `GGX` or `MULTI_GGX`; `subsurface_method` is `BURLEY`, `RANDOM_WALK`, `RANDOM_WALK_SKIN` (the 5.3 manual also lists a Legacy random walk slated for removal). Local bundled API docs (`bpy.types.ShaderNodeBsdfPrincipled`) and https://docs.blender.org/manual/en/latest/render/shader_nodes/shader/principled.html (titled Blender 5.3 Manual).
- **[V]** Inputs (manual): Base Color, Metallic, Roughness, IOR, Alpha, Thin Wall, Normal, Diffuse Roughness, Subsurface Weight/Radius/Scale/IOR/Anisotropy, Specular IOR Level/Tint/Anisotropic/Anisotropic Rotation/Tangent, Transmission Weight, Dispersion Scale and Abbe Number (new in 5.3), Coat Weight/Roughness/IOR/Tint/Normal, Sheen Weight/Roughness/Tint, Emission Color/Strength, Thin Film Thickness (nm) and IOR (default 1.33).
- **[V]** Cycles source facts (https://raw.githubusercontent.com/blender/blender/main/intern/cycles/kernel/svm/closure.h):
  - Specular IOR Level: `f0 *= 2.0f * specular_ior_level`, eta recomputed from f0; 0.5 is neutral.
  - Metallic uses F82 tint with `min(specular_tint, 1)`; dielectric specular F0 is multiplied by the tint.
  - Non-thin-wall transmission passes `sqrt(clamped base_color)` as tint; thin-wall passes base_color.
  - Subsurface: albedo and BSSRDF colour = clamped base_color; diffuse weight base_color x (1 - subsurface_weight).
  - Dispersion: only the transmission lobe, `inv_abbe = scale / abbe`.
  - Up to 9 closures per Principled: transparent, sheen, coat, emission, metallic, glass (or thin glass), specular, subsurface (or thin), diffuse/Oren-Nayar. Same count as OpenPBR's 9 primitive lobes (by coincidence of structure, not by design guarantee).
- **[R]** Sheen: Blender replaced Velvet by a Sheen BSDF with a "microfiber" model (PR #108869 title, search result) versus OpenPBR's Zeltner LTC. Open PR #166595 "Remove sheen tint from layering albedo" shows sheen layering is still being tuned.

**Mapping Principled 5.3 to OpenPBR** (the engine must ingest both; mapping choices marked):

| Blender | OpenPBR | Note |
|---|---|---|
| Base Color | base_color; also subsurface_color; transmission_color | Blender has one colour for all three. Transmission: Blender gives sqrt(base) per interface crossing; OpenPBR tints each refraction crossing by transmission_color (spec text), so to keep a solid's net colour set transmission_color = sqrt(base_color). Derived from two [V] sources; test it [U]. |
| Metallic | base_metalness | direct |
| Roughness | specular_roughness | also coat roughening rule is in the spec |
| IOR | specular_ior | direct |
| Specular IOR Level L | specular_weight = 2L | both scale F0 linearly (Cycles source; OpenPBR eta' formula) [derived U] |
| Specular Tint | specular_color | dielectric tint and metal edge tint; OpenPBR 1.1.1 removed weight from edge tint |
| Diffuse Roughness | base_diffuse_roughness | Oren-Nayar in Blender vs EON in OpenPBR: different lobe |
| Alpha / Thin Wall | geometry_opacity / geometry_thin_walled | direct |
| Subsurface Weight, Radius, Scale, Anisotropy | subsurface_weight, subsurface_radius, subsurface_radius_scale, subsurface_scatter_anisotropy | units: OpenPBR default cm; Burley method has no OpenPBR equivalent (warn, use random walk) |
| Subsurface IOR | none | Blender-only; warn if not equal to specular IOR |
| Transmission Weight, Dispersion Scale, Abbe | transmission_weight, transmission_dispersion_scale, transmission_dispersion_abbe_number | same names and same scale/abbe rule |
| Coat Weight, Roughness, IOR, Tint | coat_weight, coat_roughness, coat_ior, coat_color | no Blender coat_darkening (OpenPBR default 1); whether Blender's tint is coat_color or its square needs checking [U] |
| Sheen W/R/Tint | fuzz_weight/roughness/color | different model (microfiber vs Zeltner) |
| Emission Color x Strength | emission_color, emission_luminance | Blender is unitless (strength 1 equals colour exactly); OpenPBR is nits. Needs a documented calibration constant |
| Thin Film Thickness (nm), IOR | thin_film_thickness (um) = nm/1000, thin_film_ior; thin_film_weight = 1 if thickness > 0 | defaults differ: 1.33 vs 1.4 |
| Specular Anisotropic, Rotation, Tangent | specular_roughness_anisotropy, geometry_tangent | Cycles' anisotropy-to-alpha mapping vs OpenPBR formula not compared [U] |
| Normal, Coat Normal | geometry_normal, geometry_coat_normal | direct |
| Distribution GGX vs Multiscatter | always compensate | OpenPBR requires multiple-scattering compensation |
| none | base_weight, transmission_depth, transmission_scatter(+aniso), fuzz-free tangent for coat, coat anisotropy | Blender models an interior with a separate Volume output, not in-slab |

### B8. Blender 5.4 OpenPBR status

- **[V]** Task #156437 "OpenPBR Support", milestone 5.4 (due 2027-05-11), via the Gitea API: Blender will add an OpenPBR shading node **as an alternative to the Principled BSDF** (not a replacement); target **OpenPBR v1.1.2**; done: node, Cycles OSL and SVM implementations, EEVEE GLSL, negative-anisotropy subsurface, dispersion, thin-walled mode, anisotropic glass, thin-film thickness unit; open: backend validation, validation against other DCC renderers, USD support, MultiGGX for glass refraction, proper coat darkening, furnace test. MaterialX is "future work". https://projects.blender.org/api/v1/repos/blender/blender/issues/156437 (the HTML page returns 403).
- **[V]** PR #161383 "OpenPBR test package build" is a draft, unmerged, empty description (same API, `/pulls/161383`). Slides above say a test package was planned for September 2026. Blender's 5.4 branch is in alpha (recon: until 2027-02-03).
- **[V]** 5.0 added `Material.inline_shader_nodes()` returning `bpy.types.InlineShaderNodes` (`node_tree`, plus `from_light`, `from_material`, `from_world`); inlines node groups, drops reroute and muted nodes, "aimed at external render engines" (closures, bundles, repeat zones). Keep a reference to the object while reading the tree or Blender crashes. Local bundled API docs and https://developer.blender.org/docs/release_notes/5.0/python_api/
- **[U]** The 5.4 node's Python type name, socket identifiers and unit conventions are not yet public in anything I could open.

## Options and tradeoffs

Real forks only.

**A-1. Wavelengths per path.** 4 (pbrt, cheap state) versus 8 (less colour noise per sample for dispersion/thin film/emission lines, twice the spectral state per path and per BSDF evaluation). pbrt found gains flatten at 6-8 for complex scenes. v1's recon says register pressure from spectral BSDF + NEE was the dominant GPU cost, so state size is the lever.

**A-2. Dispersion strategy.** (i) pbrt: terminate secondaries at the first dispersive refraction (simple, noisy colour past that point, zero cost for non-dispersive materials). (ii) Wilkie hero MIS (balance heuristic) for non-delta dispersive transmission, termination only for delta/smooth glass. More complex pdf bookkeeping; [U] benefit. Dispersion is opt-in per material in OpenPBR (scale 0 = none), so (i) costs nothing for the common case.

**A-3. Spectral domain.** Fixed visible 360-830 (simple, matches CMF tables, pbrt/Mitsuba) versus a per-render band list with an importance CDF (needed for UV/IR/X-ray science outputs; costs one more indirection in sampling and film). The change is local: u-space rotation plus inverse CDF.

**A-4. RGB upsampling for reflectance.** Jakob-Hanika table (BSD-3 tooling, 3 floats per lookup, well-tested in pbrt, per-colour-space table, needs AP1 generated) versus Peters' Fourier sRGB / moment methods (Weta-reported, unknown cost and licence). Choice should be measured (round trip, smoothness, multi-bounce colour, GPU fetch cost).

**A-5. Unit convention.** Energy radiance L_lambda everywhere with an explicit scene scale (photon counts and band integrals at the film), versus photon radiance internally. Energy is what the CMFs and Blender expect; photon conversion is a per-sample factor at the film either way.

**A-6. Polarisation and fluorescence.** Build now (1.5-2x cost for polarisation per Mitsuba; extra state and Mueller algebra everywhere) versus design the extension points only (mutable wavelength packet, explicit frames) and build when a science use case lands. Fluorescence has a smaller footprint (per-vertex wavelength change) than polarisation.

**B-1. Material representation.**
- (a) Fixed OpenPBR record (41 inputs) plus a fixed, hand-written lobe stack. Bounded per-hit state; simplest; matches the spec's "reduction to a mixture of lobes". Cannot express arbitrary Blender closure graphs.
- (b) General closure graph compiled from the node tree (Cycles SVM/OSL style). Expressive; v1 hit lobe-count register spills and a translator that grey-out downstream of textures (recon).
- (c) Hybrid: node tree compiled to small per-texel evaluator programs that only produce an OpenPBR record; the BSDF itself is the fixed stack of (a).
- The evaluator can be an interpreted bytecode (no recompile on edit; slower, divergent) or per-material JIT (fast evaluation; compile latency on every edit, hurts the viewport priority). Cycles itself ships both SVM bytecode on all backends and OSL on CPU/OptiX (slides above).

**B-2. Blender closures outside the record.** Mix Shader of two records can be stochastic selection like pbrt's `MixMaterial` ([V]: picks one child by `amount` vs a hash of (p, wo)); Add Shader and arbitrary closure sums cannot be a single record. Options: map legacy single-lobe nodes onto records, and reject the rest with a visible error (project rule 7), or add a second, general path (large cost).

**B-3. Reference implementation.** Write the lobe stack from the spec (cite-before-implement) versus start from Adobe's Apache-2.0 header library (already CUDA/Slang/C++ capable, 1.1.1, with LUT generation) and verify it against furnace and analytic tests.

**B-4. Spectral semantics of RGB-defined formulas.** Apply the OpenPBR formulas per wavelength on upsampled spectra (physically coherent; deviates from RGB-defined spec numbers by metamerism) versus compute some derived quantities in RGB then upsample (matches spec numbers, loses spectral coherence for absorption). Mixed is possible parameter by parameter.

## Recommendation for Astroray 2.0

1. **Spectral sampling: 4 stratified wavelengths, pbrt-style, with a domain abstraction.** Default domain 360-830 nm with pbrt's sech^2 visible pdf; the sampler is an inverse CDF over `u` with rotation by i/4 in `u`, so a band list for UV/IR/science needs no structural change. Basis: pbrt-v4 `spectrum.h`/`sampling.h` [V] and its 4-vs-8 study [V]. Keep N a compile-time constant and let the spike (below) confirm 4.
2. **Dispersion: pbrt `TerminateSecondary` first; hero MIS only if a measurement shows dispersive-rough noise matters.** Make the wavelength packet (wavelengths + pdf, with a secondary-terminated state) a path-state object from day one; fluorescence later uses the same object. Basis: pbrt `DielectricMaterial` [V], Wilkie MIS [R], OpenPBR dispersion is opt-in [V].
3. **Do not design polarisation in beyond explicit local frames.** Mitsuba reports 1.5-2x time [V]. Revisit when a polarised science output is scheduled; keep BSDF and Fresnel code factored (Fresnel as its own function) so the Mueller form can replace it.
4. **Units and film.** Spectral energy radiance with an explicit scene scale; Planck evaluated with cutoff for float32. Film = list of sensor response curves; default CIE 1931 XYZ then a 3x3 to the Blender working space chosen by `working_space_interop_id`; extra science bands are further response curves and AOVs. Photon-count conversion at the film. Emission for Blender lights and emission colours: `RGBIlluminantSpectrum`-style (scaled sigmoid times the working space's illuminant, D65 or ACES D60), Blender Blackbody and Wavelength nodes map to true Planck and narrow-band spectra. Basis: pbrt [V], Blender 5.0/5.3 API notes [V].
5. **Upsampling: Jakob-Hanika tables for Rec.709, Rec.2020 and AP1 (we generate AP1 and any other working space with the BSD-3 `rgb2spec` optimiser, build step, checked-in hash), three wrappers (albedo, unbounded, illuminant).** Upsample after texture filtering, per shading point, then apply nonlinear material maths per wavelength. Use `Image.colorspace_settings.interop_id` to decide decode: `data` skips upsampling. Spike Peters' Fourier sRGB before locking. Outside 360-830 nm reflectance is clamp-to-edge unless a measured spectrum is attached; warn when a science band is requested on RGB-only material.
6. **Measured data:** tabulated piecewise-linear eta/k and IOR, Cauchy (OpenPBR's own dispersion law); CC0 refractiveindex.info as the source, Ag/Al/Au/Cu from pbrt-style tables, CXRO for X-ray when needed. No Sellmeier parser until a use case needs one.
7. **Material model: option B-1(c).** One OpenPBR record (v1.1.2, pinned) is the only thing the integrator sees. Blender Principled 5.3 and the 5.4 OpenPBR node both lower to it (mapping table in B7); node trees that drive record inputs compile to per-texel evaluator programs (per-texel, not constant-folded; this is v1's missing "pkg219"). The BSDF is a fixed lobe stack following the spec's 9 primitive lobes and albedo scaling with T^2 (1 - E) weights. Mix Shader of records is stochastic selection; unsupported closures raise a visible error. Evaluator form (bytecode vs JIT) is a spike. This keeps per-hit state bounded (record + lobe weights), and puts the whole BSDF in one source compiled for both targets.
8. **Reference implementation: write from the spec, cross-check against Adobe's `openpbr-bsdf` (Apache-2.0) and borrow its LUT approach.** Treat it as a verified-by-test starting point, not truth (it has a "never used in production" CUDA path). Do not use the MaterialX reference graph as ground truth (dispersion unwired, RGB-only, known disagreements).
9. **Tests (fast tier).** (a) The spec's furnace configurations, per lobe and as mixes, for every wavelength in a packet and in RGB output; (b) analytic Fresnel and conductor checks; (c) spectral round trip: white albedo returns white under the working-space illuminant to tight tolerance; saturated colours round trip within the gamut; (d) albedo-table consistency (MC estimate vs table); (e) dispersion: Cauchy n(l) at C/d/F lines reproduces the Abbe number. Merge-gate: cross-check 20+ parameter sets against Adobe's library.
10. **Pin OpenPBR 1.1.2 and track `dev_1.2`** (retroreflectivity, haze). Do not implement 1.2 features until released; the record keeps unused slots out (project rule 2).

## What a measurement spike must settle

All on RTX 5070 Ti, with command and commit recorded; no number is promised here.
1. **N = 4 vs 8**: path-state bytes, registers and occupancy of a fused hit-shade kernel with the full lobe stack, and noise at equal time for dielectric dispersion and thin-film scenes. Settles A-1.
2. **Dispersion noise**: `TerminateSecondary` versus hero MIS on a rough dispersive dielectric; time and variance. Settles A-2.
3. **Upsampling bake-off**: Jakob-Hanika versus Fourier sRGB on round-trip error, smoothness, a two-bounce colour test against a reference spectral computation, GPU fetch cost per hit and per texture sample, table memory for three colour spaces (9.4 MB each, as computed from pbrt's layout). Also AP1 table generation time. Settles A-4.
4. **Record + lobe stack cost**: registers and time of the OpenPBR record evaluation compiled once for CPU and CUDA, with all lobes, and with feature gating; record size in bytes; cost of the 7 microfacet LUTs plus LTC as textures. Include Adobe's header compiled for CUDA as a baseline. Settles B-1 and B-3.
5. **Evaluator form**: bytecode versus JIT for texture-driven records: material edit latency in the viewport and per-hit cost. Settles the B-1(c) sub-choice.
6. **Thin film**: per-wavelength Airy at 4 wavelengths versus Belcour-Barla table, cost and banding.
7. **Wide-domain sampling**: variance and float32 behaviour of Planck and CMF-free band sampling over 3 decades (e.g. 100 nm to 5 um) and an X-ray line source; film accumulation precision in float32 at high spp.
8. **Blender mapping fidelity**: render Principled 5.3 and the mapped OpenPBR record (CPU spectral engine) for a parameter grid; report per-parameter RGB deltas, specifically transmission sqrt, specular level, coat tint, sheen, thin-film units. Cycles is a diagnostic here, not a target.
9. **Cycles dispersion and OpenPBR 5.4 node**: read `bsdf_glass_ior` and the 5.4 OpenPBR node source or socket list once exposed, to fix the mapping table.

## Unverified or conflicting

- All formulas tagged [V] came through a summarising fetch tool, and I did not read every part of the OpenPBR index.html (a stretch between ~100k characters, the start of the thin-film RGB approximation, was not read; the "Parameter reference" tables I took from the MaterialX nodedef instead). Re-read before coding.
- The Wilkie 2014 paper itself was not read (PDF over the fetch cap); the MIS formula is from a student slide deck [R]. Manuka's wavelength count and Mitsuba's count are not confirmed.
- Cycles' dispersion mechanics (`bsdf_glass_ior`) not read; "RGB-based" is from press [R].
- Blender 5.4 OpenPBR: only the Gitea API summary of task #156437 and an unmerged draft PR; no socket names; the HTML pages return 403. The earlier recon claim from a video summary is partly confirmed (an alternative node, not a replacement) and partly corrected (v1.1.2 target, not 1.1.1; Cycles SVM/OSL and EEVEE done).
- OpenPBR 1.2 content from a search-result mirror of PR #255 and commit 87e3ecb [R]; no release date. The arXiv paper v2 text was read only up to ~200k of 277k characters.
- MaterialX supports OpenPBR 1.1 from 1.39.0 [V]; OpenUSD 25.05 support still [U].
- Peters' Fourier sRGB code licence, CIE data redistribution licence, LBNL data notice: not checked. Tódová and Wilkie 2025 only from a snippet. I found no 2023-2026 method that supersedes Jakob-Hanika for reflectance, but one search is not exhaustive.
- Conflict: recon says "use MaterialX reference graph as ground truth"; Blender's own validation finds SVM, OSL and PBRT renders of the same spec disagree. Resolved here in favour of analytic and furnace oracles.
- Conflict: recon says Principled is "already OpenPBR-based" and 5.4 adds a node. Both true: Principled is based on OpenPBR (manual and API docs) but is not the OpenPBR parameter set (see mapping table); the OpenPBR node is an additional node in 5.4.
- Mapping rows marked [U] (transmission sqrt, specular_weight = 2L, coat tint squaring, anisotropy mapping) are derivations to be tested, not facts.

## Verification pass

Independent re-check on 2026-11-14 by a second reader. Method: raw source files fetched through a summarising tool, plus local grep of the MaterialX file and `gh api` for releases. Nothing contradicted the recommendation; no text above needed a factual correction, only the additions below.

| # | Claim | Result | Source |
|---|---|---|---|
| 1a | pbrt-v4 `NSpectrumSamples = 4`, `Lambda_min = 360`, `Lambda_max = 830` | CONFIRMED | https://raw.githubusercontent.com/mmp/pbrt-v4/master/src/pbrt/util/spectrum.h |
| 1b | `SampleVisible` uses `up = u + i/4`, wraps past 1, then maps through the visible inverse CDF; `SampleUniform` steps by (max-min)/4 with wrap | CONFIRMED | spectrum.h above |
| 1c | Visible pdf `0.0039398042 / cosh^2(0.0072 (l-538))` on [360,830]; sampler `538 - 138.888889 atanh(0.85691062 - 1.82750197 u)` | CONFIRMED (constants match exactly) | https://raw.githubusercontent.com/mmp/pbrt-v4/master/src/pbrt/util/sampling.h |
| 1d | `TerminateSecondary` zeroes pdf[1..3] and divides pdf[0] by NSpectrumSamples; Dielectric calls it only if `eta` is not `ConstantSpectrum`, using `eta(lambda[0])` | CONFIRMED | spectrum.h; https://raw.githubusercontent.com/mmp/pbrt-v4/master/src/pbrt/materials.h |
| 2a | OpenPBR releases v1.1.2 2026-11-05, v1.1.1 2026-04-17, v1.1 2024-06-28, v1.0 2024-06-04 | CONFIRMED (read via `gh api repos/AcademySoftwareFoundation/OpenPBR/releases`; the raw WebFetch of the API URL returns 403) | https://api.github.com/repos/AcademySoftwareFoundation/OpenPBR/releases |
| 2b | Spec page itself is headed "v1.1.2, 2026-10-28" (document date; the release tag is 2026-11-05). Not a conflict | CONFIRMED, detail added | https://academysoftwarefoundation.github.io/OpenPBR/ |
| 2c | Layering `f = f_coat + T_coat^2 (1 - E_coat) f_sub` (squared, both rays) | CONFIRMED | spec page above |
| 2d | Nine primitive lobes: transparency, coat, emission, metal, specular reflection, specular transmission, fuzz, subsurface, diffuse | CONFIRMED | spec page above |
| 2e | White-furnace configurations (metal, dielectric incl. diffuse/subsurface/translucent, mixes, with coat and fuzz, robust to thin film, roughness, anisotropy, volume anisotropy, bump, opacity) | CONFIRMED. The finer parentheses on translucent depth/scatter colour were not re-read | spec page above |
| 2f | 41 MaterialX inputs | CONFIRMED (41 inputs in `ND_open_pbr_surface_surfaceshader`, version="1.1.2"; a raw grep returns 43 only because it also counts the 2 inputs of the helper `open_pbr_anisotropy` nodedef) | https://raw.githubusercontent.com/AcademySoftwareFoundation/OpenPBR/main/reference/open_pbr_surface.mtlx |
| 2g | MaterialX 1.39.0 (2024-07-05) added OpenPBR v1.1; 1.39.5 (2026-05-22) moved to v1.1.1; 1.39.6 RC moved to v1.1.2 | CONFIRMED (1.39.1, 2024-09-03, also added Standard Surface <-> OpenPBR translation graphs) | https://raw.githubusercontent.com/AcademySoftwareFoundation/MaterialX/main/CHANGELOG.md |
| 3a | `transmission_dispersion_scale` and `_abbe_number` are declared but not wired in the reference graph (they appear only in the nodedef, lines 36 and 38); no `conductor_bsdf` in the file; colour inputs are color3 only | CONFIRMED (local grep: 0 `conductor_bsdf`, no wavelength-dependent construct) | mtlx file above |
| 3b | Blender OSD 2026 slides: Cycles SVM, OSL and Adobe PBRT disagree, "which one is correct?" | COULD NOT VERIFY (slides PDF not re-fetched; taken from the original author) | https://hosted-files.sched.co/osd2026/94/ (as cited in B6) |
| 4a | Task #156437, milestone 5.4: OpenPBR node is an alternative to Principled; targets v1.1.2; done: node, Cycles OSL and SVM, EEVEE GLSL, negative-anisotropy subsurface, dispersion, thin-walled, anisotropic glass, thin-film unit; open: backend validation, validation against other DCCs, USD, MultiGGX glass, proper coat darkening, furnace test (!158426) | CONFIRMED. The Gitea API reply was fetched successfully this time, so it is no longer only the HTML page that fails. The reply is still summarised, so socket names remain unknown | https://projects.blender.org/api/v1/repos/blender/blender/issues/156437 |
| 4b | Cycles Principled: `f0 *= 2.0f * specular_ior_level` (only when level != 0.5), non-thin-wall transmission passes `sqrt(clamped base_color)`, `inv_abbe = safe_divide(dispersion_scale, abbe_number)` fed to `bsdf_glass_ior` | CONFIRMED | https://raw.githubusercontent.com/blender/blender/main/intern/cycles/kernel/svm/closure.h |
| 5a | Blender 5.0 `bpy.data.colorspace.working_space_interop_id` returns `lin_rec709_scene` / `lin_rec2020_scene` / `lin_ap1_scene` (ACEScg). The page names these three as common values and does not list every option | CONFIRMED (three-way list inferred from the ids; full option list not on the page) | https://developer.blender.org/docs/release_notes/5.0/python_api/ |
| 5b | pbrt ships tables for sRGB, DCI-P3, Rec2020, ACES2065-1; `float[3][64][64][64][3]`; no AP1 mentioned | CONFIRMED (first 100k of 102k characters of the page read; 9.4 MB per space is our arithmetic: 3*64^3*3*4 B = 9,437,184 B) | https://pbr-book.org/4ed/Radiometry,_Spectra,_and_Color/Color |
| 5c | rgb2spec is BSD-3-Clause | COULD NOT VERIFY this pass (not re-fetched; the licence URL in A5 stands as the earlier reader's) | https://raw.githubusercontent.com/mitsuba-renderer/rgb2spec/master/LICENSE |

Not re-checked (still as tagged above): Wilkie 2014 MIS formula [R]; Manuka and Mitsuba wavelength counts [U]; Cycles dispersion being RGB-based in effect [R] (the source line only shows `inv_abbe` passed to `bsdf_glass_ior`); OpenPBR 1.2 content [R]; Peters Fourier sRGB licence; Tódová and Wilkie 2025.

Recommendation: unchanged. The 4-wavelength pbrt-style sampler, TerminateSecondary dispersion, Jakob-Hanika tables with a generated AP1 table, OpenPBR v1.1.2 as the single record, and analytic/furnace oracles instead of the MaterialX graph as truth all still hold.
