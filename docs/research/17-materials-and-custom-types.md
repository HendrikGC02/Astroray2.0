# Materials, emitters, media and instruments: the full catalogue

Status: research input for `ARCHITECTURE.md` and `ROADMAP.md`. Nothing here is decided until a decision
record exists. Sources: inventories of v1's 332 work-package specs, the owner's own past messages, v1's
code and design documents (all relayed to this author; the v1 repository was not opened), and four
literature surveys (wave-optics surfaces; wavelength coupling and polarisation; measured and complex
appearance; emitters, media and instruments). The companion checklist of every v1 capability is
`docs/v1-lessons/feature-inventory.md`.

## What the owner asked for

Quotes below are the owner's own typed words as the inventories report them. Dates are approximate
where the transcript search could not pin them.

| Owner's words | Wish expressed |
|---|---|
| "is all the custom materials and types that I want to exist. In the early planning days we went through many of them, but they include things like diffraction gratings, and other advanced material types." (2026-11-14) | Custom and advanced material types, diffraction gratings named first, must not be lost in the rewrite. |
| "eventually there are many more materials (and material types) that I will want to add. Things like diffraction gratings, thin films, thin translucents like in the new blender version, astrophysical materials and more." (about 2026-09/10) | An open-ended material family: gratings, thin films, thin translucent sheets, astrophysical materials. |
| "...the scientific features of [astronomy] such as, you know, diffraction gratings, nebulae emission, line emission, line absorption, loading astrophysical data, fixed files, telescope data, point spread functions..." (about 2026-08-29; "asteroid" in the transcript is a transcription error) | The science feature set: gratings, nebula and line emission, line absorption, data loading, telescope data, PSFs. ("fixed files" is probably FITS files; unconfirmed.) |
| "in the future we will need plenty more [spectral profiles] and probably will need one for pretty much every element and or some molecules, which can then be combined and scaled to create complex emission and absorption spectra... currently the output of astroray is just an rgb image, but it has the theoretical capability of outputting complex data cubes that contain all of the spectral (and depth, normal, ray count, ray depth etc.) information" (about 2026-09-15) | An element and molecule spectrum library that combines into emission and absorption spectra; spectral data-cube output with diagnostic channels. |
| "By default when a light is added to the scene it should be a blackbody spectral emitter. Changing the color introduces 'color filters' that modify the blackbody SED, and there is a box to change the color temperature too, with a button available to switch to RGB upsampling. Preset buttons for fluorescent lamp lights and LED SEDs would also be good." (2026-05-14, quoted in a v1 spec) | Spectral lights by default: blackbody plus colour filter, temperature box, RGB toggle, lamp presets. |
| "I have a separate project that creates physically based vdb's of clouds of all types with the purpose of using them in realistic renders. It would be nice if Astroray had a dedicated material specifically designed/tuned for these cloud volumes, in a spectral kind of way. this is just something for the horizon." | A spectral cloud material for the owner's own VDB clouds; explicitly a horizon item. |
| "Volumes must most certainly be in this first usable release. Every aspect of the renderer should really be." (about 2026-09-23) | Volumes in the first release (also owner decision 7). |
| "I wondered if the sky model in Astroray will include a sun disc in the future, and also if the light it emits is spectral and if it includes infrared and UV?" and "sun disc and absolute exposure should probably be done sooner than later. The per-wavelength sky will be needed in the future for truly scientifically accurate rendering of spectral data, but that is more of a stretch goal." | Sun disc and absolute exposure early; a per-wavelength sky with IR and UV later. |
| "make own spectral plan for black body emission, on the IES one: do which ever is best and physically accurate" | Spectral blackbody; IES done the physically accurate way. |
| "Physically correct always, especially when it improves on Cycles." | Physics wins over Cycles look. |
| "I really thought the GPU dispersive caustics were either already tackled or specced, if not then yeah add it to the roadmap, but like its not neccessarily a #1 priority." | GPU dispersive caustics on the roadmap, not top priority. |
| "Astroray is still not equipped to handle complex shader node setups, that to me is one of the larger hurdles... Astroray specific features and things need dedicated interfaces that are intuitive to use and fits in with the rest of Blenders interface." | Complex node graphs must work; Astroray features need native-feeling Blender UI. |

Two further items are partly agent-authored and are recorded as such: a request to make conductor thin
film "per-λ-native to match Cycles' saturation" (anodised and oil-on-metal looks), and a plan text
saying "future plugin materials (diffraction gratings, thin films, thin translucents, astrophysical) add
a kernel instead of inflating a shared switch" (paraphrase of the owner). Under owner decision 1, thin
film is now judged against per-wavelength Airy reflectance, not Cycles saturation.

**Gap.** The list the owner refers to from "the early planning days" was not found. The transcript search
did not reach the earliest sessions, and no v1 spec designs a diffraction grating (two specs name
gratings only as deferred: "deferred until a science case names them"). The catalogue below is therefore
the union of the owner's quotes, v1's specs and code, and the literature; it may still miss items the
owner discussed. See open question 1.

## Catalogue

Origin: **O** owner wish, **B** v1 built (fully or partly), **P** v1 planned and not built, **R** new from
research. Difficulty S/M/L is the surveys' or this author's estimate, not measured. Tiers:
**first** = first usable release (owner decision 7: surfaces, volumes, sky and world, textures, lights);
**second** = the next milestone, which this document assumes contains the first astrophysics target (black
hole inside an emission nebula, owner decision 8; see open question 3); **later**; **out** = out of scope
for 2.0, with reason. "Record" means the pinned OpenPBR 1.1.2 parameter record; "ext" means the
extension-lobe mechanism and "special stage" the separate shade stage, both defined in Architectural
consequence.

