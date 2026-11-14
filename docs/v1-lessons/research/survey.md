# Surveys: other render engines, recent rendering research, the Spectra engine, product direction and gaps, accumulated knowledge

Source: the v1 research notes, as digested. Generalised so it reads without any knowledge of v1's code. Items marked (unverified) were unverified in the v1 notes themselves. Where the digests were thin, this file says so.

## Established findings

### Techniques other engines pioneered (ranked adoption list from the v1 survey)
1. **Chi-square sampler validation.** Mitsuba 3 and pbrt-v4 both ship a statistical test that draws samples from a BSDF, bins them, and compares against the integral of the evaluated density. It catches sampling/eval/pdf mismatches that a render-level comparison hides. pbrt-v4 constants: 80x160 bins, 1e6 samples, alpha 0.01, minimum expected frequency 5, 5 runs. Adopt as a gate for every sampled lobe.
2. **Light groups by emission mechanism.** LuxCore keeps radiance groups (8 on GPU). Labelling emitters by physical mechanism (thermal disk, synchrotron jet, starfield, environment map) gives per-mechanism AOVs, which is directly useful for science output.
3. **Zero-knob adaptive sampling.** Cycles' scheme (adaptive_sampling.h) needs no user tuning. Use a scalar-luminance half-buffer for the error estimate so large framebuffers (about 8 GB at the scale considered) are not doubled. Dammertz et al. (WSCG 2010) is the origin of the half-buffer idea.
4. **Host-mapped memory fallback.** Cycles allocates device-visible pinned host memory (cuMemHostAlloc with the DEVICEMAP flag) when VRAM runs out, so large scenes degrade rather than fail.
5. **Spectral sensor response functions (SRF).** Mitsuba 3's spectral film models arbitrary sensor curves; the route to instrument-like observables.
6. **Full light-path-expression (LPE) automata.** OSL's LPE subsystem compiles expressions to a DFA. Wavefront-friendly form: one small integer of DFA state per path, advanced once per path event.
7. **Sparse-texture demand loading** (NVIDIA OptiX Toolkit DemandLoading).
8. **Differentiable rendering: horizon only.** Retrofitting automatic differentiation onto hand-written CUDA kernels "is not realistic"; a Dr.Jit-style engine would be a ground-up design choice, not an add-on.

