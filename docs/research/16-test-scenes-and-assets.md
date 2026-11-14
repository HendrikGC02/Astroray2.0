# Test scenes, models and reference data

Researched 2026-11-14. Tags: **VERIFIED** = primary page opened this session (some via the fetch tool's page summary); **REPORTED** = from search snippets, memory or secondary pages. Several primary hosts were unreachable (Cornell PCG returned ECONNREFUSED/404, blender.org demo-files returned 403, McGuire archive index listed no per-model data); those items are REPORTED or in "Unverified". Nothing was downloaded. Sizes are approximate and taken from the pages where stated.

Key finding up front: **no public scene is spectrally authored.** Every collection below ships RGB textures and RGB lights. A spectral renderer must upsample them, so these scenes test correctness of the RGB-to-spectrum path and general light transport, not spectral physics. Spectral physics needs the measured-data sets (Cornell, refractiveindex.info, CIE, MERL/RGL) plus scenes the owner models (see Gaps).

## Recommended adoption list

Licence column is what the source states. "Commit" = may live inside the public Astroray git repo (small and permissive). Everything marked "no" goes through the download manifest (see handling rules).

| Asset | What it tests | Source URL | Licence | Commit | Size | Priority |
|---|---|---|---|---|---|---|
| Bitterli: Cornell Box | Closed-room GI, energy conservation, RGB reference | https://benedikt-bitterli.me/resources/ | CC0 (VERIFIED) | yes | small (REPORTED < few MB) | P0 |
| Bitterli: Veach MIS | MIS: 4 plates x 4 lights, glossy lobes | same | CC0 (VERIFIED) | yes | small | P0 |
| Bitterli: Veach Ajar | Indirect-only lighting through ajar door, bidirectional-hard | same | CC0 (VERIFIED) | yes if textures small, else manifest | medium | P0 |
| Bitterli: Veach Bidir Room | Small lights, indirect diffuse | same | CC0 (VERIFIED) | yes | small | P1 |
| Bitterli: Material Test Ball | Material lookdev ball, env lit | same | CC0 (VERIFIED) | yes | small | P0 |
| Bitterli: Volumetric Caustic | Participating media + caustics | same | CC0 (VERIFIED) | yes | small | P0 |
| Bitterli: Water Caustic | Refractive caustic through water surface | same | CC0 (VERIFIED) | yes | small | P0 |
| Bitterli: Glass of Water | Dielectric nested interfaces, caustics | same | CC0 (VERIFIED) | maybe | medium | P1 |
| Bitterli: Utah Teapot (full), Little Lamp, Bedroom, Contemporary Bathroom, Pontiac GTO, Old vintage car, Victorian House, 4060.b Spaceship, Hair Curls, Fur Ball | Showcase / indirect interiors / hair / many materials | same | CC0 (VERIFIED) | no (manifest) | 1 MB to 100s MB (REPORTED) | P1 |
| Bitterli CC-BY set (Grey&White Room, Country Kitchen, Modern Hall, Staircase, Salle de bain, White Room, Modern Living Room, Breakfast Room, Coffee Maker, Japanese Classroom, Dragon) | Indirect-dominated archviz interiors | same | CC BY 3.0 (VERIFIED) | no (manifest; attribution file) | 10s-100s MB | P1 |
| Bitterli NC set (Curly Hair, Straight Hair, Lego Digger) | Hair, plastics | same | CC BY-NC (VERIFIED) | NO | - | avoid |
| Mitsuba Material Ball (matpreview) | Standard matpreview rig | same (Bitterli lists it) | Mitsuba 3 LICENSE (page does not name a CC licence) (VERIFIED) | check first | small | P1 |
| pbrt-v4-scenes: bistro, barcelona-pavilion, bmw-m6, contemporary-bathroom, landscape, lte-orb, sportscar, head, hair, disney-cloud, bunny-cloud, explosion, dambreak etc. | Production-style stress + volumes | https://github.com/mmp/pbrt-v4-scenes | per scene, many not stated (VERIFIED) | no | not stated | P1 (per-scene licence check) |
| Stanford Bunny, Dragon, Armadillo, Lucy | Mesh/BVH, scale, reference geometry | https://graphics.stanford.edu/data/3Dscanrep/ | free research use, acknowledge, redistribution of files allowed, commercial needs permission (VERIFIED) | Bunny only, maybe (see rules) | Bunny 4.9 MB; Lucy 307 MB | P1 |
| Cornell Box measured reflectance + light spectra | Spectral correctness of RGB-box vs measured box | https://www.graphics.cornell.edu/online/measurements/ (REPORTED; host unreachable, search showed en-cg-web.coecis.cornell.edu/online/measurements) | not stated (REPORTED) | no, fetch via manifest | KB | P0 |
| refractiveindex.info database | Measured n,k of metals/dielectrics, dispersion | https://refractiveindex.info/about ; https://github.com/polyanskiy/refractiveindex.info-database | CC0 (VERIFIED) | subset yes (CC0) | zip tens of MB (REPORTED) | P0 |
| CIE illuminants D65, A, D50 (CSV, with DOI), LED reference CIE 251:2023 | Illuminant spectra | https://cie.co.at/data-tables (REPORTED) | not verified | no until licence read | KB | P0 |
| MERL BRDF database | Measured isotropic BRDF vs OpenPBR fit | https://www.merl.com/research/downloads/BRDF ; DOI 10.5281/zenodo.8101680 | page states none; check Zenodo record (VERIFIED absence) | no | 100 materials, ~33 MB each (REPORTED) | P2 |
| RGL/EPFL material database | Spectral measured BRDFs (Dupuy-Jakob) | https://rgl.epfl.ch/materials | not stated on page (VERIFIED absence) | no | not stated | P1 |
| Poly Haven HDRIs | Environment lighting | https://polyhaven.com/license | CC0 (VERIFIED) | a few small ones yes | 1k-16k EXR, MBs to 100s MB | P0 (but scraping bulk is ToS-restricted) |
| Khronos glTF-Sample-Assets | Material extension checks (transmission, volume, IOR, iridescence, dispersion) | https://github.com/KhronosGroup/glTF-Sample-Assets | repo CC-BY-4.0; per-model licences in model READMEs (VERIFIED) | selected small models if licence allows | small each | P2 |
| ASWF usd-wg/assets: StandardShaderBall | OpenUSD/MaterialX shader ball | https://github.com/usd-wg/assets/ | repo Apache-2.0; per-asset licence in readme, typically CC0 (VERIFIED) | likely yes (read readme) | < 30 MB (largest full asset) | P0 |
| ASWF DPEL: OpenPBR Shader Playground, MaterialX Teapot and Lion | OpenPBR parameter sweeps | https://dpel.aswf.io/ | ASWF Digital Assets License v1.1 default (VERIFIED); terms not read | no | not stated | P1 |
| ASWF DPEL: 4004 Moore Lane (Intel), Netflix ALab | Large USD interiors / production asset | https://dpel.aswf.io/ | ASWF v1.1 (VERIFIED) | no | GBs (REPORTED) | P2 |
| Intel Sponza (2022) | Atrium GI, glTF/USD/FBX | https://www.intel.com/content/www/us/en/developer/topic-technology/graphics-research/samples.html | "Creative Commons Attribution" (VERIFIED, per-asset not stated) | no | base 3.71 GB | P1 |
| Intel Jungle Ruins | Dense vegetation | same | CC Attribution (VERIFIED, page-level) | no | 4.0 GB | P2 |
| Intel Volumetric Cloud Library (via DPEL) | Cloud VDBs | https://dpel.aswf.io/ | ASWF v1.1 (VERIFIED) | no | <1 MB to >8 GB | P2 |
| WDAS cloud | Reference volumetric cloud | https://www.disneyanimation.com/resources/clouds/ | page links a licence PDF; ASWF list says CC BY-SA 3.0 (REPORTED) | NO (share-alike) | not stated | P1 |
| OpenVDB sample models | smoke, explosion, bunny_cloud, fire VDBs | https://www.openvdb.org/download/ | not stated on page (VERIFIED absence) | no | smoke 2 MB, explosion 72 MB, bunny_cloud 74 MB | P1 |
| JangaFX free VDBs | Smoke/fire/cloud animations | https://jangafx.com/software/embergen/download/free-vdb-animations | CC0 (REPORTED) | no | 60 MB to 5.5 GB | P2 |
| Cem Yuksel hair models | Strand hair BSDF | https://www.cemyuksel.com/research/hairmodels/ | free for personal/research; link back when publishing (VERIFIED) | NO (not a redistribution grant) | 1.7 to 39.4 MB | P1 |
| Amazon Lumberyard Bistro | Interior+exterior, many lights at night | https://developer.nvidia.com/orca | ORCA page states none; ASWF list: CC BY 4.0; pbrt-v4-scenes: CC-BY 4.0 (VERIFIED/REPORTED) | no | GBs | P1 |
| Disney Moana Island | Extreme-scale instancing | https://www.disneyanimation.com/resources/moana-island-scene/ | page links License.txt, terms not read; ASWF list says "custom" (VERIFIED/REPORTED) | NO | base 45 GB, pbrt-v4 5.6 GB | P3 (scale only) |
| ESO Milky Way panorama (Brunier) | Starfield background | https://www.eso.org/public/images/eso0932a/ | credit ESO/S. Brunier; ESO usage page; full-res withheld (VERIFIED) | no | 6000x3000, TIFF 27.7 MB | P2 |
| Gaia DR3 / Hipparcos / Tycho-2 | Physical star background | https://cosmos.esa.int/web/gaia-users/license | Gaia: CC BY-NC 3.0 IGO (VERIFIED); Hipparcos/Tycho not checked | NO | GBs for full catalogue | P2 |
| Blender BMW27 | Classic Cycles benchmark | https://download.blender.org/demo/test | licence not confirmed here | no | BMW27.blend.zip 3.0 MB | P1 |
| Blender Classroom | Indirect-heavy interior (Cycles benchmark) | same | CC0 (REPORTED via search of blender.org demo page) | maybe (70 MB: no) | classroom.zip 70.3 MB | P1 |

## Standard assets by category

### Scene collections

**Bitterli Rendering Resources** (https://benedikt-bitterli.me/resources/, VERIFIED). 35 listed scenes; formats Tungsten, Mitsuba 3 and pbrt-v4 (a few Mitsuba-only). Licences per scene: CC0, CC BY 3.0, and CC BY-NC for Curly Hair, Straight Hair, Lego Digger. Mitsuba/pbrt versions are automatic or approximate conversions, so do not expect pixel-identical output across renderers. Page asks for citation. Sizes not listed on the page. This is the single best source: most scenes are CC0 with matching formats in the two renderers that have the most published reference numbers. Includes the three Veach scenes, the Cornell Box, both caustic scenes, Material Test Ball and Fur Ball. Caveat: lights and textures are RGB.

**pbrt-v4-scenes** (https://github.com/mmp/pbrt-v4-scenes, VERIFIED). 26 scenes. README has no global licence; per-scene entries range from CC0 (bmw-m6, contemporary-bathroom) to CC-BY 4.0 (bistro, landscape), CC-BY 2.0 (barcelona-pavilion, head, lte-orb, sportscar), CC-BY-ND 2.0 (kroken, watercolor: no derivatives, so do not convert or modify them), CC BY-SA 3.0 stated as "CCA-SA 3.0" for disney-cloud, and many "not stated" (sanmiguel, villa, crown, ganesha, killeroos, explosion, bunny-cloud). Scene sizes not stated; Disney cloud is downsampled for GitHub limits. Treat "not stated" as all rights reserved. The pbrt-v4 spectral renderer is the closest published point of comparison for a spectral path tracer; reference images are in the repo's pages (REPORTED).

**Mitsuba 3 data**. Mitsuba matpreview/material ball is hosted via Bitterli (VERIFIED, licence unnamed). Mitsuba's own gallery and `resources/data` licences were not opened (see Unverified).

**McGuire Computer Graphics Archive** (https://casual-effects.com/g3d/data10/, VERIFIED page; per-model data not seen). OBJ + PNG zips; asks for citation; no archive-wide licence; the ASWF list says "varies per file" (VERIFIED). Sponza traces to Crytek and is reported as CC BY 3.0 by third parties (REPORTED). San Miguel has no licence statement found (REPORTED). Do not commit anything from this archive without reading the individual model README.

**Intel Sponza 2022** (VERIFIED page): Base 3.71 GB, formats 3ds Max source, USD, glTF, FBX; page-level "Creative Commons Attribution". Add-on Volumetric Explosion 9.13 GB (Houdini, Blender, VDB). Jungle Ruins 4.0 GB (Blender, USD). Jungle Ruins' package page states no licence and no size (VERIFIED absence); the ASWF DPEL page did not list it (VERIFIED), so treat as "Intel samples page: CC Attribution".

**NVIDIA ORCA** (https://developer.nvidia.com/orca, VERIFIED): five scenes (Bistro, Emerald Square, SpeedTree, UE4 Sun Temple, Beeple Zero-Day), FBX plus Falcor scene file, legacy/unsupported, no licence on page. ASWF list (VERIFIED page, secondary for these): Bistro CC BY 4.0, Emerald Square CC BY-NC-SA 3.0, Sun Temple CC BY-NC-SA 4.0, Zero Day CC BY 4.0, SpeedTree CC BY-NC-SA 3.0. Conflicts between sources: verify at the asset's own README before use. Emerald Square and Sun Temple are NC: never commit.

**Blender demo files** (blender.org/download/demo-files, 403 on fetch; REPORTED via search). Classroom (Christophe Seux, CC0, ~72 MB), Barbershop (Agent 327, CC-BY, ~280 MB), Barcelona Pavilion (eMirage, CC-BY, ~24 MB), Junk Shop (Alex Trevino, licence not confirmed), BMW27 (Mike Pan; licence not confirmed). Sizes for BMW27 (3.0 MB), Classroom (70.3 MB) and the pavilion (24.7 MB) VERIFIED at download.blender.org/demo/test. Monster Under the Bed: nothing found. The ./demo/test listing does not contain barbershop, junkshop or monster (VERIFIED), so those live elsewhere. Note these are Cycles-node scenes; Astroray will need an importer path and OpenPBR conversion to reuse them; they are benchmark/compat targets, not physics references.

**Disney Moana Island** (VERIFIED page): Base 45 GB (93 GB unpacked), Animation 24 GB, USD 15 GB, PBRT 6 GB, PBRT v4 5.6 GB (contributed by Matt Pharr). Licence file not read; ASWF list says custom (REPORTED). Out of reach for a 16 GB-class GPU without out-of-core and not a near-term target.

**ASWF Digital Production Example Library** (https://dpel.aswf.io/, VERIFIED): Netflix ALab (USD; textures/fur baked separately), 4004 Moore Lane (Intel, USD), OpenPBR Shader Playground (Adobe, MaterialX/OpenUSD), MaterialX Teapot and Lion (NVIDIA), Intel Volumetric Cloud Library (30 VDBs), AWS Airship, Noa Character, etc. Default ASWF Digital Assets License v1.1 (terms not read; assume redistribution restrictions until read). Sizes mostly not stated.

**ASWF USD WG assets** (https://github.com/usd-wg/assets/, VERIFIED): repo Apache-2.0, per-asset licence in readme (typically CC0), largest full asset ~30 MB, contains `full_assets/StandardShaderBall`.

**Khronos glTF Sample Assets** (VERIFIED): repo CC-BY-4.0 with per-model licences; assets without suitable licences were removed; Glass Hurricane Candle Holder uses Transmission + Volume. A "PBR tests" list (Models-pbrtest.md) exists (REPORTED by page). Good for validating material extension mapping, not as render references (glTF's BRDF is not OpenPBR).

**Stanford 3D Scanning Repository** (VERIFIED): Bunny (69k tris, 4.9 MB zip), Dragon (1.13 M tris, 11 MB), Armadillo, Happy Buddha, Lucy (28 M tris, 307 MB), Asian Dragon/Thai Statue (XYZ RGB). PLY. Terms: acknowledge, free redistribution for research, commercial use needs permission, scholarly figures allowed. Dragon/Buddha/Lucy/Thai have a request not to animate, break or Boolean them (so no shattering/melting tests). A repo that is public but MIT-licensed cannot relicense these: ship via manifest with a NOTICE.

### Material test rigs

- **Mitsuba/Bitterli matpreview ball and Material Test Ball** (VERIFIED existence): ball + backdrop + env map; the de facto look-dev rig.
- **ASWF Standard Shader Ball** (VERIFIED existence, licence in readme not read). Best choice for an OpenPBR/MaterialX-native rig.
- **Veach scenes** (VERIFIED): MIS (four glossy plates, four light sizes), Ajar (door ajar, lit only through crack), Bidir Room. CC0 on Bitterli.
- **Caustic scenes** (VERIFIED on Bitterli): Water Caustic, Volumetric Caustic, Glass of Water. Ring/cardioid and glass-egg classics have no verified public asset; they are simple enough to model (see Gaps).
- **White furnace**: no asset needed; analytic. Build in the repo (sphere inside uniform emitter, albedo sweep). Spectral extension: same at several wavelengths.
- **Many-light**: Bistro night (REPORTED, scene dependent), Emerald Square (NC). No clean CC0 many-light scene found.

### Spectral and dispersion references

- **refractiveindex.info** (VERIFIED): CC0, YAML files, whole-database zip (release dated 2026-05-24) and GitHub repo; cite Polyanskiy, Sci. Data 11, 94 (2024). Page disclaims accuracy; keep provenance (book/page of each file) in any subset. Best source for measured n,k of Au, Cu, Ag, Al, glass dispersion (BK7, SF11, etc).
- **CIE datasets** (REPORTED): illuminant A, D50, D65 CSV with DOIs; LED reference spectra CIE 251:2023; F-series not located on the CIE site (an R-package cites Hunt 1991). Licence not read; CIE publications are copyrighted, so check before committing; D65/A are formulae-derivable, which avoids the problem.
- **Cornell Program of Computer Graphics measurements** (REPORTED; primary host unreachable): gonioreflectometer BRDFs at 31/65 wavelengths over 400-700 nm; house paint diffuse reflectance 300-700 nm; light-source, filter and camera-response spectra. Noisy at short wavelengths and grazing angles (as stated in the search excerpt of the page). The Cornell box paper data (Meyer, Wallace, Liu et al. 1999) is the classic spectral box reference. Licence unstated.
- **MERL** (VERIFIED partly): 100 isotropic materials, RGB BRDF; data on Zenodo; the MERL page states no licence; the Zenodo record must be read. Tri-chromatic: tests lobe fits only.
- **RGL/EPFL** (VERIFIED partly): Dupuy-Jakob adaptive parameterization; spectral measured materials (REPORTED). Licence/format not on the page (FAQ at /pages/lab/material-database not opened). pbrt-v4-scenes sportscar cites the RGL database (VERIFIED), which is a sign that some redistribution occurs.
- Prism dispersion, soap bubble and fluorescence scenes from spectral papers: none located as downloadable assets (see Gaps).

### Volumes

- **WDAS cloud** (VERIFIED page, licence PDF not read): wdas_cloud.zip; ASWF says CC BY-SA 3.0 (REPORTED); pbrt-v4-scenes also says share-alike (VERIFIED). Share-alike is incompatible with committing it into a permissively licensed repo; use via manifest.
- **OpenVDB samples** (VERIFIED page): 25 models (smoke 2 MB, smoke2 28 MB, explosion 72 MB, fire 23 MB, bunny_cloud 74 MB, space 242 MB ...); licence not stated on the page (links to a License page, not opened).
- **Intel Volumetric Cloud Library** (VERIFIED on DPEL): 30 VDB assets, <1 MB to >8 GB, ASWF v1.1.
- **JangaFX free VDBs** (REPORTED): CC0, ~15 sims, 60 MB to 5.5 GB (tornado 1.7 GB, fire plume 5.5 GB, cloud pack 104 MB). Re-check links are live.

### Environment lighting

- **Poly Haven** (VERIFIED): HDRIs CC0, redistribution allowed; ToS 3.2 prohibits scraping/data mining without permission, so fetch a named short list (or use the public API under its API terms), not bulk. Calibration: Poly Haven HDRIs are not absolutely radiometrically calibrated (REPORTED, no source opened); use them for look, and a measured or analytic sky for absolute radiance.
- Absolutely calibrated HDRIs: none verified. Analytic sky models (Hosek-Wilkie, Nishita, Preetham) provide the radiance reference; reference data tables of the models not opened (Unverified).

### Hair, curves and skin

- **Cem Yuksel hair models** (VERIFIED): 8 models, HAIR binary, 1.7-39.4 MB; free personal/research use, link back on publication. Not an open licence: do not commit.
- Bitterli Hair Curls, Fur Ball: CC0 (VERIFIED). Curly Hair and Straight Hair: CC BY-NC (VERIFIED), avoid.
- **Infinite Realities / Lee Perry-Smith head**: pbrt-v4-scenes states CC-BY 2.0 (VERIFIED); a Three.js licence file states CC BY 3.0 from triplegangers.com (REPORTED). The versions disagree; record the exact text bundled with the file used.
- **Digital Emily 2 (Wikihuman, ICT)**: reported free for non-commercial use only (REPORTED, 2015 news); page vgl.ict.usc.edu/Data/DigitalEmily2 not opened. Treat as NC.

### Astrophysics

- **EHT code comparison**: Gold et al. 2020, ApJ 897, 148, DOI 10.3847/1538-4357/ab96c6 (REPORTED via ADS record listing; abstract found by search; paper itself not opened). Content per abstract: deflection angles for equatorial null geodesics in Kerr, images from simple parameterised matter distributions, images from GRMHD with different synchrotron emissivities, 2017 EHT observables; total-flux error ~1% with production settings; camera setup dominates discrepancy at small distances. Whether the test-problem images/tables are public as data files: not verified.
- **GRMHD data**: Dhruv, Prather, Wong, Gammie (arXiv:2411.12647) accompanies a public data release at thz.astro.illinois.edu (REPORTED; site not opened). It is the KHARMA v3 library, not the 2022 PATOKA library.
- **Kerr shadow analytic curves**: closed-form critical-curve expressions exist (Bardeen 1973; for Schwarzschild r = 3*sqrt(3)*GM/c^2); no dataset needed, implement from the equations. Reference formulas not opened this session.
- **Star catalogues**: Gaia is CC BY-NC 3.0 IGO (VERIFIED at the ESA licence page), commercial use needs ESA's separate terms. This overrides the assumption that Gaia is CC BY 4.0. Hipparcos/Tycho-2 licences not verified. ESO Milky Way panorama: credit ESO/S. Brunier, 6000x3000 (TIFF 27.7 MB), full-res original (about 800 MP) not releasable by ESO (VERIFIED).
- **Nebula emission cubes**: nothing verified; no public asset found that is a clean spectral emission cube in a renderer-friendly form.

### Benchmarks

- **Blender Open Data / Blender Benchmark** (REPORTED via search results only): default scenes Classroom, Junkshop, Monster; fixed-time run (~30 s per scene in raw submission data), score = samples per minute summed over scenes; methodology document not located, source code is the reference. A known bug report: bogus Junkshop values on Metal (T96519).
- **LuxMark v3** (REPORTED): LuxBall HDR (217 K triangles), Microphone (1.77 M), Hotel Lobby (4.97 M); score higher-is-better; v3.0 and v3.1 are not comparable.
- **pbrt/Mitsuba**: report equal-sample or equal-time renders with relMSE/MSE against high-spp references on the Bitterli scenes (REPORTED from general knowledge; no primary page opened for benchmark methodology). V-Ray/OctaneBench not researched.
- Scenes recurring in equal-time papers (REPORTED, memory): Veach MIS/Ajar/Bidir, Bitterli Kitchen/Living Room/Staircase/Bathroom, Sponza, San Miguel, Bistro, Disney Cloud, Moana. Not citation-counted this session.

## Proposed canonical test set for Astroray 2.0

Seventeen scenes. Tiers: S = renders in seconds (CI), M = showcase, H = benchmark (manifest, not CI).

| # | Scene | Tier | Source | Proves |
|---|---|---|---|---|
| 1 | White furnace sphere (albedo sweep, per-wavelength) | S | owner/analytic | Energy conservation of every BSDF lobe, spectral sampler unbiased |
| 2 | Cornell Box, RGB (Bitterli) | S | CC0 | GI sanity vs. other renderers |
| 3 | Cornell Box, spectral (measured reflectances + measured lamp spectrum) | S | Cornell data + owner model | Spectral upsampling error vs. measured spectra (compare to #2) |
| 4 | Veach MIS | S | CC0 | MIS, light sampling, glossy lobes |
| 5 | Veach Ajar | S/M | CC0 | Indirect-only light transport; where naive NEE fails |
| 6 | Material ball (ASWF Standard Shader Ball) x OpenPBR sweep | S | ASWF | OpenPBR parameters one by one, MaterialX import |
| 7 | Water Caustic + Volumetric Caustic | S/M | CC0 | Caustic paths, media + refraction |
| 8 | Glass of Water | M | CC0 | Nested dielectrics, dispersion toggle |
| 9 | Dispersion prism + glass dispersion sweep | S | owner | Wavelength-dependent IOR, spectral sampling of dispersion |
| 10 | Thin-film (soap bubble, oil slick) | S | owner | Thin-film interference (OpenPBR thin film) |
| 11 | Measured-metals tile (Au, Cu, Ag, Al, Fe from refractiveindex.info) | S | owner + CC0 data | Complex IOR conductor vs. F82-tint approximation |
| 12 | Gas-discharge lamp rig (Na, Hg, Ne lines on white and coloured cards) | S | owner | Spectral line emitters, sampling narrow lines; metamerism |
| 13 | Bitterli Hair Curls / Fur Ball (CC0) | M | CC0 | Hair BSDF, curves |
| 14 | Bitterli Kitchen or Living Room (CC BY) | M | CC BY | Indirect-dominated interior; cross-renderer reference |
| 15 | Bistro (night, many lights) | H | CC BY 4.0 | Light sampling at scale, instancing, large textures |
| 16 | Disney cloud / OpenVDB bunny_cloud | H | share-alike / unstated | Volume scattering, sparse grids |
| 17 | Black-hole disk over star background (see Gaps) | M | owner + ESO/Gaia-derived | Lensing, emission spectra, background |

Plus, as non-scene reference data: Kerr shadow curve, Schwarzschild deflection angles, Gold et al. test problems (once data located). Suggested benchmark policy: fixed time, report samples/s per scene and relMSE vs. a long reference; GPU on the 5070 Ti; CPU is the correctness oracle. Heavy scenes (#15, #16, Sponza, Moana) stay off merge gates.

## Gaps: scenes the owner should model

No verified public asset covers any of these; all public scenes are RGB-authored.

1. **Spectral Cornell box.** Subject: Cornell box geometry with per-wavelength reflectances (measured paint data from Cornell, REPORTED) and measured lamp spectrum. Why: Bitterli's box is RGB; nobody ships a spectral box with the measured data bound together. Exercises: upsampling error, illuminant choice, white balance. Lighting: single area light, then D65/A/F swapped.
2. **Dispersion cabinet.** Subject: BK7 and SF11 prisms and a crown/flint pair on a matte table, slit light source. Materials: dispersive dielectric, absorbing glass. Why: public glass scenes (Glass of Water) are RGB, constant IOR. Lighting: white point source through a slit; expected rainbow with analytic deviation angle from Cauchy/Sellmeier.
3. **Thin-film set.** Subject: soap bubble, oil on wet asphalt, anodised titanium, coated lens. Why: no thin-film scene is available as a reusable asset. Exercises: OpenPBR thin-film parameters, angle-dependent colour, spectral integration of interference.
4. **Gas-discharge lamp gallery.** Subject: Na, Hg, Ne, Ar tubes on coloured test cards (red, green, blue, white, a Macbeth-style chart reflectance set from measured data). Why: only a spectral renderer shows line emitters correctly; RGB scenes cannot. Exercises: narrow-band sampling, metamerism (the same card under a continuous vs. a line source). Expected data: NIST atomic line tables (not verified here).
5. **Measured metals tile.** Au, Cu, Ag, Al, Ti, Fe, brass spheres from refractiveindex.info n,k lit by D65 and by a line lamp. Why: public scenes use RGB metal approximations. Compares full complex-IOR Fresnel to OpenPBR F0/F82-tint.
6. **Caustic classics.** Cardioid/ring caustic in a mirrored cup, glass egg, pool floor with animated (frozen) water. No public asset verified for the first two; Bitterli's Water Caustic covers one case. Ground-truth via analytic cardioid.
7. **Many-light night interior.** 1,000+ emissive sources of varied spectrum (sodium street lights, LED signs, windows) with a mostly dark scene. Public candidates are either NC (Emerald Square) or fuzzy (Bistro night). Tests light BVH/sampling.
8. **Indirect-dominated interior with a sealed light.** Lit only via bounced light from a skylight/slit, on top of Veach Ajar. Needed for path-guiding and robust-transport work.
9. **Participating-media suite.** Homogeneous milk-in-glass, smoky room with light shaft, coloured wavelength-dependent absorption (copper sulphate), heterogeneous fog around a lamp. Why: public clouds are scattering-dominated and RGB. Exercises chromatic media sampling and spectral MIS.
10. **Black-hole scene: accretion disk foreground/background.** Subject: thin disk (Novikov-Thorne-style temperature profile, blackbody emission, Doppler/gravitational redshift) around a Kerr hole, with a star-field and Milky Way background, plus a companion star in the foreground crossing the disk's image. Why: no public renderer-ready asset exists; EHT GRMHD snapshots are simulation volumes, not camera-ready scenes. Exercises: spectral redshift of the whole SED, lensed secondary images, multi-scale scene (stellar background vs. disk), photon-count output. Compare with Schwarzschild shadow and Gold et al. test problems.
11. **HMXB-style two-body scene.** O/B star plus Roche-lobe-filling geometry and wind, accretion stream. Public: none.
12. **Emission nebula.** Volumetric H-alpha/[OIII]/[NII] emission cube lit by embedded stars. Public emission cubes are not renderer-ready; model with analytic Stroemgren sphere for a ground truth.

## Asset handling rules

- Licence hygiene: record per-asset licence, author, source URL, retrieval date and checksum in a manifest. Only CC0/MIT/Apache/CC BY-compatible files go in git. Share-alike (WDAS cloud, Moana, CC BY-SA), NC (Curly Hair, Lego Digger, Emerald Square, Sun Temple, Digital Emily 2, Gaia) and no-derivatives (kroken, watercolor) assets never enter the repo, and never get edited and redistributed. "Not stated" means not allowed.
- Keep large assets out of git. Anything over about 5 MB, and anything that is not CC0, lives only in the manifest. No Git LFS in the public repo; use a `scripts/fetch_assets` step that downloads to a git-ignored `assets/` directory.
- Manifest: one TOML/JSON entry per asset: name, URL, licence SPDX id (or "none stated"), size, SHA-256, unpack path, redistributable (bool), citation text. The fetch script must fail loudly on checksum mismatch or missing licence field.
- Attribution: a generated `ASSETS_NOTICE.md` listing every asset used by committed renders (CC BY needs it; Stanford, Yuksel, Cornell ask for credit).
- Real-world scale and units: every scene declares units (metres) and scale in its manifest entry; correct converted scenes (pbrt/Mitsuba exports) if they differ; star background and astrophysical scenes use SI plus a stated length unit (e.g. M for gravitational radius). Photometric calibration (cd/m2 vs. relative) is stated per light and per environment map; Poly Haven HDRIs are treated as relative.
- Converted scenes: the pbrt-v4 and Mitsuba files on Bitterli are automatic conversions; any Astroray converter must be tested against the Tungsten original or a third renderer, not assumed identical.
- Poly Haven: fetch only named assets (no bulk crawling, per ToS 3.2); API use falls under their API terms.
- Rendered references: committed reference renders are our own output only; renders from NC assets stay out of the repo.
- Never commit Disney/Moana, Stanford Lucy/Thai (size, cultural-use note), ESO full-res panorama.

## Unverified

- Cornell Program of Computer Graphics measurement pages: host unreachable (ECONNREFUSED); contents and licence from search excerpts only. Also the old URL www.cs.cornell.edu/ptd/ returned 404.
- Blender demo-files page: HTTP 403; licences for Classroom (CC0), Barbershop (CC-BY), Pavilion (CC-BY) are from a search summary; BMW27, Junk Shop and Monster Under the Bed licences not confirmed.
- McGuire archive per-model licences, Sponza/San Miguel details: only the index and third parties.
- Mitsuba 3 gallery/data licences; matpreview licence (Bitterli page links to Mitsuba LICENSE, not read).
- Disney Moana License.txt and WDAS cloud License PDF contents (page-level only; ASWF list says custom and CC BY-SA 3.0 respectively).
- ASWF DPEL asset licence text (ASWF Digital Assets License v1.1); StandardShaderBall readme licence.
- MERL Zenodo record licence; RGL format, licence and size; Hipparcos/Tycho-2 terms.
- CIE data licences, F-series source, LED series contents.
- Gold et al. 2020 full paper and whether test-problem outputs are public; thz.astro.illinois.edu contents.
- Poly Haven HDRI absolute calibration claim; no calibrated HDRI source found.
- OpenVDB and Yuksel redistribution beyond the quoted terms; JangaFX page liveness.
- Sizes for most Bitterli and pbrt-v4-scenes scenes, and Blender Barbershop/Junk Shop file sizes beyond the search summary.
- V-Ray / OctaneBench benchmark definitions: not researched. pbrt/Mitsuba benchmark methodology: not read from primary sources.
- Nebula emission cubes and Kerr shadow reference tables: no primary sources opened.