### 1. Surface materials inside OpenPBR

| Item | Origin | Physical model and citation | Needs from the core | Diff. | Tier |
|---|---|---|---|---|---|
| Diffuse with roughness | B | EON energy-preserving Oren-Nayar (Portsmouth, Kutz, Hill 2025, JCGT), as OpenPBR specifies; Oren-Nayar 1994 | record | S | first |
| Metal, F82-tint | B | OpenPBR F82-tint Schlick; Gulbrandsen 2014; Kutz, Hasan, Edmondson 2021 | record | S | first |
| Dielectric, smooth and rough, with multiple-scattering compensation | B | Walter et al. 2007; Kulla and Conty 2017; Turquin 2019; Heitz et al. 2016 random walk (DOI 10.1145/2897824.2925943) | record, albedo tables | M | first |
| Coat with tint and darkening | B | OpenPBR coat; TIR fix (Kutz 2021) | record | S | first |
| Fuzz / sheen | B | Zeltner et al. 2022 LTC (code github.com/tizian/ltc-sheen); Estevez and Kulla 2017 | record, LTC table | S | first |
| Single thin film (soap bubble, oil, anodised metal) | O, B | Belcour and Barla 2017, https://belcour.github.io/blog/research/publication/2017/05/01/brdf-thin-film.html; Airy evaluated per wavelength, no RGB fit | record (`thin_film_*`), per-wavelength Fresnel | S | first |
| Thin translucent sheet (thin wall) | O, B (v1 thin wall unverified) | OpenPBR `geometry_thin_walled` | record | S | first |
| Subsurface, random walk | B (v1 CPU prototype) | OpenPBR subsurface; van de Hulst albedo inversion; HG phase | record, own stage, intersect-within-object query | M | first |
| Transmission with interior absorption and scatter | R | OpenPBR `transmission_depth`, `transmission_scatter` | record, medium stack | M | first |
| Dispersion (Abbe number, Cauchy) | B | OpenPBR dispersion; Cauchy n = A + B/λ² | record, secondary-wavelength termination | S | first |
| Emission surface | B | OpenPBR emission (nits) with documented Blender calibration constant | record | S | first |
| Anisotropic specular | B | OpenPBR anisotropy mapping | record, tangent frame | S | first |
| Opacity, cut-outs, transparent shadows | B | OpenPBR `geometry_opacity` | record, shadow stage | S | first |
| Legacy Blender closures (Diffuse, Glossy, Glass, Refraction, Translucent, Transparent, Metallic, Sheen) as presets | B (partial) | mapping onto the record | VM lowering | S | first |
| Mix Shader of two materials | B | stochastic selection, pbrt `MixMaterial` | VM, per-hit hash | S | first |
| Wet surface preset | R | Jensen, Legakis, Dorsey 1999 (water coat, IOR 1.33, roughness reduction, diffuse darkening) | record preset | S | second |
| Specular retroreflectivity, specular haze | R | OpenPBR `dev_1.2` (PR #255); not released | record slots when 1.2 ships | S | later (wait for release) |

### 2. Spectral inputs to OpenPBR parameters

Each of these fills a colour or IOR slot of the record with a spectrum instead of an upsampled RGB value.

| Item | Origin | Physical model and citation | Needs from the core | Diff. | Tier |
|---|---|---|---|---|---|
| RGB upsampled per working space | B | Jakob and Hanika 2019 (DOI 10.1111/cgf.13626), tables for Rec.709, Rec.2020, AP1 | spectral slot, source "rgb" | S | first |
| Measured reflectance library (vegetation, soils, building, metals, fabrics, paints, skin, 300-2500 nm) | B | USGS splib07 (Kokaly 2017, doi 10.5066/F7RR1WDJ); ECOSTRESS (Meerdink 2019, RSE 230:111196); Rakic 1998; Bashkatov 2005 | source "table" | S | first |
| Drawn spectrum (curve) | B | piecewise linear (pbrt-v4 `PiecewiseLinearSpectrum` design) | source "table" | S | first |
| Complex-IOR conductor (Blender Metallic "Physical Conductor") | P | exact conductor Fresnel from n(λ), k(λ); data refractiveindex.info (CC0, github.com/polyanskiy/refractiveindex.info-database), Rakic 1998 | two table slots, one-bit Fresnel mode in the metal lobe | S | first |
| Sellmeier and tabulated n(λ) glass (BK7, fused silica, SF11, diamond, manual B/C) | B | Sellmeier equation; Schott data; refractiveindex.info | source "analytic" on `specular_ior` | S | first |
| Spectral absorption σa(λ) for glass, water, gems, coat | R, P | Beer-Lambert; Pope and Fry 1997, Hale and Querry 1973 (water); Guy and Soler 2004 (gems); RRUFF spectra | spectral slot on transmission and coat colour | S | first |
| IR/UV band behaviour of materials | B | measured tables extended past the visible; clamp-to-edge plus warning for RGB-only materials | wavelength domain | S | first |
| Spectrum math, Gaussian/bandpass, SPD file import | P | host-side composition into one table (Indigo "peak" model for Gaussian) | none on device | S | second |
| Spectral melanin (hair) | B | eumelanin σa ∝ λ^-3.33 (Jacques 2013, OMLC), pheomelanin ∝ λ^-4.75 (Donner and Jensen 2006) | source "analytic" | S | second |
| Skin chromophores (melanin, haemoglobin, oxygenation) to σa, σs | R | Donner and Jensen 2006 (EGSR); Jimenez et al. 2010; Aliaga et al. 2023 (title unverified); data omlc.org | spectral slots on subsurface | M | later |

### 3. Extension surface lobes beyond OpenPBR

| Item | Origin | Physical model and citation | Needs from the core | Diff. | Tier |
|---|---|---|---|---|---|
| **Diffraction grating** (reflective and transmissive, blazed, CD/DVD) | O | grating equation sin θm = sin θi + mλ/d; Stam 1999 "Diffraction Shaders"; Toisoul and Ghosh 2017 (DOI 10.1145/3012001); PLTFalcor https://github.com/ssteinberg/PLTFalcor (CC BY-NC 4.0, archived 2025: study only) | ext kind; groove tangent frame; per-order efficiency table (λ × incidence); wavelength-dependent direction with secondary termination; roughness-broadened orders for NEE and MIS | M | first (proof of the mechanism; open question 2) |
| Echelle grating (high orders, free spectral range overlap) | O (Pillar 4), R | same, high-order efficiency data | as above | L | later |
| Multilayer stack: dichroic, AR coating, Bragg mirror | R | transfer-matrix method per wavelength, s/p split averaged; Belcour and Barla 2017 for one layer | ext kind; layer-stack table of (n(λ), k(λ), d) | M | second |
| Measured BSDF | R (v1 non-goal) | Dupuy and Jakob 2018 (rgl.epfl.ch/materials, spectral, adaptive warp); Matusik et al. 2003 MERL (RGB, non-commercial); Mitsuba 3 `measured` plugin (BSD-3); Nielsen 2015 sampling | ext kind (full replacement); 4D table with warp sampler | M | second |
| Hair and fur | B | Chiang et al. 2016; d'Eon 2011; Marschner 2003; pbrt-v4 `HairBxDF` (Apache-2.0), pbrt-v3 (BSD-2) | ext kind (full replacement); curve primitive with tangent and fibre offset h | M | second |
| Retroreflective sheeting | R | GGX-like lobe about -wi with entrance-angle term, calibrated to ASTM E810 RA tables; Guo et al. 2018 cited by request but unidentified | ext kind, RA table | S-M | later (prefer OpenPBR 1.2 retroreflectivity) |
| Planetary regolith (lunar, dusty) | R | Hapke 1981 (JGR 86(B4)); Hapke 2012 book; Lommel-Seeliger, lunar-Lambert (McEwen 1991); w(λ) from RELAB or USGS; Mitsuba 0.6 `hapke` (licence unconfirmed) | ext kind replacing diffuse; spectral albedo slot | S-M | later |
| Flakes and glints (car paint, snow sparkle) | R | Jakob et al. 2014 discrete stochastic microfacets; Yan et al. 2014 (https://cseweb.ucsd.edu/~ravir/glints2014.pdf), 2016; Chermain et al. 2020; Deliot and Belcour 2023 (title unverified) | ext kind; ray footprint; stable per-position hash | L | later |
| Pearlescent pigments | R | Guillen et al. 2020, https://rgl.epfl.ch/publications/Guillen2020Pearlescent; practical form: thin-film flakes in an orientation NDF | ext kind reusing thin-film Fresnel | M | later |
| Scratch iridescence | R | Werner, Velinov, Jakob, Hullin 2017, https://arxiv.org/pdf/1705.06086 (supplement https://light.informatik.uni-bonn.de/?p=346); real-time follow-up Velinov et al. 2018, https://light.informatik.uni-bonn.de/?p=373 | ext kind with baked table; ray footprint | L | later |
| Holographic foil | R | tabulated angle × λ lobe, Toisoul-Ghosh style | ext kind, spatially varying table | M | later |
| General layered BSDF | R | Guo, Hasan, Zhao 2018; Belcour 2018; Jakob et al. 2014 layerlab (github.com/wjakob/layerlab, BSD); Weidlich and Wilkie 2007 | ext kind, stochastic pdf flag | L | later (or bake to measured) |
| Toon, Ray Portal (Blender nodes) | R | Blender semantics | ext kind | S | later |
| Wave-optics glints | R | Yan, Hasan, Walter, Marschner, Ramamoorthi 2018 (ToG 37(4) art. 75); prefiltering https://arxiv.org/pdf/2109.14807 | per-pixel coherent patch integrals | L | out: research-level, no reference code found |
| Morpho butterfly, opal (photonic crystals) | R | no model found | none | L | out: no source; use a measured or tabulated BSDF |
| Yarn-level cloth | R | Irawan and Marschner 2012 | weave UV | L | out: fuzz covers the common case |
| Phong | B | not physical | none | S | out: map to Glossy preset |

### 4. Wavelength-coupling and polarisation materials

| Item | Origin | Physical model and citation | Needs from the core | Diff. | Tier |
|---|---|---|---|---|---|
| Fluorescence, optical brighteners | R (v1 explicit non-goal) | Mojzík, Fichet, Wilkie 2018 (EGSR, title unconfirmed); Jung et al. 2019 CGF 38(4), https://cg.ivd.kit.edu/uplifting.php; Jung et al. 2018 BBRRDF; Hua et al. 2023 CGF 42(1); measured matrices Hullin et al. 2010 | ext kind; transport wavelengths stored and rewritable; film wavelength kept separate; domain to about 300 nm | M | second |
| Doppler and gravitational wavelength shift | B | λ_obs = λ_emit/g, radiance by g³ per frequency (Rybicki and Lightman 1979) | transport wavelengths rewritable per vertex | M | second (astro) |
| Polarisation: polarisers, Brewster, polarised sky | R | Stokes and Mueller with frame rotators, Mitsuba 3 (https://mitsuba.readthedocs.io/en/stable/src/key_topics/polarization.html, 1.5-2x time per Mitsuba's variant docs); Wolff and Kurlander 1990; Mojzík et al. 2016 | whole-build compile-time variant of the spectrum type; Stokes film | L | later |
| Birefringence (calcite) | R | Weidlich and Wilkie 2008 | optic axis, IOR tensor, o/e split pdf; unpolarised fallback possible | M-L | later |
| Pleochroic gemstones | R | absorption tensor along optic axis; Guy and Soler 2004 | crystal axis, directional σa | M | later |
| Photoelasticity | R | retarder Mueller matrices | polarised variant | M | later |
| Interference band-pass filter angle shift | R | λc(θ) ≈ λ0 sqrt(1 - sin²θ/neff²) (formula unverified) | thin-film hook on a filter | S-M | later |
| Phosphorescence | R | decay exp(-t/τ); transient rendering (Jarabo et al. 2014) | scene time, persistent per-surface buffer | L | later (reserve scene time only) |
| Raman scattering | R | signal 1e-3 to 1e-6 of elastic | none | - | out: negligible for rendering |
| Full physical light transport with coherence | R | Steinberg and Yan 2021, https://sites.cs.ucsb.edu/~lingqi/publications/paper_gplt.pdf | generalised-ray state | L | out: research-level |

### 5. Emitters

| Item | Origin | Physical model and citation | Needs from the core | Diff. | Tier |
|---|---|---|---|---|---|
| Point, spot, sun, area lights with calibrated energy | B | Cycles light conventions as semantics; pbrt-v4 lights | light sampler | S | first |
| Blackbody default light with colour filter, temperature, RGB toggle | O, B | Planck; pbrt-v4 `BlackbodySpectrum` (Apache-2.0); photopic normalisation, K = 683 lm/W | emission spectrum source "analytic", filter product cached at set time | S | first |
| Lamp presets: CIE F2, F3, LED 3000/5000/6500 K, sodium, mercury | O, B | CIE 15:2004 and 15:2018; CIE 224:2017; NIST ASD lines; colour-science (BSD-3) | source "table"; line-aware wavelength sampling | S | first |
| Laser / narrow line emitter | B | delta or narrow Gaussian line; speckle ignored (Goodman) | line-aware wavelength sampling | S | first |
| Line-aware hero wavelength sampling | B (partial), R | pick a line by power, sample its profile; pdf is the mixture; Wilkie et al. 2014 | scene-built wavelength CDF | M | first |
| Emissive meshes unified with lights | P | one sampling contract (pbrt-v4 `DiffuseAreaLight`) | light sampler | M | first |
| IES profiles (LM-63) on point and spot | B | ANSI/IES LM-63; lumens to watts through the emitter SPD | angular table on light | S | first |
| HDRI environment with importance sampling | B | pbrt-v4 s12.5 | env sampler | S | first |
| Sky with sun disc (Blender Sky node) | O, B | Nishita et al. 1993, evaluated per wavelength; sun disc S = L_disc Ω | sky stage | M | first |
| Research-grade atmosphere (ozone, altitude, UV and IR) | O (stretch) | Bruneton and Neyret 2008, Bruneton 2017 (BSD-3); Hillaire 2020 (MIT code); Hošek-Wilkie 2012 | atmosphere module, wide domain, planet-scale precision | L | second |
| Gas-discharge and element line emission | O, P | NIST ASD (Kramida et al.); LTE Voigt (scipy `voigt_profile`, Faddeeva package MIT, Humlíček 1982); Ciddor 1996 / Morton 2000 air-vacuum | host-side bake to table plus line list | M | second |
| Absolute photometric units and exposure | O | radiance in W/sr/m²/nm with global exposure | unit contract | S | first |
| Goniometric, projection, portal lights | P | pbrt-v4 | light types | S-M | later |
| Synchrotron and free-free emission (Stokes, Faraday) | B (unpolarised) | Rybicki and Lightman 1979; Pandya et al. 2016; ipole, grtrans, RAPTOR (licences unverified) | wide frequency domain, g factor; Stokes for polarised | L | later (unpolarised with astro objects) |

### 6. Participating media

| Item | Origin | Physical model and citation | Needs from the core | Diff. | Tier |
|---|---|---|---|---|---|
| Homogeneous media, world fog, HG phase | B | Henyey and Greenstein 1941; pbrt-v4 volpath | medium stage | S | first |
| Heterogeneous grids (Blender Volume, OpenVDB) | O, B | NanoVDB (Museth 2021); Woodcock 1965; Novák, Selle, Jarosz 2014; majorant grid and DDA (pbrt-v4 s11.4.2) | medium stage, grids | M | first |
| Chromatic σ with spectral MIS | B | Kutz et al. 2017; Miller, Georgiev, Jarosz 2019; pbrt volpath r_u/r_l | 4-wide spectra in path state | M | first |
| Principled Volume incl. blackbody emission | B | Planck per wavelength; Cycles node semantics | spectral slots in medium record | M | first |
| Mesh-bounded, nested, camera-inside volumes | B (partial) | stateless inside test; Woop, Benthin, Wald 2013 | medium stack | M | first |
| Volume NEE, equiangular sampling | B | Kulla and Fajardo 2012 | medium stage | M | first |
| Rayleigh scattering | R | σ ∝ λ^-4 with King factor (Bucholtz 1995) | phase kind | S | second |
| Lorenz-Mie phase tables (fog, cloud, rainbows, glories, fogbows) | P, R | Frisvad, Christensen, Jensen 2007; Bohren and Huffman 1983; Wiscombe 1980; miepython (MIT), PyMieScatt (BSD-3); Jendersie and d'Eon 2023 approximation; Sadeghi et al. 2012 | phase kind "table" (θ × λ) | M | second |
| Dust extinction and scattering | P | Cardelli-Clayton-Mathis 1989; Fitzpatrick 1999; Gordon 2003; Weingartner and Draine 2001; Draine 2003; `dust_extinction` (BSD-3) | spectral σ slots, wide domain | S | second |
| Emission nebula, line emission and absorption in volumes | O, P | Osterbrock and Ferland 2006; Storey and Hummer 1995; Case-B Hα/Hβ = 2.86; Cloudy/MAPPINGS offline only (licence unverified) | line-aware sampling, emission-dominated medium stage | M | second (astro) |
| Cloud material (LWC/IWC, effective radius, ice habits) | O (horizon) | Yang et al. 2013 (Zenodo 5348402, CC-BY-4.0); Baum et al. 2014; Warren and Brandt 2008; Hale and Querry 1973; Segelstein 1981 | Mie and ice phase tables | L | later |
| Ice halos (oriented crystals) | R | oriented hexagonal crystal scattering | geometry or table | L | later |
| Volume motion blur from velocity grid | P | advect by velocity times shutter time | time per path | M | later |

### 7. Sensors and instruments

| Item | Origin | Physical model and citation | Needs from the core | Diff. | Tier |
|---|---|---|---|---|---|
| CIE 1931 2° observer to the working space | B | CIE 1931; CIE 15:2004 | film | S | first |
| Diagnostic AOVs: sample and bounce heat maps, ray count, variance | O (decision 15), B | none needed | film channels | S | first |
| Wavelength bands (NIR, UV, custom) and false-colour maps | B | band CDF; colourmaps grayscale, inferno, viridis, hot, IR false colour | configurable domain | S | second |
| Spectral data cube output (FITS, multi-layer EXR) | O, P | FITS 4.0 with spectral WCS (Greisen et al. 2006, A&A 446, 747); cfitsio, astropy; Mitsuba `specfilm` | N-band film | M | second |
| Filter × QE response channels; selectable observer; measured camera curves | P, R | CIE 2006 LMS (cvrl.org); Jiang et al. 2013 (WACV); SVO Filter Profile Service; synphot (BSD-3) | film response list, response-weighted λ sampling | S | second |
| Absolute radiance and photon counts | P | N = ∫ L R λ/(hc) A Ω t dλ; Van Oosterom and Strackee 1983 pixel solid angle | unit contract end to end | M | second |
| PSF convolution per band, diffraction spikes | O | PSF = |FT(pupil)|²; WebbPSF/STPSF (BSD-3); Tiny Tim | post-process before tone map | M | later |
| Detector noise (Poisson, read, dark, gain, ADU) | O, P | EMVA 1288; Healey and Kondepudy 1994 | absolute units | S | later |
| Spectrograph and slit | R | post-process on the cube; PyEchelle (MIT) | cube output | M | later |

### 8. Astrophysical objects

| Item | Origin | Physical model and citation | Needs from the core | Diff. | Tier |
|---|---|---|---|---|---|
| Schwarzschild and Kerr black holes | B | Bardeen, Press, Teukolsky 1972; Bardeen 1973; ipole (BSD-3) cross-check; GYOTO as external oracle | spacetime policy, geodesic segment stage, double CPU reference | L | second (astro target) |
| Emission nebula around the black hole | O | see media table | emission-absorption along curved rays | M | second (astro target) |
| Thin Novikov-Thorne disk with invariant transfer | B | Page and Thorne 1974; Cunningham 1975; Rybicki and Lightman 1979 s4.2 | g factor, blackbody | M | second |
| FITS image and cube import | O, B (image only) | FITS 4.0; cfitsio (public domain) | texture and grid input | S | second |
| Slim disk, ADAF, synchrotron jet | B | Sadowski 2009; Narayan and Yi 1995, Yuan and Narayan 2014; Pandya et al. 2016, Lind and Blandford 1985 | volumetric emitters, wide domain | M each | later |
| HDF5, yt, npy and SPH ingestion | P | h5py, yt (BSD-3); Wendland C4 (Dehnen and Aly 2012) | host-side conversion to grids | M | later |
| HMXB Roche geometry | P | Eggleton 1983; Kopal potential | analytic shapes | M | later |
| Weak and strong lensing | P | Schneider, Ehlers, Falco 1992; Narayan and Bartelmann 1996 | screen pass or metric | M | later |

**Counts by tier (rows above):** first 41, second 22, later 29, out 6; 98 rows in total.

## Architectural consequence

**Verdict.** "One fixed OpenPBR record" survives as the only thing the *common* shade path sees. It does
not survive as the only material representation. Four bounded additions, each a closed set, let gratings,
fluorescence, measured BSDFs, hair and astrophysical emitters exist without a fat shared struct, a
template matrix or a second implementation.

**1. Resolved spectral slots.** Every colour and IOR input of the record (base, specular, transmission,
subsurface, coat and emission colours, `specular_ior`, transmission scatter) is filled by the material
evaluation stage with **N per-wavelength values at the path's current transport wavelengths**, not RGB.
The evaluator resolves a small closed set of spectrum sources: `rgb` (Jakob-Hanika coefficients in the
file's working space, after texture filtering), `table` (index into a scene spectrum table, piecewise
linear), `analytic` (blackbody T, Cauchy/Abbe, Sellmeier, Gaussian line). The BSDF stage never sees RGB
and never branches on source type. Complex-IOR conductors reuse the two metal slots (n in the base slot,
k in the specular slot) plus one Fresnel-mode bit in the metal lobe, so the record does not grow. OpenPBR's
RGB-defined formulas are applied per wavelength, as research file 05 (B4) already proposes. Cost when
unused: with N = 4, one extra float per colour slot over RGB, about 8 slots, so about +32 B on the record
handed from stage 1 to stage 2 (S3 measures the traffic).

**2. One extension lobe per material, from a closed enum, in a side table.** The record gains `ext_kind`
(u8) and `ext_index` (u24). Parameters live in a side array indexed by material, not in the record (v1
kept its material struct at a fixed size the same way: per-material texture ids and texture bindings in
side tables). Each kind declares which primitive OpenPBR lobe it **replaces**: a grating replaces the
specular reflection (or conductor) lobe, fluorescence and Hapke replace diffuse, a multilayer stack
replaces the interface Fresnel, and measured BSDFs and hair replace the whole stack except emission and
opacity. Replacement, not addition, keeps OpenPBR's albedo-scaling layering valid: every kind must
provide eval, sample, pdf and its directional albedo E(wo) so coat and fuzz still layer on top. The
parameter block has a fixed size (set by spike measurement); anything larger is a table handle. Sample
results carry flags: delta, wavelength-dependent direction, terminates secondaries, rewrites wavelengths,
stochastic pdf. Mix Shader's stochastic selection already lets a texture mask a grating onto part of a
surface. Adding a kind means one enum entry, one source file and its tests; the common kernel is not
touched. The order of kinds is: grating (first release, proves the mechanism), then multilayer, measured,
hair, fluorescent diffuse (second wave).

**3. A special-surface shade stage.** The shader sort key (research file 04, R3) gets a high bit for
`ext_kind != 0`; those hits go to a separate queue and a separate stage that runs the same OpenPBR lobe
code plus a switch over extension kinds. The common shade stage contains no extension code, so its
register count is untouched. If the special stage itself spills, a heavy kind moves to its own stage (as
Cycles splits `shade_surface_raytrace`); this is one stage per kind at most, not a combinatorial
template. CPU and GPU run the same stage functions. Cost when unused: an empty queue and one skipped
launch per bounce.

**4. A wavelength packet that can change mid-path.** Fluorescence (excitation λi below the observed λo),
Doppler and gravitational redshift (factor g), dispersion and gratings all change or collapse the
wavelengths a path carries. The path state therefore keeps two things apart:

- **film wavelengths**: the hero seed and sampling pdf used for CMF and response weighting at the
  film, recomputed from the seed as file 04 (R2) proposes;
- **transport wavelengths**: N stored floats that the material, medium and geodesic stages read and may
  rewrite, with flags `secondaries_terminated` and `wavelengths_rewritten`.

Tracing from the camera, a fluorescent vertex collapses the packet to the hero (as dispersion does),
samples λi from the reradiation distribution for that λo, multiplies throughput by M(λi→λo)/p(λi) and
continues at λi; the film still weights by λo. A redshift vertex scales the transport wavelengths by g and
radiance by g³. This replaces file 04's "hero seed only (wavelengths recomputed)": it costs 16 B per path
at N = 4 (about 10% of the estimated 130-170 B state; S3 measures it). Spectral MIS (`r_u`, `r_l`) after a
rewrite needs a stated convention; that is unverified and must be derived before fluorescence lands.

**Polarisation: designed-for, not reserved in state.** No Stokes slot in the first milestone. What is
reserved costs nothing: an explicit orthonormal local frame at every vertex (needed anyway for
anisotropy, tangent normals and grating grooves); Fresnel functions return s and p terms that the caller
averages; the spectrum type is defined in one place so a polarised build can replace it with a Stokes
spectrum. A polarised renderer is then a whole-build variant used for science renders (Mitsuba reports
1.5-2x time), not a runtime mode and not a per-material template.

**Media follow the same pattern.** The medium record has resolved spectral slots for σa, σs and emission,
a closed `phase_kind` (HG, Rayleigh, Draine, table) with one table handle, and an emission-dominated
"medium advance" stage for nebulae (file 04, R8). Line emission and line absorption are spectral sources
like any other.

**What the first milestone must reserve, and what it costs when unused**

| Reservation | Where | Size | Cost when unused |
|---|---|---|---|
| Transport wavelengths stored, film seed kept | path state | +16 B | memory traffic only |
| Packet flags (terminated, rewritten) | path state flags | 2 bits | none |
| Ray cone (spread angle, width) | path state | +8 B | traffic; also serves texture filtering and bump, which need it regardless |
| `ext_kind`, `ext_index` | material record | 4 B | none in the common stage |
| Spectral slots resolved to N values | stage-1 output record | about +32 B at N = 4 | traffic between stage 1 and stage 2 |
| Special-surface queue and stage | stage layout | one queue | one empty-queue check per bounce |
| Scene-built wavelength CDF (lines, response) | per render | one table | an inverse-CDF lookup instead of an analytic one (spike E) |
| `phase_kind` and table handle | medium record | 4 B | none |
| s/p Fresnel factoring, one spectrum type definition | code | 0 | none |

**How this avoids v1's four failures**

| v1 problem | Why it does not recur |
|---|---|
| Fat shared material struct | Record pinned to OpenPBR plus 4 B; extension parameters in a side table; large data in tables. |
| Register saturation of the fused shade kernel | Extension code lives only in the special stage; spikes A and S1 gate both stages with a no-spill check. |
| Template variant matrix | No compile-time axis per feature; runtime `ext_kind` switch in one stage; polarisation is a single whole-build option. |
| Hand-ported CPU/GPU twins | Every kind is one source compiled for both targets; adding a kind touches no backend code. |

A general closure graph (v1's closure graph, Cycles SVM closures) is rejected again: it is the source of
the lobe-count spills, and Add Shader stays a visible error until a use case forces a decision.

## How these appear in Blender

- **Native first.** Principled, Metallic (including Physical Conductor with IOR and Extinction), Glass,
  Blackbody, Wavelength, Sky, Volume and Principled Volume map onto the record, medium and lights with no
  Astroray UI. This is where most users meet spectral behaviour.
- **Astroray nodes** (v1 already registered custom `ShaderNode` subclasses with an Astroray Add-menu
  submenu: Spectral Sources, Spectral Modifiers, Response, Hints). Candidates: Spectrum (preset, drawn,
  blackbody, file, Gaussian, element lines with mixing), Spectral Reflectance (shader + spectrum, replaces
  the colour slots), Spectral Filter (also usable as a light gel), Diffraction Grating, Multilayer
  Coating, Measured BSDF, Fluorescent, Cloud Volume. Each should be a custom group node whose inner node
  group is a Cycles/EEVEE approximation, so files still render in other engines. Whether a custom node
  survives `inline_shader_nodes()` with its identity, or must be read before inlining, is unverified and
  needs a spike (v1 had the same open check).
- **Property panels**: light panel with spectrum mode (blackbody plus filter, preset, custom, RGB), as
  the owner described; material panel for extension parameters; object panel for astrophysical objects
  (black hole presets); render panel for wavelength domain, film response curves and data-cube output.
- **Asset library** of spectral materials and lamps (measured reflectances, glasses, metals, lamp SPDs,
  gratings with standard pitches) shipped as a `.blend` library. Whether an extension can register its own
  asset library without editing user preferences is unverified.
- **What an addon cannot do**: add sockets to native nodes (a spectrum cannot be wired into Principled's
  Base Color; it goes through a wrapper node), change how Cycles or EEVEE evaluate a node, make custom
  socket types link cleanly into native sockets (v1 used shader-typed outputs as a workaround), or rely on
  a Blender fork (owner decision 14). Material previews do use the engine.

## Test scenes each family needs

Analytic or explicit-spectrum oracles first; renders are inspected by eye as well (CLAUDE.md rule 4).

- **OpenPBR lobes**: the spec's furnace configurations per lobe and in mixes, at each packet wavelength;
  analytic Fresnel; chi-squared sampling tests per lobe.
- **Spectral slots**: white albedo returns white under the working-space illuminant; a measured table
  reproduces itself in a one-bounce render; gold reflectance peaks in 550-600 nm (v1's planned check);
  conductor Fresnel at normal incidence equals ((n-1)² + k²)/((n+1)² + k²).
- **Thin film and multilayer**: reflectance versus thickness against Airy and the transfer matrix at
  three thicknesses (v1 used hue within 10° of the Airy prediction); soap bubble; anodised titanium;
  energy ≤ 1 over a thickness × IOR × roughness grid.
- **Dispersion**: prism deviation within 5% of Sellmeier; Abbe number reproduced from C/d/F lines; sodium
  lamp through BK7 gives a narrow amber band against a broadband control (v1 measured a chromaticity spread
  of 0.0146 versus 0.4917).
- **Grating**: a laser line onto a grating and screen, spots at the grating-equation angles for orders
  m = ±1, ±2; white light gives an ordered spectrum; sum of efficiencies ≤ 1 under a furnace; a CD disc
  scene for the eye.
- **Fluorescence**: UV-only light on a brightener gives emission only at longer wavelengths, energy
  bounded by quantum yield; a no-UV control stays dark.
- **Measured BSDF and hair**: single-bounce reproduction of table values; per-lobe furnace (v1 used 1%).
- **Emitters**: analytic irradiance for point, spot, area and sun (albedo·S/π); 6500 K blackbody neutral;
  sodium at xy (0.583, 0.417) and mercury near (0.33, 0.38); a line emitter that converges without
  fireflies at equal time (spike E); IES orientation and absolute scale.
- **Media**: Beer-Lambert slab; single-scatter density shape; forward/back ratio under HG; emission-only
  slab I = jL within 0.5%; Case-B Hα/Hβ = 2.86; Mie rainbow at 42° (primary) and 51° (secondary).
- **Sensors**: data-cube bands sum to the integrated radiance; band-limited NIR render (v1: snow/water NIR
  ratio 4.82); analytic photon count that fails on a 10⁹ unit slip.
- **Astro**: invariant transfer I_ν/ν³; recovered colour temperature equals g·T within 1%; Kerr shadow
  edges against an external oracle; the black-hole-in-nebula target scene.
- **Polarisation (later)**: zero p-reflection at Brewster's angle; Malus's law with two polarisers.

## Open questions for the owner

1. The early-planning list was not recovered. Does the catalogue miss any material type you discussed
   then? Please name any you remember.
2. Diffraction gratings in the first release, as the first extension lobe and proof of the mechanism, or
   in the second wave?
3. Is the first astrophysics target (black hole in an emission nebula) part of the first release or the
   milestone after it? The tiers above assume the milestone after.
4. Fluorescence is placed in the second wave. Which use matters most: brighteners and paper, minerals
   under UV, or astrophysical fluorescence?
5. Is any science output polarised (synchrotron, sky polarisation, polarimetry)? That decides when the
   polarised build is scheduled.
6. Measured data with restrictive licences (MERL is non-commercial; RGL is free for research): ship the
   loader only, and let users bring the data?
7. Is one extension lobe per material enough (for example a grating with its own coating folded into its
   efficiency table), or do you need two stacked effects on one surface?

## Unverified

- The owner's early-planning material list (not found). Dates on several owner quotes are approximate,
  and "fixed files" as FITS is a guess.
- All literature claims in the four surveys are REPORTED (search summaries or memory) except: the PLTFalcor
  README and licence, the Belcour-Barla and Guillen project pages, the Mitsuba polarisation page (first
  100k of 149k characters), and two search-confirmed claims about Frisvad et al. 2007 and Jendersie and
  d'Eon 2023. Titles or venues unconfirmed: Mojzík 2016 and 2018, Aliaga 2023, Deliot and Belcour 2023,
  which Chermain 2020 paper is meant, Guo et al. 2018 retroreflection, Stam 1999 details.
- No licence-compatible reference implementation was found for diffraction gratings; the grating equation
  is textbook, but the efficiency model needs a citable source before coding (CLAUDE.md rule 5).
- Licences unchecked: Jung 2019 and Hua 2023 code, ART, Mitsuba 0.6 `hapke`, Cloudy, ipole, grtrans,
  RAPTOR, glint code, the Belcour-Barla supplement.
- Whether F82 slots can be replaced by exact complex-IOR Fresnel through one mode bit without changing
  OpenPBR layering results (the albedo tables assume F82) is this author's proposal, not tested.
- The spectral MIS convention after a wavelength rewrite (fluorescence, redshift) is not derived.
- Whether custom shader nodes survive `inline_shader_nodes()`, and whether an extension can register an
  asset library, are untested.
- Byte and cost figures in the reservation table are this author's arithmetic, not measurements.
- Research file 05 recommends "no Sellmeier parser until a use case needs one"; v1 shipped Sellmeier
  presets and the owner's dispersion showcase is such a use case. This conflict is resolved here in favour
  of an analytic Sellmeier source, pending a decision record.
- The inventories disagree on some v1 statuses (headers marked done with unticked acceptance boxes); the
  catalogue's "B" marks follow the companion inventory, which flags those cases.

## Review findings (2026-11-14, unresolved: the material-model decision record must answer these)

An independent check of this file against the raw inventory found no fabricated owner quotes and confirmed the tier counts. It found the following problems.

**Weaknesses in the architectural consequence**
- The special-surface stage as written is a second full shade kernel with a switch over about 11 extension kinds: its register count is the maximum over all kinds, and "one stage per heavy kind" tends toward one kernel per material. This is the shape the owner wanted to avoid and needs a tighter rule.
- More than one special stage already exists in the first release (random-walk subsurface, the medium stack, the geodesic stage); the reservation table counts one queue.
- Gratings are delta in direction per wavelength. A point or laser source behind a grating, a CD scene, and a Mie rainbow cannot be reached by next-event estimation or BSDF sampling; they need photon or light-tracing machinery, which nothing reserves. v1 needed the same for prism caustics.
- Albedo-scaling layering assumes each replacement lobe supplies a directional albedo, which is ill-defined for fluorescence, stochastic layered BSDFs and wavelength-split gratings.
- One extension lobe per material cannot express a coated blazed grating or a thin film over a grating.
- Complex IOR (k up to about 7) placed in colour-style slots that are bounded, textured and filtered like colour; albedo tables assume F82. Coat over a conductor needs complex-IOR directional albedo. Untested.
- The "rgb" spectrum source does not distinguish reflectance from illuminant upsampling; v1 saw a 116x emitter error from the wrong choice.
- Polarisation would require every lobe to return a Mueller matrix: a second implementation of all BSDFs, not a typedef.

**Status claims to correct**
- v1 fixed the IES candela-to-watt scale as a constant; this file says lumens to watts through the emitter spectrum. Decide which and say why.
- "Line-aware hero sampling" was built in v1 only for photon wavelengths; general emission-line sampling was a v1 non-goal.
- Principled Volume, thin film on glass, spectral hair on GPU, laser and random-walk subsurface are marked "built" but were partial, CPU-only or wrong on GPU in v1.
- The owner asked for line absorption and molecules; the catalogue covers emission lines only.
- The owner's thin-film request was to match Cycles' saturated look; this file re-reads it as per-wavelength Airy under the physics-first decision. Confirm with the owner.

**Items missing from the catalogue** (also added to `v1-lessons/feature-inventory.md`)
- Analytic sphere primitive with exact multi-bounce chain, used as the dispersion and caustic oracle.
- Gemstone material (brilliant-cut diamond showcase) beyond the Sellmeier preset.
- Input nodes: Layer Weight, Fresnel, Backfacing, Ray Length, Hair Info.
- Per-object caustic-caster switch.
- Hair near-field model and per-strand random.
- RGB-lift conventions: reflectance vs unbounded vs illuminant; authored reflectance clamped to [0,1].
- v1 materials dropped without comment: Disney BSDF, mirror, isotropic, two-sided, normal/bump wrapper, thin architectural glass, closure graph.