### Materials and microfacet energy
- Cycles removed its stochastic Heitz multiscatter GGX and replaced it with Turquin-style albedo scaling from precomputed LUTs (Blender commit 888bdc1, PR blender/blender#107958, `intern/cycles/app/cycles_precompute.cpp`). Reason: stochastic evaluation was 7-15x slower in Mitsuba; albedo scaling is deterministic and cheap. The v1 recommendation was to port Turquin-style compensation for reflection.
- Without compensation, single-scatter conductors at GGX alpha=1 lose about 60% energy (over 90% for GTR/STD) (unverified-extracted).
- `adobe/openpbr-bsdf` (Apache-2.0) is a portable C++/GLSL/CUDA/MSL/Slang implementation with 7 multiscatter LUTs: a ready reference for OpenPBR.
- "OpenPBR layering is the mechanism for energy-preserving multiscatter" was refuted (0-3); layering is a different thing from multiscatter compensation.
- Thin film: treat as a self-contained material feature (OpenPBR defines thin film, coat and fuzz).

### Specular and caustic transport (richest adoptable cluster, 2023-2026)
- 23 claims confirmed, 2 refuted in the sweep. Candidates: Specular Polynomials as a seed finder for specular manifold sampling (Fan et al. 2024); PSMS-ReSTIR (Hong et al. 2025); manifold path guiding (Fan et al. 2023, up to 40x variance reduction on long specular chains); 3D-Gaussian photon guiding (no public implementation).
- Layered BSDFs: Guo, Hasan, Zhao (2018) position-free layered evaluation.

### Guiding and resampling on a GPU wavefront
- Refuted (0-3): "OpenPGL is built for GPU wavefront integration". OpenPGL is CPU-oriented (Apache-2.0, v0.7.1 in 2026, ASWF sandbox).
- SVO-based guiding (Yalcıner and Akyuz 2024) has a fixed memory footprint (about 3.96 MiB) against 19 to 37 MiB for an SD-tree.
- ReSTIR PT / GRIS (Lin et al. 2022) is the formal basis for reservoir reuse in a wavefront design. Its unbiased recipe: temporal reuse off, 32 candidates per pixel, 3 spatial rounds, 6 neighbours, 10 px radius.
- The 2026 ReSTIR cluster is the headline of the 2026 sweep (see Sources). Nothing there had usable open code; use as ideas only. RTXDI is proprietary (see the licensing conflict below). NVIDIA SER is OptiX-only and likely inapplicable.

### 2026 sweep: other findings
- No 2023-2026 successor to hero-wavelength spectral MIS (Wilkie 2014) was found, and no 2025-26 spectral-MIS paper. No 2026 firefly-suppression or volume-guiding papers either. Treat as absence of evidence, not proof.
- Owen-scrambled RQMC: confidence intervals need replicate scrambles; Student-t intervals had 3 failures in 2400 at R=5, bootstrap-t 81 (arXiv 2608.17143). Implication: convergence tests should use R >= 10 scrambles.
- Ray-origin offset: Mitsuba 3.10-dev derives offsets from per-hit error bounds (`p_err`) instead of fixed epsilons. Candidate replacement for hand-tuned epsilons.
- Null/transparent-BSDF shadow rays in Mitsuba use an explicit transmittance evaluation (`eval_null()` via `ray_test_tr`).
- Cycles 5.2 (released 2026-07-14, LTS): no spectral, light-sampling or denoiser changes (3/3 confirmed). New: negative SSS anisotropy, Random Walk radius rescale (PR#133848), Principled Thin Wall. Blender Sky: Multiple Scattering is the only non-legacy type; Sun Disc is Cycles-only. Cycles 5.3 beta (unverified): anisotropic glass (#163343), dispersion (#162041), DLSS viewport denoiser (#153077).
- OIDN releases: 2.3.3 (Blackwell), 2.4.0 (CUDA 13, Volta dropped), 2.5.0 (semaphore import), 2.5.1 (leak fix). NanoVDB 13.1.0 (2026-09-16): ReadAccessor fix, `getDimAndActive()`.

### Choosing comparison renderers (Cycles is RGB and cannot arbitrate spectral effects)
- Install Mitsuba 3.9.1 (BSD-3; PyPI mitsuba 3.9.1, drjit 1.5.0; mitsuba-blender extension, Blender >= 4.2) first as the spectral oracle. pbrt-v4 (Apache-2.0, github.com/mmp/pbrt-v4) CPU-only second as tie-break. LuxCore (BlendLuxCore v2.11.1) optional third.
- Skip Arnold, V-Ray, RenderMan, Karma, Octane: licence terms bar publishing benchmark numbers. Appleseed and Tungsten are unmaintained.
- Cycles RGB cannot arbitrate dispersion, chromatic media, or line emitters.
- Rule proposed: if Mitsuba and pbrt agree within 3 sigma and Cycles differs, mark "RGB reference limit".
- Arbitration scenes designed: SF11 prism with floor caustic; chromatic homogeneous medium under a rectangle lamp; narrow-band emitter on a coloured wall. Metrics: relVar, chroma relVar, relMSE split into variance and bias squared, top-0.1% variance share, N x relVar slope over 4/16/64/256 spp. Equal-time budgets 2/10/60 s with spp differencing; efficiency = 1/(relMSE x t); min-of-3.
- Small frames under-fill a GPU (a camera-geometry kernel read 16x slower at 320x180 and 3.8x at 1280x720 than at saturation): time at >= 1280x720.

### Licensing posture (carried from v1; re-decide for the rewrite)
- v1 targeted MIT or Apache-2.0. Link Apache/BSD/MIT; LGPL dynamic only; GPL code can only be cross-checked (GYOTO, GRay2 GPL-3, CLOUDY, ART). Blender sky sources: single scattering Apache-2.0, multiple scattering MIT, only two small headers GPL.
- Free/public data: NIST ASD (public domain), RefractiveIndex.info (CC0), USGS/ECOSTRESS (public domain). ExoMol is CC BY-SA 4.0 (owner allowed bundling in v1). HITRAN/HITEMP not bundled. Kurucz and CHIANTI: acknowledgement only.
- **Conflicting evidence on RTXDI:** one older v1 note said the RTXDI SDK is MIT and its kernels could be ported; the later PBR note (licence file section 4(e)) and the 2026-10 sweep say it is proprietary and disqualified. The newer evidence wins; re-check before any use.

### Spectral and physical modelling facts
- Four hero wavelengths over 360-830 nm sampled uniformly; for wavelength-dependent bounces (dispersion) collapse to the hero wavelength by zeroing the other PDFs and dividing the hero PDF by 4 (as pbrt-v4).
- **Eta-squared footgun (important):** Jakob-Hanika RGB-to-spectrum upsampling clamps reflectance to [0,1]. Dielectric transmission scales radiance by eta^2 (2.25 at IOR 1.5), which exceeds 1. Upsample only the normalised tint and carry the >1 scalar separately. Upsampling `albedo*cos/pi` directly darkened glass on both CPU and GPU. Related: the Jakob-Hanika map is nonlinear in magnitude, so scaling an upsampled spectrum is not the same as upsampling a scaled colour.
- Thin film: Belcour and Barla 2017, evaluated per wavelength.
- Volumes: heterogeneous media use the pbrt-v4 hero-wavelength spectral MIS tracker over NanoVDB; blackbody emission from a shared log-space Planck lookup. Only the nearest medium per ray segment was tracked in v1 (a limitation, not a design).
- Melanin hair absorption anchors: Jacques 2013 eumelanin sigma_a = 6.6e11 lambda^-3.33 cm^-1 (DOI 10.1088/0031-9155/58/11/R37); Donner and Jensen 2006 pheomelanin sigma_a = 2.9e14 lambda^-4.75 mm^-1; both anchored at 550 nm to Cycles' green melanin for magnitude parity.
- Spectral data semantics adopted: emission L(lambda) = sum_i w_i P_i(lambda; T_i, P_i); absorption sigma_a = sum_i n_i sigma_i; reflectance is never summed. LTE is wrong for nebulae (Case B recombination needed). A 5 nm grid cannot resolve the Na D doublet (0.6 nm apart); the v1 spectral profile table was 441 samples over 300-2500 nm at 5 nm, 47 entries.

### Product direction (v1 owner decisions; the part that outlives v1)
- North star: research-grade astrophysical spectral rendering driven from Blender. Near-term bar: a production-capable Blender renderer. Priority order: correctness, then visual fidelity, then speed (viewport speed was co-equal as a product goal).
- Physics-first rule: where Cycles is approximate, implement the physically correct model and treat Cycles as a cross-check band. Rough-glass reference: Heitz 2016 random-walk oracle.
- Denoising stays out of the interactive loop; F12 final render pauses the viewport session.
- Socket coverage gate was frequency-weighted (how often a socket appears in real production scenes), capped at 3 scenes per socket, target >= 95% with zero silent drops.
- Complex node trees ranked the most important product theme. Native Blender UI surfaces only, no separate sidebar app. Element/molecule spectra and data-cube output are science foundation, built after the production bar.
- Production-polish bucket (unsequenced): multi-GPU (deferred; 4x RTX 6000 target), OIDN 3 temporal and AOV denoising, optional OptiX denoiser plugin (quoted 100-200 ms/frame), multi-layer OpenEXR (non-negotiable), viewport session, motion blur (camera and object, box/triangle shutter), hair as curves converted to triangles at export, docs, example scenes. Acceptance bar: multi-GPU identical to single within noise; OIDN 3 parity with OptiX; EXR round-trip test; viewport responds within 100 ms of edits.

### Data-cube output (product theme)
- 1920x1080 x 64 bins x float32 = 531 MB per cube (about 1 GB with variance). A scalar AOV is about 8 MB. Recommendation: FITS cube (CTYPE3='WAVE', CUNIT3='nm') plus multi-layer EXR; HDF5/Zarr only on request. Values relative, with provenance; no absolute-units claim.
- Missing in v1: sample-count, variance, ray-count and path-depth AOVs.

### Generic rendering lessons from v1 postmortems (not v1-specific)
- Apply gamma once, at final write. Furnace/energy gates must render linear and check an upper bound: gamma fakes a stable 1.8-2.4x false divergence, and clamping can hide an energy gain (a linear 4.14 read as clamped 1.000).
- Compare renders to a high-spp reference, not to each other; MSE between two noisy renders is unreliable because of fireflies.
- Open sky gives mean brightness about 0.4 after gamma, so "no light = dark" tests are wrong.
- Keep path depth identical across every entry point (depth 50 vs 8 caused runaway near-TIR caustic fireflies). Values used then: per-bounce throughput clamp 10x, Russian roulette survival 95%, per-sample luminance clamp 20.0 (a crude firefly control, not a recommendation).
- NEE double cosine (the NdotL factor applied twice) caused about 2x overexposure; the BSDF-sample branch is the reference convention.
- Reflection direction bug pattern: `wo - 2(wo.n)n` is -wi; correct is `2(wo.n)n - wo`.
- Near-zero GGX roughness (below about 0.08) collapses when an additive epsilon dominates; v1 raised the delta threshold from 0.01 to 0.08 and floored alpha at 0.0064. A cleaner design: one D formula with pdf = D*NdotH/(4*HdotV), used by eval and pdf alike (mismatched epsilon placement made them disagree).
- Guards defeated by earlier clamping (a back-face check made dead because NdotL/NdotV were clamped first); uninitialised pdf on below-surface samples; image rows flipped by a wrong v mapping.
- Blocky brightness suggests correlated RNG per region.
- Progress callbacks inside an OpenMP parallel region are not thread-safe.
- Debug method that worked: add one material type at a time to a walls-plus-light scene (Metal alone gave mean 1.0 vs about 0.4).
- Toolchain hazards (relevant if the rewrite uses MinGW or fast-math): MinGW GCC 15.2 corrupted structs over about 32 bytes passed by value (pass const reference); `-ffast-math` makes `std::isfinite` fold to true, silently disabling NaN/Inf guards (use `-fno-finite-math-only` and a bit-pattern check); stale binaries earlier on the import path shadow fresh builds (verify the loaded module's file path before trusting any bisection); OneDrive-synced paths made fopen fail silently; a standalone binary that ignores unknown flags and exits 0 let tests pass on wrong output.

## Sources and reference implementations

External-engine files are listed exactly as the notes recorded them.

| Source | Detail |
|---|---|
| Mitsuba 3 chi-square harness | `src/python/python/chi2.py` (BSD-3) |
| pbrt-v4 BSDF tests | `src/pbrt/bsdfs_test.cpp` (Apache-2.0) |
| Heckbert, SIGGRAPH 1990 | DOI 10.1145/97879.97895 |
| OSL LPE | `src/liboslexec/{lpeparse,lpexp,automata,accum}` (BSD-3) |
| LuxCore light groups | Apache-2.0, 8 groups on GPU |
| Manuka, Fascione et al., ACM TOG 2018 | DOI 10.1145/3182161 (PDF too large to fetch; sensor details unread) |
| Wilkie et al., EGSR 2014 | DOI 10.1111/cgf.12419 (hero-wavelength spectral MIS) |
| ART renderer | GPL, literature only |
| Mitsuba 3 spectral film | `specfilm` (BSD-3) |
| Cycles host-mapped alloc | `src/device/cuda/device_impl.cpp`, `shared_alloc()` |
| NVIDIA OptiX Toolkit DemandLoading | BSD-3 per README (unverified; LICENSE.txt not read) |
| Jaros et al., ACM TOG 40(2), 2021 | DOI 10.1145/3447807 (abstract only) |
| Dammertz et al., WSCG 2010 | adaptive sampling with half-buffer error |
| Christensen et al., TOG 2018 | DOI 10.1145/3182162 |
| Christensen, Kensler, Kilpatrick, CGF/EGSR 2018 | progressive multi-jittered sequences |
| Cycles adaptive sampling | `src/kernel/film/adaptive_sampling.h` (Apache-2.0) |
| Dr.Jit, Jakob et al., TOG 2022 | arXiv 2202.01284 |
| Nimier-David et al., SIGGRAPH 2020; Vicini et al., SIGGRAPH 2021 | radiative backprop; path replay backprop |
| Jipole | arXiv 2509.07065 (GR polarised transport); sensitivities arXiv 2604.11869 |
| Turquin multiscatter compensation | blog.selfshadow.com ms_comp_final.pdf |
| Kulla-Conty 2017; Heitz 2016 | multiscatter microfacet |
| Cycles multiscatter change | Blender commit 888bdc1; PR blender/blender#107958; `intern/cycles/app/cycles_precompute.cpp` |
| `adobe/openpbr-bsdf` | Apache-2.0, 7 multiscatter LUTs |
| OpenPBR, Portsmouth, Kutz, Hill | arXiv 2512.23696 (Apache-2.0 spec) |
| pbrt-v4 | Apache-2.0; `src/pbrt/util/spectrum.h/.cpp`, `color.h` (port idea, not code) |
| Mitsuba 3 spectra | `src/librender/spectrum.cpp` |
| Specular Polynomials, Fan et al., SIGGRAPH 2024 | arXiv 2405.13409; github.com/mollnn/spoly ("MIT-style", unverified) |
| PSMS-ReSTIR, Hong et al., SIGGRAPH Asia 2025 | DOI 10.1145/3757377.3763927; Utah-Graphics-Lab/PSMS-ReSTIR (Falcor, BSD-3 to verify) |
| Manifold path guiding, Fan et al., ACM TOG 42(6) | DOI 10.1145/3618360, arXiv 2311.12818; github.com/mollnn/manifold-path-guiding (BSD-3 to verify) |
| 3D-Gaussian photon guiding, Huang et al. | arXiv 2403.03641 (no public implementation) |
| Position-free layered BSDF, Guo, Hasan, Zhao, TOG 2018 | DOI 10.1145/3272127.3275053 |
| SVO wavefront guiding, Yalcıner and Akyuz, Computers & Graphics 121 (2024) 103945 | arXiv 2405.06997 (no confirmed repo) |
| ReSTIR PT / GRIS, Lin et al., TOG 41(4) | DOI 10.1145/3528223.3530158; github.com/DQLin/ReSTIR_PT (licence to verify) |
| ReSTIR PG, Zeng et al., SIGGRAPH Asia 2025 | DOI 10.1145/3757377.3763813 (no open implementation) |
| OpenPGL | Apache-2.0; v0.7.1 (2026-04-08) |
| ReSTIR PT Enhanced, lin2026, I3D May 2026 | 2-3x faster claim, no code |
| ReSTCV | github.com/Hercier/ReSTCV: BSD-3 base ("Daqi Lin's ReSTIR PT code") plus CC-BY-4.0 (LICENSE.md); Falcor/D3D12, ideas only |
| SPMIS (EG 2026); Gradient-domain ReSTIR (EG 2026) | no licence; github.com/elite-sheep/gradient-restir |
| Compatibility-Guided Neighbour Selection | HPG 2026 best paper (tie) |
| RCPG, TOG 46(2) | no code; about 2x paths per pixel (throughput, not variance) |
| Mitsuba 3.10-dev | `p_err` ray offsets; `eval_null()` via `ray_test_tr` |
| Owen RQMC | arXiv 2608.17143 |
| Q-ART, C&G 139 (2026) | base-4 LUT about 1 KB |
| Kratos-linerad | arXiv 2608.09340 |
| KPolaris | arXiv 2609.26298 (Kokkos GR polarised RT) |
| TARTARUS | arXiv 2609.08989 |
| Hybrid neural-microfacet, EGSR 2026 | arXiv 2608.09604; MERL MAE 0.0051 vs GGX 0.0099; 2.94 ms vs 1.10 ms GGX at 1080p, RGB only, no code |
| Dupuy | arXiv 2609.20394 (closed-form quadratic NDF, not GGX) |
| Jakob-Hanika 2019 | RGB-to-spectrum; coefficient tables on Zenodo; `simple-spectral` LUT reader |
| Data and libraries | CIE CMFs (CVRL); RGL and MERL BRDF databases; tiny-cuda-nn (BSD-3); instant-ngp |
| Cycles procedural textures | `intern/cycles/kernel/svm/{gradient,magic,checker,wave,brick,voronoi,noisetex}.h` |
| Chiang 2016, CGF 35(2) | Cycles `bsdf_principled_hair_chiang.h`; pbrt-v3 `hair.cpp` (BSD-2) cross-check |
| Jacques 2013; Donner and Jensen 2006 | melanin coefficients above |
| Other Cycles files read | `shade_volume.h`, `ies.cpp`, `spot.h`, `closure.h`, `bsdf_microfacet.h`, `bsdf_util.h`, `path_state.h` |
| Miller et al. 2019 | TOG 38(4) 44 |
| I/O libraries | CFITSIO; EleFits (LGPL-3); HighFive (BSD-3); OpenEXR (BSD-3); OIDN (Apache-2.0) |
| GR cross-checks | GYOTO, GRay2 (GPL-3; Chan et al. 2018, arXiv 1706.07062 for the Kerr axis chart), CLOUDY: cross-check only |
| Wave optics | wave_tracer (Steinberg and Pharr, "Wave Tracing"); BEMsim3D (boundary-element full-wave) |
| Belcour and Barla 2017 | thin film |
| Comparison tooling | PyPI mitsuba 3.9.1, drjit 1.5.0; github.com/mmp/pbrt-v4; BlendLuxCore v2.11.1 |
| Blender sky | Blender `intern/sky` (Nishita) |
| Spectra web footprint | spectra-render.com; valorcatstudios.com; bark.com; IMDb nm6005405; youtube.com/c/valorcatstudiosinc; developer.blender.org Cycles 5.2 notes; devtalk Cycles meetings 2026-10-13 and 2025-02-04; devtalk "thoughts on making cycles into a spectral renderer"; projects.blender.org PR 121023; render.otoy.com forum posts 443775 and 443729; docs.otoy.com Blender page; indigorenderer.com/blender; Maxwell forum 392361; mitsuba-renderer.org plugins; blender-addons.org/mitsuba; stkrake.net/blRstr; github kalexis1994/ignis-rt |

## Measured numbers worth keeping

Most numbers in these notes are from surveys, not measurements; those are flagged. Measured items:
- v1 production-readiness snapshot (2026-09-07), as a sense of scale only: viewport in-process p99 37.7 ms (bypassing Blender's GPU blit); with OIDN in the loop p50 140.8 ms, p99 184.8 ms; idle progressive refine about 1.3 Hz; real present latency unmeasured.
- Socket coverage in v1: 152 supported, 35 approximated, 340 silently dropped of 527 (64.5%).
- Cornell box GPU vs Cycles: SSIM 0.9538; mean ratio 0.9967/0.9975/0.9944; 1477 ms vs Cycles-CUDA 3134 ms. Checker texture std CPU 0.4182 vs GPU 0.0330 (a CPU/GPU disagreement that went undetected: shows the need for CPU/GPU parity tests on procedural textures).
- Node-tree capacity limits in v1's shader program (a cautionary scale): 32 instructions, 8 registers, 2 textures, 2 ramps, one mapping. Noise texture correlated 0.73 with Cycles' version. Predicted about 2 of 8 production materials in a quality band.
- Register pressure: fused shading kernels at 254 registers; extra per-hit state spilled and caused up to +52% regression on unrelated scenes. Check with `cuobjdump --dump-sass`, not ptxas alone.
- v1 scene re-upload per viewport edit about 125-132 ms; incremental replay rendered stale data.
- Procedural textures baked to 64x64 (UV) and 64^3 (generated/object) was a v1 compromise.
- Competitor: Spectra claimed 1000x1000 in under 8 s on an RTX 3080 Mobile (a forum claim, 64 spp, 12 bounces, studio HDRI, AgX).
- Survey claims, unverified: ReSTIR PT Enhanced 2-3x (no baseline); manifold guiding up to 40x on long specular chains; hybrid neural microfacet 2.94 ms vs 1.10 ms at 1080p; positivization MSE 0.079 to 0.019 at 39.8 ms.
- 435 collected tests in the v1 suite on 2026-05-09 (scale of the test effort only).

## Dead ends and negative results

- Retrofitting AD onto hand-written CUDA kernels: not realistic.
- Stochastic Heitz multiscatter: 7-15x slower; Cycles itself abandoned it.
- Looking for a post-2014 hero-wavelength successor: none found in 2023-2026 sweeps (the verifier phase completed 0/75, an infrastructure failure, not a refutation).
- Using OpenPGL directly on a GPU wavefront: refuted. RTXDI: proprietary (disqualified).
- Comparing two noisy renders via MSE; step-count bisection on a stale binary (an entire "window" of 4500/4900 steps was stale-binary noise); `-ffast-math` silently disabling NaN guards.
- A megakernel was eventually removed in favour of the wavefront split; caller-less kernels were deleted. Details are v1-structure-specific.
- A fully RGB architecture was replaced by spectral-only; light transport must live in the spectral entry points, not in RGB paths.
- Incremental scene replay for viewport edits rendered stale data; a full re-sync was safer.
- Chasing sub-percent parity tails against Cycles was ruled out by the owner.
- Not proposed after review: neural BRDFs, neural texture compression, PureSample, offline gradient ReSTIR, LoD ReSTIR.
- Astro-science work (Kerr/GR, accretion, FITS/HDF5 ingest, telescope PSF) was frozen until the production gate; do not read it as a research failure.
- A "material zoo" asset for hair under HDRI turned out to be a byte-identical placeholder: a gate passed on a fake input.

## Design ideas worth reconsidering in the rewrite

- Chi-square validation of every sampled lobe, in CI, using the pbrt-v4 constants above.
- LPE as a per-path small-integer DFA state; emission-mechanism labels as light-group ids; photon-ring winding counter as extra path state (for GR/lensing science).
- Scalar-luminance half-buffer for adaptive sampling (halves the memory cost of the error estimate).
- Turquin albedo-scaling multiscatter with precomputed LUTs; the OpenPBR reference already ships the LUTs.
- GRIS for ReSTIR-to-wavefront reservoirs; ReSTIR PT as a viewport-only option; Specular Polynomials as a manifold seed finder.
- Per-hit error bounds replace fixed ray epsilons.
- Explicit transmittance for null/transparent shadow rays; audit that shadow rays through transparent surfaces are right.
- Replicate-scrambled RQMC (R >= 10) for noise benchmarks, to separate bias from variance honestly.
- Equal-time efficiency metric 1/(relMSE x t) with chroma variance reported separately: the right metric for a spectral renderer.
- Demand-loaded textures and host-mapped memory fallback for scenes exceeding VRAM.
- Spectral film with sensor response functions as the observable pipeline (instrument-like outputs); FITS cube plus multi-layer EXR as outputs.
- A cheap experiment before adopting heavier hero-wavelength machinery: A/B chroma variance on dispersive scenes (prism, BK7) against equal-cost configurations with more paths and fewer wavelengths.
- Dielectric bounce-cap policy: Spectra advertises a "Dielectric Graceful Exit" after bounce 8 ("similar to Redshift's refraction limit"); compare with a dead-sample redistribution that moves lost mass into transmission.
- Publish node-parity tables as a trust-building asset; users distrust unverified renderers (vibe-coding suspicion, closed licences, watermarked alpha builds).
- Reusable test-scene recipes derived from competitor claims: double-slit as a negative control for any wave-optics module (650 nm, 20 um slits on 100 um pitch); thin-film ladder at 300/600/900 nm against analytic Airy; dispersive crystal (diamond n about 2.41) with a prism sub-test; soap bubble furnace check (shell IOR 1.33, film 300-600 nm); CC0 classroom and CC-BY Junk Shop Blender demos as shared scenes; procedural corridor as a stand-in for non-public sci-fi interiors; skin/SSS close-up; shader-ball node-parity bench (21 materials, cheapest and highest signal); tumbler bounce-cap sweep (4/8/12); iridescent bunny (thin film); car paint; 532 nm wavelength-node sphere; macro close-up DoF.

## What no longer applies

- Pixel parity with Cycles, parity-tail chasing, SSIM and mean-ratio Cycles gates: superseded by physics and OpenPBR conformance.
- Cycles-specific approximations as targets (RGB-only dielectric handling, Cycles' stochastic quirks): keep only as knowledge of Blender semantics.
- v1 socket-coverage counts and the shader-program limits: tied to v1's node-evaluation design.
- v1 viewport gate numbers (measured bypassing Blender's blit): the instrument was wrong; re-measure real present latency.
- Texture bake resolutions and "fall back to base colour with more than two textures": v1 compromises.
- Build/packaging lessons about module naming, pybind11, stale module shadowing, Blender embedded Python version: v1 build structure (the verify-what-is-loaded habit is still worth keeping).
- Wavefront register-spill specifics (template isolation, 254 registers, side-table tricks): symptoms of v1's fused shading kernels.
- Test-infrastructure conventions (assert vs return, matplotlib Agg, conftest paths): v1 tooling.
- v1 work-package ordering, process machinery, and agent routing.
- The 4x RTX 6000 multi-GPU target: deferred in v1 until science shipped; revisit only if the rewrite plans it.

## Open questions

- Does hero-rotation MIS cut chroma variance on dispersive paths? No successor paper exists; needs an experiment.
- Which multiscatter reference do we adopt (Turquin LUT from OpenPBR, Kulla-Conty, Heitz oracle)? v1 chose Turquin for reflection and the Heitz random walk as the rough-glass oracle.
- Licences unverified in v1: OptiX Toolkit DemandLoading (BSD-3 per README only), appleseed MIT, Jakob-Hanika tables, the Cycles file owning host-texture movement, the mollnn and ReSTIR PT repos, Falcor. The RTXDI conflict is resolved against it.
- Cycles 4.x/5.x kernel feature list: never reviewed directly in Blender source; 5.3 beta items unverified.
- Mitsuba `cuda_ad_spectral` on sm_120 untested; pbrt-v4 GPU build deferred; a maintained pbrt-v4 Blender 5 exporter unknown; LuxCore spectral support unknown.
- Unread: Manuka paper, Ghost ReSTIR poster (ACM 403), Q-ART paper, Blender #156437, I3D 2026 titles; Karma, V-Ray, Octane, Redshift 2026 notes are missing.
- Spectra (a competitor; all claims vendor-only and unbenchmarked):
  - Claims: single-page site (every subpage 404); four hero wavelengths stratified over 380-780 nm plus full Stokes/Mueller polarisation; wavelength-dependent IOR, thin film, gratings and edge diffraction driven from the Principled BSDF; Vulkan KHR ray tracing (Vulkan 1.3); Blender 5.1+ (5.2 LTS recommended); DLSS Ray Reconstruction on RTX, OIDN elsewhere; NVIDIA validated, AMD/Intel experimental, Apple via MoltenVK; "ReSTIR-WO (patent pending)"; closed core plus GPL shell with per-machine perpetual licence. The Blender Artists thread describes a Rust + Vulkan + Slang addon with an out-of-process daemon, Cauchy dispersion node (IOR 1.517, dispersion 0.008), thin film, blackbody, car paint, ReSTIR BDPT plus photon caustics (4M photons max, 8 caustic bounces, inferred from UI), Windows only, Blender 4.0-5.2. The thread product may differ from the website one.
  - Footprint is tiny: 24 posts, 930 views, 4 outside users; no hits on YouTube, Reddit, GitHub, Superhive. Likely a very early closed beta. Strongest distinct claim: wave optics plus polarisation; Mitsuba 3 is the only comparable with polarisation and is not real-time. Inferred parity sheet: about 9 of 21 node rows close to Cycles, 12 differ or fail (bump, Voronoi plus bump, XYZ separate/combine, vector math dot/normalize, RGB curves hue, sheen). A devtalk snippet says ReSTIR plus SVGF is "almost always slower" at equal quality (unverified).
  - Unknown: RGB-to-spectrum upsampling method, bin count and wavelength PDF; what "unbiased" means with ReSTIR plus denoisers; whether F12 is denoiser-free; node compatibility; volumes, hair, AOVs, motion blur; price and release date.
- Other-engine status to verify from vendor docs before any public comparison: Octane (2026.4, neural radiance cache), Maxwell, Indigo (about GBP 525-630), LuxCore 2.10 (May 2025), blRstr (up to 15 indirect bounces), ignis-rt (12 stars).
- User asks seen on the Spectra thread (market signal): presets, easy SSS, lamp falloff, bloom/chromatic aberration, licence transparency, a test build. Nobody asked for GR, volumes, hair, Linux, AMD or animation.
- Largest v1 feature gaps (check whether the rewrite's OpenPBR plus node-graph design avoids them by construction): missing per-hit input nodes (Attribute, Geometry, Layer Weight, Object Info, Light Path); Add Shader keeping only the first BSDF; mixed metal/glass off by 2-4x; textured factors sampling only one texture; no responsiveness/cancellation in long renders.
