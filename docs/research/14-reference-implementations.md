# 14 - Reference implementations map

Date: 2026-11-14. Purpose: let an implementation task name the exact file to read
instead of browsing a whole engine. Paths are for the default branch named in the
tables; branches move, so pin a commit when you clone.

## 0. Conventions and how claims are tagged

- **[V]** = verified this session: I opened the primary source (licence file,
  file header, repository tree, or the file text) via raw GitHub fetch.
- **[R]** = reported: from my background knowledge or a secondary page. Treat as a
  lead to confirm, not a fact.
- Every file path below was fetched and returned HTTP 200 on the named branch [V]
  unless the row says otherwise. File existence is verified; the "what it does"
  note is verified only where I grepped the file (stated in the note).
- Paper citations (authors, year) are [R] unless the note says "cited in source"
  (then [V]: the source file's own comment names the paper).
- Repo sizes are the GitHub `size` field (git history, KB converted to MB), not a
  checkout size [V].

### Licence compatibility rules used in this document

Astroray 2.0 licence is not chosen. Two outcomes are described for every source:

| Source licence | If Astroray = permissive (Apache-2.0/MIT) | If Astroray = GPL-3 |
|---|---|---|
| Apache-2.0 | Adapt, keep copyright + NOTICE + file headers, state changes. | Adapt (Apache-2.0 is GPL-3 compatible, one way). |
| BSD-3 / MIT / Zlib | Adapt, keep copyright notice in file. | Adapt, keep notice. |
| GPL-3.0 | **Study only.** Re-derive from the paper; do not copy or paraphrase closely. | Adapt, whole combined work stays GPL-3. |
| GPL-2.0-or-later | Study only. | Adapt (the "or later" lets it go to GPL-3). |
| NVIDIA RTX SDK licence | Study only (not open-source compatible). | Study only. |
| Public domain (Unlicense) | Adapt freely (attribution still good practice). | Adapt freely. |

"Study only" still permits reading, running as an oracle, and comparing numbers.
It forbids pasting or line-by-line transliteration. Facts and published formulas
are not copyrightable; the implementation text is.

## 1. Engine catalogue

### 1.1 Core renderers

| Engine | Repo (default branch) | Licence | Best used for | Adapt? |
|---|---|---|---|---|
| PBRT-v4 | github.com/mmp/pbrt-v4 (master, 14.5 MB; submodules in src/ext not counted) | Apache-2.0 [V: LICENSE.txt; file headers say SPDX Apache-2.0] | Canonical spectral path tracer (hero wavelengths, RGB upsampling, sensor, volumes with spectral majorants, BVH light sampler) and the only reference with a complete **wavefront GPU** path (OptiX) in readable C++. Test suite with chi-squared tests. | Yes, both outcomes, with attribution. Note the repo drives CPU/GPU via `PBRT_CPU_GPU` macros and its own SOA code generator (`soac`); reading is easy, lifting needs adaptation. |
| Mitsuba 3 | github.com/mitsuba-renderer/mitsuba3 (master, 31 MB; ext/ holds submodules: drjit, embree, rgb2spec, nanobind, ...) | BSD-3-Clause plus an "Enhancements" patent-style grant [V: LICENSE read in full] | Spectral/polarised rendering, microfacet reference, null-scattering volpath with spectral MIS, Hosek-Wilkie sun/sky, chi-squared test harness (`chi2.py`), OptiX accel builder. | Yes, both. Keep the BSD notice; the extra Enhancements clause only binds contributions you send back. |
| Dr.Jit | github.com/mitsuba-renderer/drjit (master, 10.6 MB) | BSD-3-Clause [V] | Wavefront-by-construction JIT array language: shows how loops/branches become wavefront kernels (`while_loop.h`, `if_stmt.h`), RNG, complex numbers, textures. Useful as a design reference, not as a dependency decision. | Yes, both. |
| Blender Cycles | standalone: github.com/blender/cycles (main, 22 MB; canonical home projects.blender.org/blender/cycles [R]); also `intern/cycles` inside github.com/blender/blender [V: files present, e.g. intern/cycles/blender/sync.cpp] | **Apache-2.0 repo-wide** [V: LICENSE; scan of 563 .h/.cpp/.cu files under src/kernel, src/util, src/scene, src/bvh, src/integrator: 533 Apache-2.0, 28 BSD-3-Clause, 2 Zlib, 0 GPL]. See 1.2. | Production wavefront GPU integrator state/queues, Principled BSDF incl. thin film + multiscatter LUT, light tree, SVM node graph, random-walk SSS, volume null scattering, OpenPGL/OIDN integration, Blender sync + RenderEngine glue (`intern/cycles/blender`). | Yes, both, honouring the BSD-3 headers on 28 files. **GPL boundary:** anything in Blender outside `intern/cycles` (for example `source/blender/draw/engines/eevee/eevee_engine.cc` is GPL-2.0-or-later [V]) is GPL, not Cycles. |
| LuxCoreRender | github.com/LuxCoreRender/LuxCore (main, 172 MB) | Apache-2.0 [V: COPYING.txt] | OpenCL/CPU path engine with many materials (glossycoating, thinfilmcoating, carpaint, velvet), Prague sky (`sky3light.cpp`), direct-light-sampling cache, Sobol sampler. Older RGB architecture. | Yes, both (Apache). Prague utils file carries its own Apache header [V]. |
| BlendLuxCore | github.com/LuxCoreRender/BlendLuxCore (main, 358 MB, mostly assets) | GPL-3.0 [V: LICENSE] | Python Blender add-on: RenderEngine subclass layout, viewport draw, depsgraph handlers, Cycles node-tree reader (`export/cycles_node_reader.py`). The model for how a third-party engine plugs into Blender. | **Study only** unless Astroray is GPL-3. Whether an add-on that imports `bpy` must be GPL is commonly assumed yes [R]; that is a separate question from the engine licence. |
| RTXPT | github.com/NVIDIA-RTX/RTXPT (main, 180 MB) | **NVIDIA RTX SDKs LICENSE** [V: LICENSE.txt read; file headers also say redistribution without NVIDIA licence is prohibited] | Real-time path tracer: megakernel + SER, NEE-AT adaptive light sampling for up to 2M lights, bounded-VNDF sampling, ReSTIR DI/GI via RTXDI, SHaRC cache, denoiser guide layers, ray cones. | **Study only**, both outcomes. Licence text (around line 51) restricts combining the SDK with code that is distributed in source form or licensed for derivative works [V: read, summarised]. |
| Falcor | github.com/NVIDIAGameWorks/Falcor (master, 41 MB) | BSD-3-style (NVIDIA copyright) [V: LICENSE.md and headers of PathTracer.slang, Shift.slang read]; GitHub shows NOASSERTION because linked SDKs (DLSS, RTXDI, NRD...) have other licences | Research path tracer in Slang: StandardBSDF, LayeredBSDF, Chiang hair, light BVH, grid volume sampler (NanoVDB), **ReSTIR PT module** (`Source/Modules/ReSTIRPathTracing`), RTXDI bridge. Slang is close to CUDA/HLSL and readable. | Falcor's own files: yes, both (BSD-3, keep notice). Do not pull the linked DLSS/RTXDI/NRD parts. |
| LuisaRender | github.com/LuisaGroup/LuisaRender (next, 537 MB history) | BSD-3-Clause [V] | Small modern GPU renderer on LuisaCompute: **`spectra/hero.cpp` (hero wavelength)**, `spectra/srgb2spec.cpp`, wavefront `integrators/wave_path.cpp` plus megakernel variants, PBRT-style samplers (zsobol, padded_sobol, pmj02bn), Nishita sky. See 1.4. | Yes, both. Depends on its own DSL, so read for algorithm structure. |

### 1.2 Cycles file-level licence notes [V]

- Apache-2.0: the bulk, including `src/kernel/svm/sky.h`, `light/tree.h`,
  `sample/sobol_burley.h`, `integrator/subsurface_random_walk.h`,
  `closure/bsdf_principled_hair_huang.h`, `integrator/guiding.h`,
  `util/types_spectrum.h`.
- **BSD-3-Clause** (Sony Pictures Imageworks / OSL-derived; fine in both outcomes
  but the notice must stay): `src/kernel/closure/bsdf_microfacet.h`,
  `bsdf_util.h`, `bsdf_diffuse.h`, `bsdf_burley.h`, `bsdf_hair.h`,
  `bsdf_ashikhmin_velvet.h`, `bsdf_toon.h`, `bsdf_transparent.h`, `emissive.h`,
  `src/kernel/camera/projection.h`, `src/kernel/osl/*` (camera.h, closures.cpp,
  closures_setup.h, osl.h, services_gpu.h, strings.h), `osl/shaders/stdcycles.h`,
  `osl/shaders/node_fresnel.h`. The microfacet file's header says "adapted code
  from Open Shading Language".
- BSD-3 third-party sky: `third_party/sky/include/sky_nishita.h`,
  `third_party/sky/source/sky_hosek.cpp`, `sky_hosek_data.h` (the Hosek-Wilkie
  files carry the original authors' copyright).
- Zlib: `src/util/md5.cpp`, `md5.h`.
- Cycles is an RGB renderer with `Spectrum` macros (`src/util/types_spectrum.h`);
  it is **not** a spectral/hero-wavelength reference. Only `svm/wavelength.h`,
  `svm/blackbody.h` and the thin-film code do per-wavelength work.

### 1.3 Material, geometry, denoise and volume libraries

| Library | Repo | Licence | Best used for | Adapt? |
|---|---|---|---|---|
| Adobe OpenPBR BSDF | github.com/adobe/openpbr-bsdf (main, 0.4 MB) | Apache-2.0 [V] | **Complete reference implementation of OpenPBR 1.1.1** (README says so [V]): eval/sample/pdf per lobe, energy LUTs, LTC fuzz, thin-film, dispersion, homogeneous volume helpers; GLSL-style source with C++/CUDA/MSL/Slang interop headers. Extracted from Adobe's shipping renderer. The best single file set for "OpenPBR lobe X". | Yes, both. Large LUT data headers are part of it. |
| OpenPBR spec repo | github.com/AcademySoftwareFoundation/OpenPBR (main, 69 MB incl. images) | Apache-2.0 [V] | Specification text (`index.html`, `parametrization.md.html`), canonical MaterialX graph `reference/open_pbr_surface.mtlx`, example materials, `openpbr.bib` | Yes for the .mtlx; spec text is documentation (read it, cite it). |
| MaterialX | github.com/AcademySoftwareFoundation/MaterialX (main, 237 MB incl. resources) | Apache-2.0 [V] | OpenPBR as a node graph (`libraries/bxdf/open_pbr_surface.mtlx`), per-target shader generation (`source/MaterialXGenGlsl/Osl/Mdl/Msl/Slang/Shader/Hw`), GLSL BSDF implementations (`libraries/pbrlib/genglsl`), OSL/MDL implementations. | Yes, both. |
| Embree | github.com/RenderKit/embree (master, 278 MB) | Apache-2.0 [V] | CPU BVH builders (SAH, Morton, spatial splits, two-level, hair), curve primitives, `tutorials/pathtracer`. | Yes, both. |
| Open PGL | github.com/OpenPathGuidingLibrary/openpgl (devel, 14 MB; the old RenderKit URL redirects) | Apache-2.0 [V] | Path guiding: DQT (directional quadtree) and parallax-aware VMM guiding fields, Russian roulette guidance, image-space guiding buffer. Used by Cycles. | Yes, both; or link as a library. |
| Open Image Denoise | github.com/RenderKit/oidn (master, 35 MB) | Apache-2.0 code [V]; the trained weights are a separate repository whose licence I did not read [R: Apache-2.0] | Denoiser API with `albedo` and `normal` auxiliary inputs (`include/OpenImageDenoise/oidn.h`, README [V]); CUDA/HIP/SYCL/Metal/CPU devices. | Code yes; prefer linking as a library. |
| OpenVDB / NanoVDB | github.com/AcademySoftwareFoundation/openvdb (master, 130 MB); NanoVDB lives in `nanovdb/nanovdb/` | Apache-2.0 [V] | GPU-portable sparse volume grids: `NanoVDB.h`, `math/HDDA.h`, `math/SampleFromVoxels.h`, raytrace examples. | Yes, both. NanoVDB is header-only. |

### 1.4 Additional engines found (with reasons)

| Engine | Repo | Licence | Why it is useful | Adapt? |
|---|---|---|---|---|
| LuisaRender (see 1.1) | | BSD-3 [V] | Only compact open renderer I found with **hero wavelength + wavefront + zsobol** in one small tree, on a cross-backend GPU DSL. | Yes |
| rgb2spec | github.com/mitsuba-renderer/rgb2spec (master, 43 KB) | BSD-3-Clause [V] | The Jakob-Hanika coefficient-table optimiser and evaluator in 3 files (`rgb2spec_opt.cpp`, `rgb2spec.h`, `rgb2spec.c`). Smallest standalone path to "RGB to spectrum". | Yes |
| Prague Sky Model | github.com/PetrVevoda/pragueskymodel (master) | Apache-2.0 [V] | Reference implementation of the fitted multiple-scattering sky + attenuation model (`src/PragueSkyModel.cpp`). Spectral, with ground albedo and altitude. | Yes |
| Bruneton precomputed atmospheric scattering | github.com/ebruneton/precomputed_atmospheric_scattering (master) | BSD-3-Clause [V: file headers/repo licence field; LICENSE not opened] [R] | Physically based multiple-scattering atmosphere with LUTs (`atmosphere/model.cc`); radiometric and spectral-aware. | Yes |
| Unreal Engine Sky Atmosphere (Hillaire) | github.com/sebh/UnrealEngineSkyAtmosphere (master) | MIT [V: LICENSE head read] | Real-time multiple-scattering sky LUT technique (`Resources/RenderSkyRayMarching.hlsl`). | Yes |
| Optix_Apps | github.com/NVIDIA/OptiX_Apps (master) | BSD-style, NVIDIA copyright [V: LICENSE.md head read; full text not read] | Clean OptiX 7+ host/device examples (SBT, pipelines, wavefront-ish `rtigo*`) [R for the app list]. | Yes after reading the full licence |
| McNopper Hyperion + Harmonia | github.com/McNopper/Hyperion and /Harmonia (main) | MIT [V] | Very recent (2026) Vulkan/Slang path tracer implementing OpenPBR with medium walk and SMS; shows a small Slang OpenPBR integration (`shaders/bsdf.slang`, `path_integrator.slang`, `medium_walk.slang`). Quality not audited; RGB, not spectral [R]. | Yes, but treat as a second opinion, not an oracle |
| lajolla_public | github.com/BachiLi/lajolla_public (main) | MIT [V] | Small teaching volumetric path tracer; easy to read MIS and null-scattering basics [R for content]. | Yes |
| tungsten | github.com/tunabrain/tungsten (master) | GitHub reports NOASSERTION; I did not read the licence [R: zlib] | Clean RGB path tracer with photon beams, not spectral. Low priority. | Check licence first |

### 1.5 General-relativistic codes

| Code | Repo | Licence | Best used for | Adapt? |
|---|---|---|---|---|
| ipole | github.com/AFD-Illinois/ipole (master, 2 MB) | BSD-3-Clause [V] | Polarised covariant radiative transfer: geodesics, plasma tetrad, Stokes evolution via complex tensor N, thermal/power-law/kappa emissivity fits (`src/symphony`), analytic tests (`tests/analytic`). Moscibrodzka and Gammie 2018 (README [V]). | Yes, both. The most reusable GR transfer code. |
| RAPTOR | github.com/jordydavelaar/raptor (main, 95 MB; older github.com/tbronzwaer/raptor 22 MB) | GPL-3.0 [V] | RK45 geodesics in KS/MKS metrics, polarised RTE, kappa emission, tetrads. | **Study only** unless GPL-3. |
| Blacklight | github.com/c-white/blacklight (master, 0.6 MB) | **Unlicense (public domain)** [V: LICENSE read] | Dormand-Prince geodesics, polarised transfer with Strang splitting of rotativity vs emissivity, adaptive sampling, true-colour output. Compact C++ (about 30 files). | Yes, both. |
| GYOTO | github.com/gyoto/Gyoto (master, 83 MB) | GPL-3.0 [V: COPYING] | Many metrics (KerrBL, KerrKS, numerical), parallel-transport polarisation (`lib/Photon.C`), thermal and kappa synchrotron spectra, thin/thick disk models. Broadest astrophysics object library. | **Study only** unless GPL-3. |
| Odyssey | github.com/hungyipu/Odyssey (master, 0.3 MB; paper arXiv:1601.02063) | GPL-3.0 [V: COPYING] | Minimal CUDA Kerr ray tracer (`src/Odyssey.cu`); good for GPU kernel shape. Dated, 2022 last push. | Study only unless GPL-3. |
| GRay2 | github.com/luxsrc/gray (main, 0.9 MB; depends on the `lux` framework) | GPL-3.0 [V: COPYING] | OpenCL Kerr geodesics (`sim-org/phys.cl`, `RK4.cl`, `KS.cl`), paper arXiv:1706.07062 [V: a copy of the paper was present locally]. Last push 2021; the repo has no top-level LICENSE file, only COPYING. | Study only unless GPL-3. |
| KPolaris | github.com/zelinzh/KPolaris (main, 0.9 MB, pushed 2026-09; paper arXiv 2609.26298 [R]) | BSD-3-Clause [V] | Brand-new Kokkos GPU polarised GRRT: Kerr/FMKS geometries, thermal and non-thermal synchrotron, Stokes rotation, documented polarisation conventions, tests against ipole outputs. Maturity unknown; use as a cross-check, not an oracle. | Yes, both |

Not found as a public repository [V: GitHub search returned nothing]: a standalone
"GRay2" other than luxsrc/gray. grtrans (Dexter) and geokerr were not checked.

## 2. Algorithm -> where to read it

Columns: engine path (branch as in section 1), canonical paper [R unless noted],
one-line note. "pbrt" = mmp/pbrt-v4/master/src/pbrt. "cyc" = blender/cycles/main/src.

### 2.1 Spectral machinery and colour

| Algorithm | Paper | Read | Note |
|---|---|---|---|
| Hero-wavelength spectral sampling | Wilkie, Nawaz, Droske, Weidlich, Hanika 2014 | pbrt `util/spectrum.h` class SampledWavelengths (line 269), `SampleVisibleWavelengths`, `TerminateSecondary()` (line 313; called in `cpu/integrators.cpp:843`); LuisaRender `src/spectra/hero.cpp`; Mitsuba `include/mitsuba/core/spectrum.h` `sample_rgb_spectrum`, `pdf_rgb_spectrum`, `sample_wavelength` | pbrt is the cleanest. Dispersion must call TerminateSecondary. Mitsuba samples 4 stratified lambdas with a visible-range pdf. |
| RGB to spectrum, reflectance (albedo) | Jakob and Hanika 2019 | pbrt `cmd/rgb2spec_opt.cpp` (table optimiser), `util/color.h` RGBSigmoidPolynomial (line 332), `util/spectrum.h` RGBAlbedoSpectrum; rgb2spec repo `rgb2spec_opt.cpp`, `rgb2spec.h`; Mitsuba `include/mitsuba/render/srgb.h` `srgb_model_eval`; LuisaRender `src/spectra/srgb2spec.cpp` | Table is built offline; runtime is a 3-coefficient sigmoid polynomial lookup. |
| RGB to spectrum, unbounded and illuminant | same + pbrt book | pbrt `util/spectrum.h` RGBUnboundedSpectrum and RGBIlluminantSpectrum (declared lines 46-47) | Unbounded = scale * sigmoid; illuminant = scaled colour-space illuminant (D65) times albedo-style spectrum. |
| Newer uplifting (moments) | Peters, Merzbach, Hanika, Dachsbacher 2019; Todova, Wilkie, Fascione 2021 | **No reference implementation confirmed** in any engine here | Re-derive from the papers if the Jakob-Hanika gamut limits matter (saturated emission-line colours). |
| CIE XYZ matching, XYZ to RGB | CIE 1931; Wyman, Sloan, Shirley 2013 (analytic fits) | pbrt `util/spectrum.cpp` (CIE tables), `util/color.h` class XYZ; Mitsuba `core/spectrum.h` `cie1931_xyz` (line 241), `cie1931_y`; Cycles `src/app/cie_d65_luminance_fit.py`, `src/kernel/util/colorspace.h` | Cycles uses fits, not tables. Prefer tabulated CMFs for science. |
| Camera sensor response (spectral to pixel) | pbrt book 4e ch. 5/9 | pbrt `film.h` class PixelSensor (line 36), `film.cpp` PixelSensor::Create (213) and swatch fit (263); Mitsuba `src/films/specfilm.cpp` (spectral film) | PixelSensor handles XYZ and fitted camera RGB sensors with exposure/ISO. Mitsuba SpecFilm writes per-band output (useful for instrument-like observables). |
| Blackbody emission | Planck 1901 | pbrt `util/spectrum.h:69` `Blackbody()`, `D(T)` (line 83) for CIE daylight; Mitsuba `src/spectra/blackbody.cpp`; Cycles `kernel/svm/blackbody.h`; MaterialX `libraries/pbrlib/genglsl/mx_blackbody.glsl` | pbrt normalises by peak (Wien) so values stay near 1; photon-count astrophysics needs absolute units, so do not copy the normalisation. |

### 2.2 GPU architecture, sampling, acceleration

| Algorithm | Paper | Read | Note |
|---|---|---|---|
| Wavefront path state + queues | Laine, Karras, Aila 2013 | pbrt `wavefront/integrator.cpp` (Render at 290, kernel loop ~417-426, HandleEscapedRays 495, TraceShadowRays 575), `wavefront/workqueue.h`, `wavefront/workitems.soa`, `surfscatter.cpp`, `media.cpp`, `subsurface.cpp`; Cycles `kernel/integrator/state_template.h`, `state_flow.h`, `device/gpu/parallel_active_index.h`, `parallel_sorted_index.h`, `src/integrator/path_trace_work_gpu.cpp`; LuisaRender `integrators/wave_path.cpp` (RayQueue at line 175) | pbrt = clear queue-per-stage design with SOA. Cycles = production, material-sorted shading, shadow-state queues. |
| Sorting/compaction primitives | - | Cycles `device/gpu/parallel_prefix_sum.h`, `parallel_active_index.h`, `parallel_sorted_index.h` | Reusable pattern for active-ray compaction without Thrust. |
| Owen-scrambled Sobol (hash-based) | Burley 2020 (cited in source [V]) | Cycles `kernel/sample/sobol_burley.h`, `pattern.h`, `tabulated_sobol.h`; pbrt `util/lowdiscrepancy.h` (FastOwenScrambler near line 220), `samplers.h` (ZSobolSampler 225, PaddedSobolSampler 144); RTXPT `Shaders/PathTracer/Utils/NoiseAndSequences.hlsli` (cites Burley [V]; study only); LuisaRender `samplers/zsobol.cpp` | Cycles file is the shortest GPU-ready one (203 lines, Apache). |
| Blue-noise sampling (screen space) | Heitz et al. 2019; Ahmed and Wonka 2020 [R] | Cycles `kernel/sample/pattern.h` `blue_noise_indexing` (line 34) + `tabulated_sobol.h`; pbrt `util/bluenoise.h` (128x128x48 textures), `samplers.h` PMJ02BNSampler (367), `util/pmj02tables.h` | PMJ02BN: Christensen, Kensler, Kilpatrick 2018 [R]. |
| Counter-based RNG / hashing | Salmon et al. 2011 (Philox) [R] | pbrt `util/hash.h`, `util/rng.h` (PCG); Dr.Jit `include/drjit/random.h`; Falcor `Source/Falcor/Utils/Sampling/Pseudorandom/` (LCG, SplitMix64, Xoshiro) | Reproducible per-pixel streams need a documented hash; pbrt hash.h is MurmurHash-based. |
| BVH build, CPU | Wald 2007 (SAH binning); Karras 2012 (LBVH) [R] | pbrt `cpu/aggregates.cpp` (Morton primitives + radix sort at lines 29-42 (HLBVH-style builder); other builders in the same file not individually checked); Embree `kernels/bvh/bvh_builder_sah.cpp`, `bvh_builder_morton.cpp`, `bvh_builder_twolevel.cpp`, `include/embree4/rtcore_builder.h`; Cycles `src/bvh/build.cpp` | Embree is the quality benchmark. |
| BVH/OptiX use (GPU) | NVIDIA OptiX docs | pbrt `gpu/optix/aggregate.cpp`, `gpu/optix/optix.cu`; Cycles `src/bvh/optix.cpp`, `kernel/device/optix/kernel.cu`; Mitsuba `src/render/optix/accel.cpp`; Optix_Apps | pbrt shows OptiX 7 SBT and geometry with custom primitives; Cycles shows curves/motion blur via OptiX. |

### 2.3 Light sampling and integrator structure

| Algorithm | Paper | Read | Note |
|---|---|---|---|
| Light tree / many lights | Conty Estevez and Kulla 2018 | pbrt `lightsamplers.h` BVHLightSampler (line 260), `lightsamplers.cpp`; Cycles `kernel/light/tree.h` (importance function, line 122) + `src/scene/light_tree.cpp`; Falcor `Rendering/Lights/LightBVHSampler.slang`; RTXPT `Libraries/NEE-AT/NEEATBaker.hlsli` (study only); LuxCore `slg/lights/strategies/dlscache.cpp` | Cycles and pbrt differ in orientation-cone and importance terms; pick one and test against uniform-light-selection. |
| Environment map importance sampling | pbrt book 4e (piecewise-constant 2D); Clarberg et al. 2005; Bitterli 2015 (portals) [R] | pbrt `util/sampling.h` PiecewiseConstant2D, `lights.h` ImageInfiniteLight (544) and PortalImageInfiniteLight (631); Mitsuba `emitters/envmap.cpp` + `core/distr_2d.h`; Cycles `kernel/light/background.h`; Falcor `Rendering/Lights/EnvMapSampler.slang`, `Utils/Sampling/AliasTable.slang` | Alias table (Falcor) trades memory for O(1) sampling on GPU. |
| MIS and NEE | Veach and Guibas 1995 | pbrt `cpu/integrators.cpp` PathIntegrator::Li (628), SampleLd (764), VolPathIntegrator::Li (953), SampleLd (1273); Cycles `kernel/sample/mis.h` (power/balance, 41 lines), `integrator/shade_surface.h`; Mitsuba `integrators/path.cpp` (`mis_weight`, line 201) | Mitsuba path.cpp is the shortest readable full example. |
| Caustics via manifold NEE (optional) | Zeltner et al. 2020 [R] | Cycles `kernel/integrator/mnee.h` (cites "Manifold Next Event Estimation" [V]) | Relevant for glass-lens caustics. Not a v1 requirement. |
| Adaptive sampling | Hachisuka et al. 2008 [R]; Cycles' exact heuristic: **no paper confirmed** | Cycles `kernel/film/adaptive_sampling.h` (convergence check line 29) | Per-pixel variance stop test. See also colour-blindness caution: test on coloured noise. |
| Path guiding | Mueller, Gross, Novak 2017; Herholz et al. 2016; Ruppert, Herholz, Lensch 2020; Rath et al. 2020 [R] | Open PGL `openpgl/directional/dqt/DQT.h`, `directional/vmm/ParallaxAwareVonMisesFisherMixture.h`, `include/openpgl/cpp/Field.h`, `RussianRoulette.h`; Cycles `kernel/integrator/guiding.h`, `src/util/guiding.h` | Cycles shows the integration points (training samples, guiding probability, MIS with BSDF). |
| ReSTIR DI/GI | Bitterli et al. 2020; Ouyang et al. 2021 | RTXPT `Rtxpt/RTXDI/*` (GenerateInitialSamples.hlsl, SpatialResampling.hlsl, GITemporalResampling.hlsl, GISpatialResampling.hlsl) - study only; Falcor `Rendering/RTXDI/RTXDI.slang` | RTXDI SDK itself is NVIDIA-licensed [V: GitHub reports NOASSERTION]. |
| ReSTIR PT | Lin, Kettunen, Bitterli, Pantaleoni, Yuksel, Wyman 2022 (GRIS) | Falcor `Source/Modules/ReSTIRPathTracing/` (`PathReservoir.slang`, `Shift.slang`, `SpatialResampling.cs.slang`, `TemporalResampling.cs.slang`), `RenderPasses/PathTracer/ReSTIRPTData.slang` | Only open ReSTIR PT here; BSD-3. Hybrid shift and retrace work queues are the complex part. |
| Denoiser auxiliary buffers | OIDN docs | OIDN `include/OpenImageDenoise/oidn.h` (filter images "albedo", "normal" [V]), README; pbrt `film.cpp` GBufferFilm::AddSample (588); Cycles `kernel/film/denoising_passes.h`, `src/integrator/denoiser_oidn.cpp`; RTXPT `Shaders/PathTracer/DenoisingLayers.hlsli` | Albedo/normal at the first non-specular hit; for spectral renderers convert to RGB before denoising. |

### 2.4 Materials

| Algorithm | Paper | Read | Note |
|---|---|---|---|
| GGX / Smith, VNDF sampling | Heitz 2018 (cited in source [V]) | pbrt `util/scattering.h` TrowbridgeReitzDistribution (109), `Sample_wm` (163); Cycles `closure/bsdf_microfacet.h` `microfacet_ggx_sample_vndf` (304); Mitsuba `render/microfacet.h`; Adobe `impl/openpbr_vndf_microfacet_distribution.h` | Cycles file is BSD-3 (OSL-derived). pbrt is simplest. |
| Bounded VNDF / spherical caps | Eto and Tokuyoshi 2023 (bounded VNDF); Dupuy and Benyoub 2023 (spherical caps) | RTXPT `Shaders/PathTracer/BSDFs/Microfacet.hlsli` `evalPdfGGX_BVNDF` (line 105; link to the GPUOpen paper in comment [V]) - study only | No open-licence engine implementation confirmed. Re-derive from the papers (formulas are short). |
| Multiple-scattering energy compensation | Kulla and Conty 2017 (cited in Cycles [V]); Turquin 2019; Heitz et al. 2016; Fdez-Aguera 2019 | Cycles `closure/bsdf_microfacet.h` `microfacet_ggx_preserve_energy` (line 544, LUT `ggx_E`/`ggx_Eavg`; tables built by `src/app/cycles_precompute.cpp`); Adobe `impl/openpbr_microfacet_multiple_scattering_lobes.h` + data headers; MaterialX `libraries/pbrlib/genglsl/lib/mx_microfacet_specular.glsl` `mx_ggx_energy_compensation` (line 517) | pbrt-v4 has no compensation. Adobe code is the OpenPBR-aligned one. |
| Conductor / dielectric Fresnel, complex IOR | Born and Wolf; pbrt book | pbrt `util/scattering.h` `FrDielectric` (61), `FrComplex` (81, spectral overload 94); Cycles `closure/bsdf_util.h` `fresnel_dielectric_polarized` (47), `fresnel_dielectric` (94); Mitsuba `render/fresnel.h`; MaterialX `mx_generalized_schlick_bsdf.glsl` | Cycles also has F82-tint (microfacet.h line 97) used by OpenPBR-style metal tint. |
| Thin-film interference | Belcour and Barla 2017 (cited in Cycles [V]) | Cycles `closure/bsdf_util.h` (section at line 446: `iridescence_lookup_sensitivity_channel`, `iridescence_airy_summation_channel`) used from `bsdf_microfacet.h`; Adobe `impl/openpbr_thin_film_iridescence_utils.h`; LuxCore `slg/materials/thinfilmcoating.cpp` | Mitsuba and pbrt: none found [V: not in file lists]. Cycles' version is RGB-channel-based; a spectral renderer evaluates Airy summation per wavelength. |
| Dispersion | Cauchy/Sellmeier; OpenPBR spec | Adobe `impl/openpbr_dispersion_utils.h`; pbrt `SampledWavelengths::TerminateSecondary` + `DielectricBxDF` (bxdfs.h:167, bxdfs.cpp:77); Cycles Abbe-number input in `kernel/svm/closure.h` (RGB approximation) | The spectral engine is the proper home; Cycles' is an approximation. |
| OpenPBR layering and lobes | OpenPBR spec; Belcour 2018 (atomic layers); Guo, Hasan, Zhao 2018 | Spec: OpenPBR repo `index.html`, `parametrization.md.html`; graph: `reference/open_pbr_surface.mtlx`, MaterialX `libraries/bxdf/open_pbr_surface.mtlx`; code: Adobe `impl/openpbr_bsdf.h`, `openpbr_aggregate_lobe.h` (base), `openpbr_coating_lobe.h`, `openpbr_fuzz_lobe.h`; pbrt `bxdfs.h` LayeredBxDF (432, stochastic layering) | Adobe composes lobes outer to inner with eval/sample/pdf per lobe. |
| OpenPBR fuzz / sheen | Zeltner, Burley, Chiang 2022 (LTC sheen) [R]; Conty and Kulla 2017 | Adobe `impl/openpbr_fuzz_lobe.h` + `data/openpbr_ltc_*.h` (README: LTC table for fuzz [V]); Cycles `closure/bsdf_sheen.h`; MaterialX `libraries/pbrlib/genglsl/mx_sheen_bsdf.glsl`, `lib/mx_microfacet_sheen.glsl` | |
| OpenPBR coat | OpenPBR spec | Adobe `impl/openpbr_coating_lobe.h`; Cycles `kernel/svm/closure.h` principled coat handling (line 117 onward) | Coat darkens and tints the base; coat roughness/ior/IOR-dependent emission attenuation (Adobe README mentions emission attenuated by coat and fuzz [V]). |
| OpenPBR subsurface and translucent base | OpenPBR spec | Adobe `openpbr_homogeneous_volume.h`, `impl/openpbr_volume_utils.h`, `impl/openpbr_thin_wall_diffuse_transmission_lobe.h`, `impl/openpbr_thin_wall_specular_transmission_lobe.h`; MaterialX `mx_subsurface_bsdf.glsl`, `mx_translucent_bsdf.glsl` | Adobe merges SSS and transmission into one interior homogeneous volume; integrator walks it. |
| Rough dielectric transmission | Walter et al. 2007 | pbrt `bxdfs.cpp` DielectricBxDF::Sample_f (77); Mitsuba `bsdfs/roughdielectric.cpp`; Cycles `closure/bsdf_microfacet.h`; Adobe `impl/openpbr_reflection_transmission_coefficient.h` | Check dead-sample handling (energy lost when sampled direction is invalid). |
| Random-walk subsurface | Chiang, Kutz, Burley 2016; Christensen and Burley 2015 [R] | Cycles `kernel/integrator/subsurface_random_walk.h`; compare diffusion-based pbrt `bssrdf.h`, `wavefront/subsurface.cpp` | Cycles = volumetric random walk (OpenPBR-aligned). pbrt = tabulated beam-diffusion BSSRDF. |
| Hair / curve BSDF | Chiang, Bitterli, Tappan, Burley 2015/2016 (cited in Cycles [V]); Huang, Hullin, Hanika 2022 [R] | pbrt `bxdfs.h` HairBxDF (921); Cycles `closure/bsdf_principled_hair_chiang.h`, `bsdf_principled_hair_huang.h`, `kernel/geom/curve_intersect.h` (adapted from Embree curves [V]); Mitsuba `bsdfs/hair.cpp` with white-furnace tests; Falcor `Rendering/Materials/HairChiang16.slang`; MaterialX `mx_chiang_hair_bsdf.glsl` | |

### 2.5 Volumes

| Algorithm | Paper | Read | Note |
|---|---|---|---|
| Null scattering with spectral majorants | Miller, Georgiev, Jarosz 2019 (cited in Mitsuba [V]); Kettunen et al. 2021; Novak et al. 2018 | pbrt `media.h` (DDAMajorantIterator 136, HomogeneousMedium 217, GridMedium 265, NanoVDBMedium 599), `cpu/integrators.cpp` VolPathIntegrator, `wavefront/media.cpp` (r_u, r_l rescaled probabilities per wavelength); Mitsuba `integrators/volpathmis.cpp`, `media/heterogeneous.cpp`; Cycles `kernel/integrator/shade_volume.h` (majorant/minorant, residual ratio tracking, null scattering at line 692; cites Miller [V]) | pbrt is the spectral reference: chromatic majorants handled with ratio rescaling, no hero-wavelength bias. |
| Heterogeneous volume data | Museth 2021 (NanoVDB); Museth 2014 (HDDA) [R] | NanoVDB `nanovdb/nanovdb/NanoVDB.h`, `math/HDDA.h`, `math/SampleFromVoxels.h`, `examples/ex_raytrace_fog_volume/nanovdb.cu`; pbrt `NanoVDBMedium`; Cycles `kernel/util/nanovdb.h`; Falcor `Rendering/Volumes/GridVolumeSampler.slang` | Per-voxel majorant grids over HDDA nodes. |
| Homogeneous medium + phase | HG 1941; OpenPBR | Adobe `openpbr_homogeneous_volume.h` (spectrally aware distance MIS across channels [V from README]); RTXPT `Utils/HomogeneousVolumeSampler.hlsli` (study only); Cycles `closure/volume_henyey_greenstein.h` | |

### 2.6 Sky, textures, shader graph, reconstruction, tests

| Algorithm | Paper | Read | Note |
|---|---|---|---|
| Sky: Nishita single scattering | Nishita et al. 1993 | Cycles `third_party/sky/include/sky_nishita.h` (BSD-3), `kernel/svm/sky.h`; LuisaRender `textures/nishita_sky.cpp` | |
| Sky: Hosek-Wilkie (sky and sun) | Hosek and Wilkie 2012, 2013 | Cycles `third_party/sky/source/sky_hosek.cpp` (BSD-3, original authors); Mitsuba `emitters/sunsky.cpp`, `include/mitsuba/render/sunsky.h` (cites both papers [V]) | Analytic, spectral, turbidity-parametrised. |
| Sky: newer multiple-scattering | Wilkie et al. 2021 (Prague); Hillaire 2020; Bruneton 2017 | PetrVevoda/pragueskymodel `src/PragueSkyModel.cpp`; LuxCore `slg/lights/sky3light.cpp` (states it is based on Prague [V]); UE Sky Atmosphere `Resources/RenderSkyRayMarching.hlsl`; Bruneton `atmosphere/model.cc` | Prague is spectral (55 channels fitted) [R]; best match for Astroray spectra. |
| Shader-node graph on GPU | none canonical; Cycles docs | Cycles `kernel/svm/svm.h` (interpreter), `kernel/svm/types.h`, `src/scene/svm.cpp` (graph compile), `src/scene/shader_graph.cpp` (graph build and constant folding), per-node files in `kernel/svm/`; MaterialX `source/MaterialXGenShader/` for generating code from graphs | SVM = bytecode interpreter in the kernel; MaterialX = codegen. These are the two designs to choose between. |
| Texture filtering, ray differentials | Igehy 1999; Akenine-Moller et al. 2019/2021 (ray cones) | pbrt `cameras.h` `Approximate_dp_dxy` (155), `GenerateRayDifferential`, `util/mipmap.h` (EWA 21); Cycles `kernel/util/differential.h`, `kernel/svm/image.h`; Falcor `Rendering/Materials/TexLODHelpers.slang`; RTXPT `Utils/TexLODHelpers.hlsli` (study only) | pbrt approximates differentials for secondary hits; ray cones are cheaper on GPU. |
| Pixel reconstruction filters | Mitchell and Netravali 1988; Ernst et al. 2006 (filter importance sampling) | pbrt `filters.h` (FilterSampler 26, Box 48, Gaussian 80, Mitchell, Lanczos, Triangle), `film.cpp` RGBFilm; Mitsuba `src/rfilters/*.cpp`; LuisaRender `src/filters/*.cpp` | pbrt samples the filter and weights equally (no negative-lobe noise from splatting). |
| Chi-squared sampling tests | Standard chi-squared; pbrt tests derived from Jakob's Mitsuba code [R] | pbrt `bsdfs_test.cpp` (chi^2 harness comment at 42, tests TRCondIso/TRCondAniso/TRDielIso at 461-485), `lights_test.cpp`, `samplers_test.cpp`, `media_test.cpp`, `lightsamplers_test.cpp`; Mitsuba `src/python/python/chi2.py` | pbrt harness is C++ and CPU only. Mitsuba's is Python over any variant. |
| White furnace / energy tests | - | pbrt `bsdfs_test.cpp` section "Energy Conservation Tests" (line 511); Mitsuba `src/bsdfs/tests/test_hair.py` (white_furnace tests) | No furnace test confirmed in Cycles. |

### 2.7 General-relativistic and radiative-transfer algorithms

| Algorithm | Paper | Read | Note |
|---|---|---|---|
| Kerr geodesic integration | Chan et al. 2013 (GRay), 2018 (GRay2, ApJ 867:59); Pu et al. 2016 (Odyssey); Dexter and Agol 2009 [R] | ipole `src/geodesics.c`; Blacklight `src/geodesic_integrator/geodesics.cpp` (Dormand-Prince 5th order, adaptive, line 27 [V]); RAPTOR `src/gr_integrator.c` (rk45_step), `src/metric.c` (KS/MKS, Christoffels at 433); KPolaris `src/geodesic/rk4.hpp`, `src/geometry/kerr_fmks.hpp`; GYOTO `lib/KerrBL.C`; GRay2 `sim-org/RK4.cl`, `KS.cl`; Odyssey `src/Odyssey.cu` | Prefer horizon-penetrating Kerr-Schild coordinates and an adaptive integrator. Permissive sources: ipole, Blacklight, KPolaris. |
| Covariant radiative transfer, unpolarised | Lindquist 1966; Rybicki and Lightman | ipole `src/ipolarray.c` (transfer along trajectory, Taylor switch near small optical depth, line 31); RAPTOR `src/rte_integrator.c`; Blacklight `src/radiation_integrator/unpolarized.cpp` | I_nu/nu^3 is invariant. |
| Polarised transfer (Stokes, Faraday) | Gammie and Leung 2012; Dexter 2016; Moscibrodzka and Gammie 2018; Bronzwaer et al. 2020 | ipole `src/ipolarray.c` (complex tensor N, `jar_calc` at 357, plasma tetrad at 384), `src/tetrads.c`; Blacklight `src/radiation_integrator/polarized.cpp` (Strang splitting at line 49 [V]); RAPTOR `src/pol_rte_integrator.c` (parallel transport via f_parallel, rk4_step_f); KPolaris `src/frame/stokes_rotation.hpp`, `docs/polarization_conventions.md`; Mitsuba `include/mitsuba/render/mueller.h` for the Mueller-calculus alternative | Fix and document sign/EVPA conventions early (KPolaris documents ipole's). |
| Synchrotron emissivities (thermal, power-law, kappa) | Leung, Gammie, Noble 2011; Pandya et al. 2016; Dexter 2016; Mahadevan et al. 1996 [R] | ipole `src/symphony/maxwell_juettner_fits.c`, `kappa_fits.c`, `power_law_fits.c`, `src/model_radiation.c` (E_THERMAL, E_DEXTER_THERMAL, E_LEUNG switches at 33-37 [V]); RAPTOR `src/pol_emission.c` (kappa rho_Q at 72); KPolaris `src/radiation/thermal_synchrotron.hpp` (Pandya/Dexter switch), `nonthermal_synchrotron.hpp`; GYOTO `lib/ThermalSynchrotronSpectrum.C`, `KappaDistributionSynchrotronSpectrum.C` | ipole (BSD-3) and KPolaris (BSD-3) are the permissive options. Validate ipole fits against direct integration where fits are out of range. |
| Relativistic Doppler / redshift | Lindquist 1966 | Transform wavevector into the emitter frame with a tetrad: ipole `src/tetrads.c` (`make_plasma_tetrad`, used at ipolarray.c:384 [V]); Blacklight `radiation_geometry.cpp` tetrad function (line 577 [V]); RAPTOR `src/tetrad.c` | **No standalone g-factor file confirmed.** For spectral rendering: g = (k.u_obs)/(k.u_emit); shift wavelength by g and scale intensity by g^3 for I_nu/nu^3 invariance. Write analytic unit tests (Schwarzschild redshift, moving emitter). |
| Analytic validation cases | - | ipole `tests/analytic/analytic_test.py`, `tests/thin_disk`, `tests/isothermal_sphere` [V: files exist] | Use as regression targets for the GR module. |

## 3. Gaps (no confirmed open reference)

- Moment-based spectral uplifting (Peters 2019, Todova 2021).
- Bounded VNDF and spherical-cap VNDF in a permissive-licence engine (only RTXPT).
- Spectral thin-film with Airy summation per wavelength inside a spectral engine
  (Cycles/Adobe are RGB-channel versions; pbrt/Mitsuba have none).
- A single "relativistic Doppler plus wavelength shift" implementation for a
  spectral path tracer. GR codes work in frequency space, not hero wavelengths.
- Spectral GR emission-line or atomic emissivity tables (no repository checked).
- Stokes/polarised rendering in a general path tracer: only Mitsuba (`mueller.h`).

## 4. How to use these effectively

1. **Name two sources per algorithm task**: the paper section and one file path
   from section 2. The task brief quotes both. In code, cite both in a comment
   (paper, equation number, file and function you checked against).
2. **Check the licence row before opening a file for reuse.** Apache/BSD/MIT: you
   may adapt, keeping the notice. GPL-3 sources (RAPTOR, GYOTO, GRay2, Odyssey,
   BlendLuxCore): read the paper and the physics, write your own, never copy.
   RTXPT and RTXDI: read only. Record the decision once the project licence is
   chosen (a decision record).
3. **Re-derive instead of transliterating** when licence forbids copying or when
   the source is built on a different abstraction (pbrt's SOA macros, Cycles'
   `ccl_device`, Slang interfaces). Re-derive, then compare.
4. **Verify against the source's own tests, not only against its renderings.**
   Port pbrt's chi-squared and energy tests (`bsdfs_test.cpp`) or run Mitsuba's
   `chi2.py` on the Astroray BSDF; run ipole's analytic tests for GR. A rendered
   comparison to another engine is a sanity check only (CLAUDE.md rule 4:
   correctness is judged against physics, analytic tests and the OpenPBR spec).
5. **Treat Cycles and RGB engines as structural references only for spectral
   work.** Cycles' constants (fits, RGB thin film, LUT approximations) are RGB
   approximations. For spectral material code, read pbrt/Mitsuba/Adobe first.
6. **Record divergences.** When Astroray differs from a reference (for example
   pbrt's peak-normalised blackbody, Cycles' F82 tint), write why in the code
   comment and in the test.
7. **Pin the commit** you read (clone SHA in the research note). Branches such as
   pbrt `master`, Cycles `main` and Falcor `master` move.
8. **Offline lookups.** If a lookup table is needed (rgb2spec, GGX energy LUT,
   LTC), regenerate it from the published optimiser or precompute tool and
   document the generation command; do not paste large data headers from a
   source with a different licence.
9. **Fail loudly on unsupported inputs** (project rule 7), as pbrt does for
   unsupported spectrum types, and do not silently fall back to the RGB path.

### 4.1 Which to clone locally (read-only, shallow)

Use `git clone --depth 1 --filter=blob:none` and sparse-checkout where noted.
Sizes are history sizes from GitHub [V]; a depth-1 checkout is far smaller.

| Clone | Why | History size |
|---|---|---|
| mmp/pbrt-v4 | Primary spectral/wavefront reference (skip submodules except if you need rgb data) | 14.5 MB |
| blender/cycles | Production GPU integrator, SVM, light tree, Blender glue (or sparse `intern/cycles`) | 22 MB |
| adobe/openpbr-bsdf | OpenPBR lobes with code | 0.4 MB |
| AcademySoftwareFoundation/OpenPBR | Spec and reference .mtlx | 69 MB (images); sparse-checkout `reference/`, `index.html` |
| mitsuba-renderer/mitsuba3 (+ drjit) | Volumes, chi-squared harness, sky | 31 MB + 11 MB |
| NVIDIAGameWorks/Falcor | ReSTIR PT, light BVH, hair; sparse `Source/Modules`, `Source/Falcor/Rendering`, `Source/RenderPasses/PathTracer` | 41 MB |
| AFD-Illinois/ipole | Permissive GR polarised transfer | 2 MB |
| c-white/blacklight | Public-domain GR code | 0.6 MB |
| zelinzh/KPolaris | Modern GPU polarised GRRT, BSD-3 | 0.9 MB |
| OpenPathGuidingLibrary/openpgl, RenderKit/oidn | Guiding, denoising (usually link, not clone) | 14 MB, 35 MB |
| McNopper/Harmonia | Small Slang OpenPBR, second opinion | 1.5 MB |
| mitsuba-renderer/rgb2spec, PetrVevoda/pragueskymodel | Tiny single-purpose references | <1 MB each |

Clone only for reading, and keep them outside the repo (for example
`C:\refs\`). Do not clone: RTXPT/RTXDI (licence; read on GitHub if needed),
GPL GR codes unless a task explicitly needs to run them as an oracle (RAPTOR
95 MB, GYOTO 83 MB), Embree and OpenVDB (278 MB and 130 MB; read single files on
GitHub or link them), MaterialX (237 MB; sparse-checkout `libraries/` and `source/MaterialXGen*`),
BlendLuxCore (358 MB, assets), LuisaRender and Hyperion (500-800 MB histories).
Nothing here requires a full clone; fetching single files by raw URL works.

## 5. Not verified

- Licences not opened in full: Bruneton precomputed atmosphere (repo field
  only), Optix_Apps beyond the header, tungsten, OIDN training weights.
- Paper details: every author/year listed [R] came from memory; only papers cited
  in source comments (Burley 2020, Heitz 2018, Chiang et al., Belcour and Barla,
  Miller 2019, Kulla and Conty, Hosek-Wilkie) are [V]. DOIs and exact titles
  were not checked.
- No repo was cloned, built or run; all behaviour notes beyond the quoted lines
  are from reading small excerpts or grep hits.
- Canonical home of Cycles (projects.blender.org) was not fetched; GitHub
  mirrors were used.
- KPolaris's maturity and arXiv number, Hyperion/Harmonia quality, LuisaRender
  hero-wavelength correctness: not assessed.
- Whether Cycles' sync code in the standalone repo exists: `src/blender` is absent
  there; the Blender-integration code was verified only in the blender/blender
  mirror.
