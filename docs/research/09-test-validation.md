# Test and validation strategy for Astroray 2.0

Date: 2026-11-14. Workstream: test-validation.

Tags: VERIFIED = I fetched the primary source (raw file or GitHub API) this session. Where a fetch tool summarised a file, quoted constants are as the tool reported them. REPORTED = secondary source. UNVERIFIED = background knowledge or my own derivation. "Derived" = my arithmetic, shown so it can be checked.

## Question

How do we keep the suite fast and trustworthy for a spectral, single-source (CPU and GPU) OpenPBR path tracer? That covers kernel unit tests on the CPU target, statistical sampler and BSDF tests, analytic radiometry, CPU-vs-GPU agreement, image regression, independent oracles, headless Blender end-to-end, noise-robust performance gates, and tier definitions with budgets. Budgets from CLAUDE.md rule 9 (pre-commit under 10 s, merge gate under 3 min) are treated as requirements.

## Findings

### A. PBRT-v4 (Apache-2.0, repo license VERIFIED via API)

- Tests are gtest, one executable (`pbrt_test`). Files include `bsdfs_test.cpp`, `lights_test.cpp`, `lightsamplers_test.cpp`, `samplers_test.cpp`, `util/sampling_test.cpp`, `util/spectrum_test.cpp`, `util/rng_test.cpp`. VERIFIED: https://github.com/mmp/pbrt-v4/tree/master/src/pbrt
- BSDF chi-squared (`bsdfs_test.cpp`):
  - Constants: significance 0.01, 80x160 (theta x phi) bins, 1,000,000 samples, minimum expected frequency 5 (cells below it are pooled), 5 runs, each with a random outgoing direction. VERIFIED.
  - A Sidak correction is applied with the number of runs as `numTests`. A p-value dump goes to a `.m` file for plotting. VERIFIED.
  - The reference histogram is the BSDF pdf integrated per cell, not a second sampler. VERIFIED.
  - Coverage: Lambertian, conductor and dielectric microfacets, isotropic and anisotropic, front and back. No layered, OpenPBR or spectral cases. VERIFIED: https://github.com/mmp/pbrt-v4/blob/master/src/pbrt/bsdfs_test.cpp
- Energy conservation is a one-sided bound: 10 random directions, 16,384 samples each, `Lo.MaxComponentValue() < 1.01`. It catches gain only, not loss. Only `BSDFEnergyConservation.LambertianReflection` is enabled; the microfacet energy tests sit inside `#if 0` (disabled). So pbrt has no active energy test for any microfacet, dielectric or layered lobe. VERIFIED (re-fetched in verification pass).
- Hair has white-furnace, reciprocity, sampling-weight and sampling-consistency tests. Furnace uses low-discrepancy (radical inverse) sequences, 20k to 100k samples per case. VERIFIED.
- Sampling and light-sampler tests are deterministic or QMC where possible:
  - Spot-light power is checked against closed form with a QMC sum to 1e-3 absolute (`lights_test.cpp`).
  - `Sampling.Invert*` round-trip tests check sample/invert pairs.
  - `Sampler.ConsistentValues` and `*.ElementaryIntervals` check order-independent values and stratification structure.
  - Light-sampler tests check that sampling frequency over pdf is consistent against the same sampler's `PMF()`. All VERIFIED.
- Spectrum tests use external oracles with a loose tolerance when stochastic. Blackbody is checked against four hand-tabulated values from an online calculator at 1e-3 relative error, plus Wien's law. A constant spectrum's XYZ is within 0.035 using 100 stratified wavelength samples. VERIFIED.
- Single source: `PBRT_CPU_GPU` expands to `__host__ __device__` (`pbrt.h`) and BxDFs are annotated with it. VERIFIED.
- CI (`.github/workflows`) VERIFIED:
  - CPU workflows run `pbrt_test` on Linux, macOS and Windows.
  - The CPU matrix includes `-DPBRT_FLOAT_AS_DOUBLE=True` (builds, but tests run only for the default config).
  - The GPU workflow is build-only across OptiX 7.7/8.1/9.0 and CUDA 12.9 to 13.2. No GPU test runs in CI.
- Spectral: 4 wavelengths per path. Secondary wavelengths are terminated at dispersive events (`lambda.TerminateSecondary()`, `cpu/integrators.cpp`; `SecondaryTerminated()` checked in `wavefront/surfscatter.cpp`). VERIFIED.
- `imgtool diff` supports MAE, MSE, MRSE and FLIP metrics. The film keeps a per-pixel RGB `VarianceEstimator`. VERIFIED.

### B. Mitsuba 3 (BSD-style; GitHub API reports NOASSERTION, so check LICENSE before relying)

- Layout: per-plugin `tests/` directories (`src/bsdfs/tests/*.py` has 18 files: diffuse, conductor, rough_conductor, dielectric, rough_dielectric, rough_plastic, principled, ...). `src/render/tests/` has `test_renders.py`, `test_microfacet.py`, `test_spectra.py`, `test_medium.py`, `test_phase.py` and others. Python/pytest with a `slow` marker; docs say `pytest -m 'not slow'` and a `ninja pytest` target. VERIFIED: https://mitsuba.readthedocs.io/en/latest/src/developer_guide/testing.html
- Chi-squared (`chi2.py`) VERIFIED:
  - Defaults: `sample_count=1_000_000`, `res=101` (odd, to avoid sharp-boundary precision issues), `ires=4` quadrature subintervals per cell axis, `seed=0`, PCG32 with TEA scrambling.
  - Cells below expected count 5 are pooled. The p-value comes from the regularised gamma function.
  - `run(significance_level=0.01, test_count=1)` applies a Sidak correction: `1-(1-a)^(1/test_count)`.
  - It fails outright if a cell has zero expected count but nonzero observed count, if samples fall outside the domain, or if negative weights appear.
  - The `sample_func` takes an array of explicit uniform numbers `[sample_dim, N]`. It may return `(positions, weights)` so weighted samples can be tested. On failure it dumps `chi2_data.py`.
  - Source: https://github.com/mitsuba-renderer/mitsuba3/blob/master/src/python/python/chi2.py
- BSDF tests use per-case `seed`, `res` and `ires` overrides to dodge known weaknesses. Examples: `ires=16` for the rough-dielectric transmission edge, because "the default integration resolution underestimates its mass"; the smooth dielectric chi2 test is commented out as "way too slow". VERIFIED (`test_principled.py`, `test_rough_dielectric.py`).
- Mitsuba's BSDF tests for dielectric, rough dielectric, conductor, plastic and principled contain no white-furnace or reciprocity tests found. They add an eval/pdf consistency sweep at 20 polar angles and hardcoded regression values. VERIFIED for those files. Correction: the hair BSDF does have furnace tests (`test_hair.py`: `test02_white_furnace` 1e8 samples at atol 1e-2, `test03_white_furnace_importance_sample` 1e6 samples), so "Mitsuba has none" is true only for non-hair lobes. A full-tree search for other furnace tests was not exhaustive (GitHub code search returned only `test_hair.py`).
- Render suite (`test_renders.py`) VERIFIED:
  - Scene XMLs are globbed from `resources/data/tests/scenes/*/`, with references beside them as `<scene>_ref_<mode>.exr` plus `_var.exr`.
  - References are generated by Mitsuba itself (`render_ref_images`) on variants whose names start with `scalar_` and do not end with `double` (so `scalar_rgb`, `scalar_spectral` and polarized forms), default 32,000 spp, `seed=0`, so they are self-references, not independent truth. The "moment integrator" detail was not re-confirmed in the verification pass. Without `--overwrite`, existing references are kept.
  - Test: sample budget 2e6 samples (`spp = budget // pixels`), `seed=0`, per-pixel Z = |mean - ref| * sqrt(spp / max(ref_var, 1e-4)), two-sided p-value, Sidak across pixels at 0.01, pass if at least 0.9975 of pixels pass.
  - It runs every enabled variant (so CPU, LLVM and CUDA agree with one reference) and writes `_img/_ref/_var/_error/_pvalue.exr` on failure.
  - A missing reference is a skip, not a failure. All render tests are marked `slow`.
- `check_vectorization` runs scalar vs vectorised variants on seeded inputs with `dr.allclose` at 1e-6 absolute. VERIFIED (`python/test/util.py`).
- Variants: `scalar_spectral`, `llvm_ad_spectral`, `cuda_ad_spectral`, plus `_polarized` and `_double` forms. Spectral range 360 to 830 nm. `pip install mitsuba` ships six variants including `scalar_spectral` (no `_double`). Double precision still traces rays in single precision with Embree or OptiX. VERIFIED: https://mitsuba.readthedocs.io/en/latest/src/key_topics/variants.html
- Release: v3.9.0 on 2026-08-07. Windows is in the wheel build matrix. VERIFIED via API.
- Integrator tests use analytic values (50% floor under constant radiance-1 environment is about 0.5) with 1.25% to 5% tolerances, and compare integrators against a reference integrator at 3% relative. VERIFIED (`test_integrators.py`).

### C. Cycles / Blender (the production-renderer pattern)

- Cycles C++ unit tests (`intern/cycles/test/`) cover utilities, SIMD, colour space, adaptive-sampling and scheduler logic and one kernel (`kernel_camera_projection_test.cpp`). No BSDF statistical tests are in that directory. Quality testing is image tests. VERIFIED via API listing.
- Cycles render tests (`tests/python/cycles_render_tests.py`) VERIFIED:
  - Headless: `blender --background --factory-startup --enable-autoexec --debug-exit-on-error`, `-E CYCLES -F PNG`.
  - Per-directory blocklists, including `many_lights.blend` excluded for "High noise variance between platforms".
  - CPU output is compared against stored references. GPU devices are compared against CPU references (tool summary also said EEVEE references for CPU; treat that detail as unconfirmed).
- Comparison is `oiiotool --fail T --failpercent P --diff` (`render_report.py`). Defaults: T=0.016, P=1. Per-directory overrides grow up to T=0.06 and P up to 4 (volume, light, camera, gsplat, OSL principled). There is no retry. `BLENDER_TEST_UPDATE` overwrites references explicitly. A crashing batch is re-run per file. VERIFIED.
- Lesson: pixel-difference tests on noisy renders accumulate hand-tuned tolerances and blocklists rather than a statistical criterion.

### D. Embree (Apache-2.0)

- `tutorials/verify/verify.cpp` is a large randomised API/regression suite with an `intensity` multiplier that scales iteration counts. CMake exposes `EMBREE_TESTING_INTENSITY` with levels 0 (none), 1 (verify and tutorials), 2 (light), 3 (intensive), 4 (very intensive). Each test declares `INTENSITY <i>` and runs only if the level is at least `i`. VERIFIED: https://github.com/RenderKit/embree/blob/master/tests/CMakeLists.txt and `common/cmake/test.cmake`.
- Image tests render primitive XMLs with deterministic debug shaders (`eyelight`, `uv`, `Ng`, `primID`, `occlusion`) and compare with `--compare-threshold`, documented as "threshold in number of wrong pixels". Thresholds are per-test and platform-patched (55 default, 60, 200, 700 for known platform differences). VERIFIED.
- `EMBREE_TESTING_MEMCHECK` is a separate opt-in. VERIFIED.

### E. Independent oracles for a spectral OpenPBR engine

- OpenPBR: v1.1.2 released 2026-11-05 (tags v1.1, v1.1.1, v1.1.2 exist). The repo ships `reference/open_pbr_surface.mtlx` (a MaterialX graph), not a sampler, not reference renders. VERIFIED via API. https://github.com/AcademySoftwareFoundation/OpenPBR/releases/tag/v1.1.2 (resolves the recon's unverified 1.1.1 question: 1.1.1 and 1.1.2 both exist).
- v1.1.2 changelog: thin-walled `subsurface_color` was applied twice (squared albedo) in the MaterialX graph, and the coat transmittance formulas in the spec were corrected. Pin the spec commit and read the changelog before generating oracle scenes. VERIFIED.
- Adobe `openpbr-bsdf` (Apache-2.0): OpenPBR 1.1.1 `prepare/eval/sample/pdf`, one source with macros for C++, GLSL, CUDA, MSL and Slang. The README lists white-furnace unit tests as a wanted contribution, so it ships no energy validation and CUDA "not yet used in production". VERIFIED: https://github.com/adobe/openpbr-bsdf
  - Use as a second implementation for lobe-level comparison and as a precedent for single-source BSDF code. Do not treat it as ground truth.
- NVIDIA Typhoon: CPU-only reference path tracer (Embree intersection) with its own OpenPBR plus Adobe's, a MaterialX implementation independent of the shader generator, a conformance suite of more than 1,200 tests (credits Ben Houston's MaterialX Fidelity tests), about 50k SLOC. The 1,200-test count comes from the OSD 2026 slides only; the branch README states no test count (REPORTED, not re-confirmed). The test harness is a separate repo (`typhoon-test-suite`, pytest, `pixi`). The README says the branch is "on hold while undergoing a large refactor". VERIFIED (slides read via text extraction; README via API): https://hosted-files.sched.co/osd2026/20/A%20Reference%20Renderer%20for%20OpenUSD%20Interoperability%20-%20Anders_Langlands.pdf and https://github.com/NVIDIA-Omniverse/OpenUSD/tree/typhoon/main/pxr/imaging/plugin/hdEmbree
  - Not a stable oracle today, but the strongest precedent: an independent, CPU-only, readable OpenPBR path tracer used as a reference. Revisit when its new branch lands.
- MaterialX: ships 10 OpenPBR example materials (`resources/Materials/Examples/OpenPbr`: aluminum_brushed, carpaint, default, glass, honey, ketchup, lightbulb, pearl, soapbubble, velvet) and OSL/GLSL/MDL/MSL/Slang render tests. v1.39.5 released 2026-05-22. I found no image comparison in `RenderUtil.cpp` (grep), so MaterialX gives inputs, not reference images. VERIFIED listing; no-reference-images claim is UNVERIFIED beyond that grep.
- Mitsuba 3 spectral: independent code for diffuse, rough conductor, rough dielectric, rough plastic, principled (Disney-style, not OpenPBR), and volumetric path tracers. I found no OpenPBR plugin or dispersion model (searched docs and PRs; not exhaustive). `pip install mitsuba` gives a ready oracle with no build. REPORTED/VERIFIED mix as above.
- PBRT-v4 spectral: independent code for diffuse, conductor and dielectric microfacets, dispersion, layered coated diffuse and coated conductor. It needs a source build (CMake, submodules, rgb2spectrum tables; CI pulls the tables from a separate repo). VERIFIED.
- Caveat for all oracles: RGB-to-spectrum upsampling differs across renderers (Mitsuba, PBRT, ours). Oracle scenes must specify spectra explicitly (constants or sampled tables), never RGB. This is my design rule (UNVERIFIED as to each tool's table).

### F. CPU vs GPU numerics

- NVIDIA states CPU and GPU can differ even when both follow IEEE 754 because of fused multiply-add, reordered reductions and non-correctly-rounded libm; the same single IEEE operation gives the same result. FMA merging can be prevented with intrinsics or a compiler flag. VERIFIED: https://docs.nvidia.com/cuda/floating-point/index.html
- CUDA single-precision max ULP errors: `sinf`/`cosf`/`expf` 2, `logf` 1, `powf` 4, `atan2f` 3, `sqrtf` 0, divide 0 by default (2 with `-prec-div=false`). VERIFIED: https://docs.nvidia.com/cuda/cuda-programming-guide/05-appendices/mathematical-functions.html
- Consequence (derived): exact bit equality is achievable only for integer code, RNG, hashing and permutations, and for float code that avoids libm and contracted FMA. Anything with transcendentals needs a ULP or relative tolerance. A path that diverges on one flipped intersection never re-converges bitwise, so transport agreement has to be statistical.
- Precedents for isolating precision from logic: PBRT builds the CPU in double (`PBRT_FLOAT_AS_DOUBLE`, in the CI matrix) and Mitsuba has `_double` variants. VERIFIED.
- Cross-backend precedent: Mitsuba renders each test scene on every variant against one reference (Z-test), and Cycles checks GPU devices against CPU references. VERIFIED.
- OptiX and a software BVH break ties differently on coincident faces (v1 lesson, recon). Compare intersection results with a tie exclusion. REPORTED (v1 memory digest).

### G. Image-difference metrics

- NVIDIA FLIP: BSD-3-Clause, header-only C++/CUDA `FLIP.h`, pip package `flip-evaluator` 1.7 (2025-11-07, Python 3.10+), LDR and HDR variants, PyTorch modules. VERIFIED: https://github.com/NVlabs/flip and https://pypi.org/project/flip-evaluator/
- I could not confirm default pixels-per-degree, HDR exposure range, or any guidance on Monte Carlo noise (README does not state them). UNVERIFIED.
- FLIP, SSIM and `oiiotool --fail` are perceptual or pixel metrics and do not distinguish noise from bias. The statistically principled comparison is the Mitsuba Z-test (needs a variance image) or a two-sample test with both variances. v1 reached the same conclusion (a Welch tile test replaced SSIM; SSIM gates mislead). REPORTED (v1 digest).

### H. Blender headless

- Blender 5.3.0 is on PyPI as `bpy` (2026-11-10), CPython 3.13 only, Windows x86-64 wheel 356 MB, GPL-3.0. VERIFIED: https://pypi.org/project/bpy/
- Cycles tests drive the real binary headless with `--background --factory-startup --python...`, batching files per process and re-running after a crash. VERIFIED (section C).
- Whether `bpy` as a module can host a third-party render engine's full session (registering an engine, rendering, `view_draw`) was not verified. The viewport (`view_draw`) needs a GL/Vulkan context and cannot run in `--background`. UNVERIFIED.
- Blender 5.4 is in alpha until 2027-02-03 (recon, REPORTED).

### I. Performance regression

- Google Benchmark `compare.py` uses a Mann-Whitney U test and states it needs "no less than 9" repetitions to be meaningful. VERIFIED: https://github.com/google/benchmark/blob/main/docs/tools.md
- v1 measured about 5% GPU boost-clock drift in A/B timing, and used burn-in and min-of-N (memory `gpu-perf-ab-clock-drift`, `gpu-alloc-layout-luck-in-perf-ab`: allocation layout alone shifted results). REPORTED (index descriptions only; I did not open the files).
- v1 gated kernel changes on `cuobjdump -res-usage` REG/STACK/SHARED/LOCAL/CONSTANT per kernel being identical (358/358 functions) and on measured build times (fresh 8.0 min, touch-one-part 3.5 min). VERIFIED: v1 `.astroray_plan/docs/build-speed-directive.md` section 3. These are deterministic proxies that need no noise handling.

### J. v1 testing practices (read-only: `tests/conftest.py`, `build-speed-directive.md`, recon digests)

Keep (the idea, not the code):
- Gating curated evidence: the session fails if tests modify tracked `test_results/`; per-run output goes to an untracked `_runs/`. VERIFIED (conftest).
- A `slow` marker rather than deleting tests (Mitsuba does the same). VERIFIED.
- A latent-CUDA-error check after each GPU test, failing loudly at the boundary. VERIFIED (conftest `cuda_cleanup_and_error_check`).
- A render guard that forces furnace and energy tests to render linear (`apply_gamma=False`), enforced at the render call. VERIFIED (conftest, `_linear_render_guard`). In v2 make this structural: furnace tests read radiance, not a display transform.
- Differential gates that fail on stale artifacts: stale `.pyd`, CMake cache arch mismatch. REPORTED.
- Kernel-identity gate with `cuobjdump -res-usage`. VERIFIED.
- GPU tests strictly serial; CPU tests parallel. VERIFIED. The reason (concurrent CUDA gave false `illegal memory access`) stands.
- Look at the render as well as the numbers. REPORTED.

Do not keep:
- Classification of tests by regex on source text (`_gpu_classification.py`) and filename stems. It is conservative-by-design and was already misfiring (CPU-only carve-out missed CUDA-reachable headers). VERIFIED. In v2, class follows the executable: a CPU-target test binary links no CUDA and cannot touch the GPU.
- Auto-skip when the module is missing (`pytest.skip` in `astroray_module`). Rule 7: a missing build must fail the tier, not skip it.
- A 565-file, 4,587-case suite with a 45-minute full run (recon). Cost grew without a budget.
- `-x` fail-fast in sweeps (hid GPU results; recon).

Reference assets that are not trustworthy (all REPORTED from the recon digests; I did not open the assets):
- Two of three gate scenes were byte-identical placeholders (same SHA-256). Gate scenes (b) and (g) are RED or churned.
- The reference-bank smoke fails 2 of 3 gates; the 2026 gate baselines were re-pinned many times.
- Tests "calibrated on a broken engine" (memory: fix them in the same change), and baselines that "can encode the bug" (memory `ssim-gate-traps`).
- The "clean-room oracle" proves only self-consistency (memory).
- The glass chi-squared gate: about 90% red at `ires=4` was a quadrature artifact (memory). Mitsuba's own tests set `ires` per case for the same reason (VERIFIED, section B).
- Anything compared against Cycles output: the owner decision removes Cycles pixel parity as a target, and Cycles is RGB, not spectral.
- Gamma comparison traps (`applyGamma` can fake a 2x, clamping hides energy gain; memory).
- Recommendation: treat all v1 references as untrusted inputs until re-derived from an analytic or independent oracle. Physics lessons in the recon (refraction front-face, dead-sample mass, occlusion sentinel) are worth turning into tests.

## Options and tradeoffs

Real forks only.

1. Where kernel tests live.
   - C++ in-process (pbrt style, gtest/Catch2/doctest): no binding layer, fastest edit-link-run, kernels called directly and parallel with `ctest -j`. Statistics (chi2 CDF, p-values) must be written in C++ (pbrt has its own `Chi2CDF`, small). Weak for plotting, EXR, oracles.
   - Python via bindings (Mitsuba style): scipy for statistics, trivial parametrisation, easy oracle and image work. Needs a vectorised binding of every kernel function, and Python start-up inside the 10 s pre-commit budget.
   - Hybrid: C++ drivers for T0/T1 kernel checks that print JSON; Python only for image, oracle, Blender and reporting. Blender tests need Python anyway. This costs two languages. The deciding factor is the binding design in the architecture workstream; I recommend C++ for T0 and a thin Python layer above it, pending the spike.
2. Reference class for image tests.
   - Analytic or deterministic quadrature: no storage, no rot, limited to simple scenes.
   - Independent renderer (Mitsuba, PBRT): covers realistic scenes, but must avoid RGB upsampling mismatch and each tool covers a subset of features.
   - Self-golden: cheap, catches regressions, can bake in a bug.
   These are complementary, not alternatives. The real choice is how to label them (section Recommendation 7).
3. CPU/GPU agreement bar.
   - Demand bitwise everywhere: forces `--fmad=false` and custom math functions on both targets, which costs performance and contradicts the speed goal.
   - Tolerance ladder: bitwise for integer, RNG and index code; ULP or relative for pure float functions; statistical for transport. Cheaper, with a small risk that a few-ULP drift hides a real bug at the function level. I recommend this.
4. Significance policy for statistical tests.
   - Fixed seeds, no correction: deterministic and fast, but each test is a single draw at its own alpha, so any change in RNG consumption order re-rolls every test.
   - Fixed seed plus Sidak per family (PBRT and Mitsuba both do this): gating stays deterministic and failures are rare at alpha_family.
   - Rotating nightly seed on top: finds bugs a lucky fixed seed hides, at the price of nightly failures that must be diagnosed. I recommend fixed seeds in the gate and a rotating seed nightly, both with Sidak.
5. GPU in CI.
   - PBRT's GPU CI only builds (VERIFIED). v1's CI had no GPU, which let GPU regressions merge (recon).
   - A self-hosted runner on the owner's RTX 5070 Ti closes the gap but ties CI to one machine. Hosted GPU runners were not researched (UNVERIFIED).

## Recommendation for Astroray 2.0

### 1. Design requirements the test strategy imposes on the architecture

Each one is something the cited suites needed (evidence in brackets).

1. Every lobe, light and sampler exposes pure functions that take explicit uniform random numbers (`sample(u)`, `eval`, `pdf`) and return the pdf and flags. [pbrt `Sample_f(wo,u,u2)`; Mitsuba adapters take a `samples[dim,N]` array.] No hidden sampler state in kernels.
2. One scalar typedef (`Real`) so the CPU target builds in double. [pbrt `PBRT_FLOAT_AS_DOUBLE`, Mitsuba `_double`.] This must be decided in the kernel-language workstream; it is cheap now and expensive later.
3. Counter-based RNG keyed on (pixel, sample index, dimension, seed), value independent of call order. [pbrt `Sampler.ConsistentValues`; Mitsuba uses PCG32 plus TEA scrambling.] Required for CPU-vs-GPU and thread-count independence.
4. The film accumulates per-pixel first and second moments in double or compensated float. [pbrt per-pixel `VarianceEstimator`; Mitsuba moment integrator and `_var.exr`.] Without it the Z-test needs a fixed tolerance. A 1x1-pixel analytic test with 1e7 samples also breaks plain float accumulation (derived: a float sum stops growing near 2^24 equal addends).
5. Deterministic AOV "shaders" are first-class: primary hit position, normal, material id, UV, albedo, depth. [Embree `eyelight/uv/Ng/primID` tests.] They make CPU-vs-GPU, texture orientation and exporter tests noise-free.
6. Test scenes are built by a small C++ or Python scene builder, not by Blender files. Blender enters only in the end-to-end tier.
7. Test binaries are per target: `test_cpu` links no CUDA, `test_gpu` runs in one process and one context, serially.

### 2. Test families

| Family | Where | What it proves | Method and non-flakiness |
|---|---|---|---|
| Pure functions | CPU, T0 | math, spectrum, sampling transforms, RNG, index and layout `static_assert` | Deterministic. Sample/invert round trips (pbrt style); CMF integrates to Y (pbrt); blackbody against tabulated values; counter-RNG order independence. |
| Lobe consistency | CPU, T0 deterministic | pdf integrates to at most 1; `eval` equals the `sample` weight times pdf; `pdf()` equals the pdf `sample` reports; reciprocity where declared | Fixed quadrature grid (Gauss or dense uniform), no random numbers. Never flaky. Replaces most chi2 at T0. |
| Lobe statistics | CPU, T1 and T2 | sampler matches its pdf; weights match `eval cos / pdf`; energy | See 3 below. |
| Light and light-sampler | CPU, T1 | power, sampling pdf against closed form; BVH or tree sampler unbiased against exhaustive sampling | pbrt `lights_test` and `lightsamplers_test` pattern. |
| Analytic scenes | CPU and GPU, T1 and T2 | whole-pipeline radiometry (scene list below) | Compare mean to closed form with `|mean-truth| < z*SE + eps_sys`. No stored reference. |
| CPU-vs-GPU | both, T1 and T2 | the single source agrees across targets | 4 below. |
| Oracle scenes | T2 | realistic multi-feature agreement | Mitsuba/PBRT references with variance, Z-test. |
| Golden regression | T1 smoke, T2 full | no unintended change | Z-test against self-golden; separate directory. |
| Blender end-to-end | T2 smoke, T3 full | exporter and addon fidelity | 5 below. |
| Performance | T2, T3 | no speed or compile regression | 6 below. |

### 3. Statistical tests: how to make them fast and non-flaky

- Three checks per lobe, in order of sensitivity per sample:
  1. Weighted-mean energy test: mean of `f cos / pdf` over the sampled hemisphere, with its standard error from the sample variance. For well-importance-sampled lobes the weight is nearly constant, so SE is tiny: for weights of std 0.1 to 0.5 and N=1e6, SE is 1e-4 to 5e-4 (derived). It detects gain and loss at the 0.1% level, which chi2 cannot. Assertions: for lossless lobes, mean within 3 SE of the expected albedo; for lossy lobes, mean at most 1 + 3 SE (a two-sided version of pbrt's one-sided 1.01 bound, which is active only for Lambertian).
  2. Chi-squared of sample histogram against the pdf integrated per cell (pbrt/Mitsuba pattern: pooling at expected count 5, Sidak, explicit `ires`).
  3. Weighted chi-squared of weight-histogram against `integral of f cos` per cell (Mitsuba supports weighted samples). This is what catches a sampler whose pdf is right but whose reported weight is wrong.
- Chi-squared has limited power at fine grids. Detectable uniform relative shape error is about `delta ~ sqrt(3*sqrt(2k)/N)` for k cells and N samples (derived from a noncentrality of N*delta^2 against a null standard deviation of sqrt(2k)). For pbrt's k=12,800 and N=1e6: about 2.2%. For a coarse k=800 grid at N=1e6: about 1.1%; at N=1e5: about 3.5%. So use a coarse grid for global errors and a fine grid for localised features such as lobe edges. Chi-squared finds gross errors (missing Jacobian, wrong lobe mix, wrong domain); the energy test finds subtle bias.
- Fixed seeds in the gate: seed = hash(test name, repo constant). Print the seed and a one-line reproduce command on failure. Dump the pdf and histogram tables as Mitsuba (`chi2_data.py`) and pbrt (`.m`) do.
- Sidak across the whole family, not per test. Per-test alpha for family alpha 0.01 is 5.0e-5 for 200 tests and 1.0e-5 for 1,000 (derived). Using Mitsuba's default 0.01 per test would give about 2 expected false failures per 200 tests whenever RNG consumption changes.
- Rotating seed nightly (T2), same Sidak. A weekly seed sweep (T3, 8 seeds) measures the real flake rate; any test failing on more than 1 in 20 seeds is fixed or its power reduced.
- Quarantine policy with expiry date, never indefinite xfail (v1 memory: xfail markers must be removed by the implementing PR).
- Quadrature artifacts: set `ires` per lobe from the lobe's sharpest feature; fail the suite if a cell has zero expected mass but observed samples (Mitsuba check). A chi2 failure triggers a "same test at `ires` x2" self-check before it is reported as a lobe bug. This addresses v1's glass-gate false reds.
- Reciprocity: assert only for lobes declared reciprocal. OpenPBR's layering is documented as dropping reciprocity in places (recon; the paper was only partly read, REPORTED), so declare each lobe's status and bound the violation for the rest. For refraction, use Veach's generalised reciprocity with the eta^2 factor (UNVERIFIED, background knowledge).
- Furnace: render linear radiance only. Pass if mean within 3 SE of the expected value plus a documented `eps_sys` (about 1e-3 for lobes with approximations such as multiple-scatter compensation).
- Test film uses double accumulation. 1x1 film tests are the cheapest analytic oracle, but only if accumulation is exact.

### 4. CPU-vs-GPU agreement ladder

1. Bitwise: integer ops, counter RNG, sampler indices and scrambles, hashing, permutations, sort keys. One kernel launch over 1e6 inputs, compare as bytes.
2. ULP or relative tolerance for pure float kernels over a seeded input array (BSDF eval/pdf/sample, spectral upsampling, sampling transforms). Start at 4 ULP for code without transcendentals, 1e-5 relative with them, derived from CUDA's 2 to 4 ULP `sinf/expf/powf` bounds, and tighten after measuring. Mitsuba's `check_vectorization` uses 1e-6 absolute (VERIFIED), a reasonable starting point for values near 1.
3. Deterministic AOVs from a small scene: positions, normals, material ids compared with tolerance, tie cases excluded. Catches BVH, transform and geometry-upload bugs without noise. OptiX and software traversal differ on coincident faces, so keep test meshes clean.
4. Beauty image: two-sample statistical test between CPU and GPU renders using film moments (Welch per tile or per pixel with Sidak, pass fraction as in Mitsuba's 0.9975). Never bitwise. Spectral hero-wavelength sampling is identical on both because the RNG stream is (pixel, index, dim) keyed.
5. Precision oracle on the CPU target: float vs double CPU render of the same scene must agree within noise. A failure means a numerical problem (self-intersection epsilon, cancellation), not an algorithmic one. Run at T2.
- The same mechanism checks Slang/CUDA/other backends later.

### 5. Blender end-to-end

- Headless `blender --background --factory-startup --python-exit-code 1 --python <driver>` per Cycles' pattern. Batch scenes per process, re-run a crashed batch per file, parse a JSON result. (Cycles pattern VERIFIED.)
- Three test types:
  1. Exporter equivalence: a scene exported from Blender must equal the scene built with the test scene builder, by comparing the deterministic AOVs bitwise or at tolerance, then the beauty statistically. This tests data loss without trusting a reference image.
  2. No silent drops (rule 7): enumerate node types and sockets via `bpy` and require each to be implemented or to produce a visible warning. v1's 340 "DROPPED-SILENT" sockets of 527 (recon) are the failure this prevents.
  3. Round-trip of Blender edits through the incremental update path: apply an edit sequence, then compare the result to a full re-export of the final state (viewport correctness without a GUI). Include "edit an image after full sync" (recon open question on stale texture cache).
- Real viewport tests (GL/Vulkan context, `view_draw`, frame pacing, copy cost) need a windowed Blender. Run them as a T3 script on the owner machine using `bpy.app.timers` and area screenshots. Feasibility on a Windows runner is UNVERIFIED; do not block merges on it.
- The `bpy` PyPI wheel (3.13, 356 MB) allows engine-free tests of conversion code without launching the binary. Whether a render engine can be registered and run from it is UNVERIFIED (spike).
- Matrix: Blender 5.3 on every T2; 5.3 plus 5.4 alpha weekly.

### 6. Performance regression robust to noise

- Prefer deterministic proxies as hard gates: `cuobjdump -res-usage` per kernel (REG, STACK, SHARED, LOCAL) against a committed baseline file; rays and BVH nodes visited per fixed scene; bytes moved; compile time per TU and the incremental rebuild time budget (rule 9). v1 used the first and it was reliable (358/358 identical).
- Wall-time gates are soft: fixed workload (fixed scene, spp, seed, so work is identical), CUDA events for GPU time, burn-in until clocks settle, then at least 15 interleaved A/B pairs (baseline binary and candidate alternating, because v1 saw about 5% drift), compare with a Mann-Whitney U test (Google Benchmark recommends at least 9 repetitions). Fail only if the median ratio is above 1.05 and p is below 0.01; report the minimum as well. Do not wrap in the GPU lock twice (v1 weekly-bench self-lock).
- Never run perf beside builds or tests (serialise GPU use; v1 hard resets under combined load).
- Track the test suite's own duration per tier and fail the tier when it exceeds its budget (a measured budget is a requirement, rule 9).

### 7. Reference policy

- Three classes under `tests/data/`: `analytic/` (no stored images), `oracle/` (Mitsuba or PBRT mean and variance EXR, with the tool version, command, seed and spectra files recorded alongside), `golden/` (self-generated regression only). A scene may have a golden only after it passes an analytic or oracle check, or after a recorded visual inspection of the image.
- A missing reference fails the test (Mitsuba skips, which would let a rename silently disable a test). References change only through an explicit `--bless` command that prints the diff statistics and writes to `golden/` (Cycles `BLENDER_TEST_UPDATE` precedent).
- Oracle and golden scenes specify spectra explicitly, never RGB.
- Weekly (T3) re-render of the oracle references with the pinned oracle and confirm the stored files still match, to catch reference rot.
- Size: 256x256, float32 mean plus variance is about 1.5 MB per scene (derived). Six oracle scenes fit in the repo without LFS; decide with the repo owner.
- Look at the image: every golden update and every failing image test writes `_img/_ref/_error/_pvalue` EXRs and a PNG contact sheet (Mitsuba failure outputs). A person or vision check complements the number, as v1 learned. A vision model must not decide pixel positions (v1 memory).
- Use FLIP only on converged, noise-free images (AOV or high-spp) for human-facing diff maps. It is not a gate for noisy output.

### 8. Tier definitions

| Tier | Trigger | Budget | Contents |
|---|---|---|---|
| T0 pre-commit | local hook, on staged files, affected targets only | under 10 s | CPU target build of changed tests plus pure-function and deterministic lobe-consistency tests; static_asserts; formatter and a unit-test-per-kernel presence check. No GPU, no random-number p-values. |
| T1 merge gate | every PR and push to main | under 3 min wall on the 5070 Ti machine, including a test-only incremental build | all T0; CPU chi2/energy suite at gate sample counts, run in parallel; GPU smoke in one process: bitwise and ULP kernel agreement, deterministic-AOV CPU/GPU compare, analytic scenes S1 to S6 at low resolution, 3 golden images. GPU part runs on the self-hosted machine. |
| T2 nightly | schedule and on-demand label | under 20 min (target; unmeasured) | full chi2 with rotating seed and Sidak; OpenPBR parameter-lattice lobe audit (about 200 materials from a space-filling design); all analytic scenes at higher spp; oracle scenes; CPU-vs-GPU beauty test; float-vs-double CPU; Blender headless exporter-equivalence and no-silent-drops; perf proxies and interleaved A/B. |
| T3 weekly or release | schedule, release branch | under 2 h | 8-seed sweep of the statistical suite (flake-rate estimate); convergence slope (error vs 1/sqrt(N)) on analytic scenes; oracle-reference regeneration check; Blender 5.3 and 5.4 matrix; windowed viewport session; leak and soak run; build-time audit (cold, warm, incremental). |

Selection rule: tests declare the kernel directories they depend on (from the build graph, not a regex over source), and T1 runs the affected subset first and the full tier before merge. Because kernels are single-source, a kernel change always triggers both CPU and GPU smoke. Budgets are provisional until measured; a tier that exceeds its budget by 20% fails (recon proposal, kept).

### 9. Initial canonical scenes (17)

All specify spectra explicitly. "A" = analytic closed form, "O" = independent oracle, "D" = deterministic (no stored image).

| # | Scene | Truth | Proves |
|---|---|---|---|
| S1 | Lambertian sphere (rho 0.5 and 1) in a constant environment of radiance 1, unbounded depth | A: pixel equals rho for a convex object | env sampling and MIS, Russian roulette unbiased, film normalisation, spectral to display white. |
| S2 | Closed Lambertian sphere interior with emission Le, camera inside | A: L = Le/(1-rho) | multi-bounce, path termination, no energy loss over long paths. |
| S3 | Lambertian plane under an on-axis disk emitter | A: L = rho*L_e*R^2/(h^2+R^2) (from E = pi L R^2/(h^2+R^2)) | area-light sampling pdf, geometry term, NEE plus MIS. |
| S4 | Sphere emitter, point and spot lights over a plane | A: E = pi L (R/d)^2; E = I cos/d^2 | solid-angle sphere sampling, delta lights, spot falloff. |
| S5 | Uniform emitter filling the view, pinhole and thin lens at several apertures and fields of view | A: pixel equals L | sensor, lens pdf and cos^4 handling, radiance invariance, exposure units. |
| S6 | Smooth dielectric slab and sphere in a furnace; slab with Beer-Lambert absorption | A: lossless R+T=1, slab total T = (1-R)^2 t/(1-R^2 t^2), t = exp(-sigma d); sphere equals 1 | Fresnel, TIR, eta^2 scaling, dead-sample mass (v1 lesson), absorption. |
| S7 | Rough conductor and rough dielectric sphere rows (roughness 0.05 to 1) in a furnace | A for F0=1 conductor with multiple-scatter compensation expected near 1 (OpenPBR requirement, REPORTED); O/quadrature for others | lobe energy over roughness, multiple-scatter compensation, layered albedo scaling. |
| S8 | Prism with line emitters at 450, 550 and 650 nm | A: n(lambda) from Sellmeier or Cauchy via Snell and minimum deviation; O: PBRT-v4 for beauty | dispersion, wavelength-dependent paths, hero-wavelength handling (secondary termination). |
| S9 | Thin-film coating, narrow-band lines at normal incidence | A: Airy reflectance (OpenPBR appendix) | thin-film term, per-wavelength evaluation. Phase 2 feature. |
| S10 | Blackbody diffuse emitter (T=5778 K and 3000 K) to a spectral film with bands | A: Planck B_lambda per band; total radiance sigma T^4/pi; photon count from lambda/(hc) weighting; XYZ from CIE tables | the astro-critical chain: SI units, spectral film, band-integrated radiance and photon counts, CMFs, display transform. |
| S11 | Homogeneous absorbing box in front of an emitter; non-absorbing scatterer sphere in a furnace | A: transmittance exp(-sigma_t d); furnace equals 1 | volumes: Beer-Lambert, spectral sigma, scattering energy conservation. |
| S12 | Diffuse sphere under an HDRI containing one bright texel | D quadrature: radiance = rho/pi * integral of L cos over texels | environment importance sampling, texture orientation and colour space, MIS (v1 had HDRI failures). |
| S13 | Cornell-style box, diffuse plus rough conductor plus rough dielectric balls, area light | O: Mitsuba `scalar_spectral` and PBRT-v4 at high spp, Z-test | integrator, multi-bounce, area-light MIS against two independent codes. |
| S14 | 1,000 emitters (points and triangles) over a plane | O: exhaustive-NEE render of our own build; A at selected pixels | many-light and light-tree unbiasedness (pbrt light-sampler pattern). |
| S15 | UV chart on a plane plus mapped normal and roughness | D: AOV bitwise, no transport | UV orientation, tiling, sRGB decode, filtering, Blender bottom-up pixel flip (v1 lesson). |
| S16 | `.blend` file with Principled/OpenPBR node setups and lights | D: exporter-equivalence to builder scene | exporter fidelity, no silent node drops, edit round trip. |
| S17 | Schwarzschild camera at r=20 M, thin emitting ring and flat background | A: critical impact parameter b_c = 3*sqrt(3) M (about 5.196 M), weak deflection 4M/b, redshift (1-2M/r)^(-1/2) | later astro tier (Pillar 4): a kernel-level geodesic test on the CPU target plus one image. Reserve now, implement with the GR work. |

The OpenPBR parameter-lattice lobe audit is a kernel suite, not a scene.

## What a measurement spike must settle

1. Test build and link time: one CPU-target test executable with C++ kernels compiled for host only, incremental link after editing one kernel header, on MSVC. The T0 budget (10 s) is only feasible if this is a few seconds. Compare doctest, Catch2 and gtest compile cost.
2. Throughput of the chi2 and energy tests on the CPU target at 1e5, 1e6 and 1e7 samples per lobe, one thread and 16 threads, to set gate sample counts that fit T1. Measure how many lobe configurations fit 3 minutes.
3. Detection power: inject known bugs (a missing cos, a wrong Jacobian, a 1% albedo error, a dropped transmission term, a swapped front-face) into a lobe and record which of energy test, chi2 coarse, chi2 fine and weighted chi2 catch each at what N. This validates the derived power formula.
4. CPU-vs-GPU error distribution: run the same pure kernels on 1e7 seeded inputs, histogram the ULP difference per function, then set the tolerance ladder from data. Include a build with `--fmad=false` to quantify how much of the difference is FMA.
5. GPU context start-up and teardown cost per test process, to decide how many GPU tests share a process, and whether the latent-error check must run per test.
6. Film moment accumulation cost and accuracy (float vs double vs Kahan), and whether a 1x1 film test with 1e7 samples is exact.
7. Cross-oracle agreement: render S13 in Mitsuba `scalar_spectral` and PBRT-v4 with identical explicit spectra and report their mutual Z-test. If the oracles disagree with each other beyond noise, scope the oracle claims down to the features where they agree.
8. Tie-break and BVH differences: random-ray intersection agreement between OptiX and the CPU BVH on the test meshes, with a count of tie cases.
9. Whether a Blender render engine can be registered and run through the PyPI `bpy` 5.3 wheel on this machine, and the cost of a per-process Blender launch.
10. Perf noise floor: repeated identical runs of one wavefront stage to measure run-to-run and burn-in variation on the 5070 Ti, and to calibrate the 1.05 threshold.
11. Mitsuba `pip` wheel on this machine: Python-version support for the Windows wheel and time to a first spectral render (not checked here).

## Unverified or conflicting

- FLIP default pixels-per-degree, HDR exposure range, and any advice for Monte Carlo noise (README does not say).
- Whether Cycles CPU references come from EEVEE (the tool summary of `cycles_render_tests.py` said so; I did not re-read the raw line).
- Mitsuba license text (GitHub API says NOASSERTION), and whether Mitsuba has any dispersion or OpenPBR support beyond what I found in plugin docs and release notes.
- Whether Typhoon will publish a stable successor branch (README says the current branch is on hold), and whether its conformance suite can be reused without the whole stack.
- MaterialX: I found no reference-image comparison in one file (`RenderUtil.cpp`); other CI jobs may compare images.
- OpenPBR energy claims (albedo scaling, white-furnace behaviour, dropped reciprocity) come from recon's partial read of the companion paper (REPORTED), not my own read.
- v1 memory items I cite (`gpu-perf-ab-clock-drift`, `ssim-gate-traps`, `cuda-concurrency-kills`, `fix-tests-calibrated-on-broken-engine`) come from the memory index descriptions and the recon digests; I did not open the memory files.
- Statistical power numbers and tolerances in section 3 and 4 are derived or proposed, not measured. All tier budgets are provisional until the spike.
- Whether a hosted GPU CI runner is available and affordable for Windows RTX-class hardware (not researched). Assumed: the owner's machine runs the GPU gate.
- Whether `bpy` as a module supports the full render-engine and viewport paths (not verified).
- Recon conflict: its T1 proposed one chi2 per BSDF at 1e5 samples, but section 3 shows 1e5 samples on a fine grid detects only large errors (about 7% uniform shape error at k=12,800; derived). Use the energy test as the primary gate and coarse-grid chi2 for shape.

## Verification pass

Date: 2026-11-14. Independent re-check against primary sources. Recommendation unchanged.

- Mitsuba render suite: 2e6 budget, `spp = budget // pixels`, Sidak 0.01 across pixels, pass at 0.9975 of pixels, z uses `max(var, 1e-4)`, missing reference is a skip, all `mi.variants()` run against one reference. CONFIRMED. https://raw.githubusercontent.com/mitsuba-renderer/mitsuba3/master/src/render/tests/test_renders.py
- Mitsuba references are self-generated at 32,000 spp on scalar variants. CORRECTED in place: variants are `scalar_*` not ending in `double`; "moment integrator" COULD NOT VERIFY (same URL).
- PBRT chi2 constants (1e6 samples, 80x160, slevel 0.01, minfreq 5, 5 runs, Sidak over runs). CONFIRMED. https://raw.githubusercontent.com/mmp/pbrt-v4/master/src/pbrt/bsdfs_test.cpp
- PBRT energy bound 1.01. CORRECTED: only the Lambertian test is enabled; microfacet energy tests are `#if 0`. Same URL.
- "Neither project has a white-furnace test for rough dielectric or layered lobes". CONFIRMED for pbrt (only hair) and for the Mitsuba non-hair BSDF files. CORRECTED: Mitsuba hair has two furnace tests. https://github.com/mitsuba-renderer/mitsuba3/blob/master/src/bsdfs/tests/test_hair.py
- PBRT single source (`PBRT_CPU_GPU`), CPU CI on Linux and Windows, GPU CI build-only (CUDA 12.9.1 to 13.2.0). CONFIRMED. https://github.com/mmp/pbrt-v4/tree/master/.github/workflows
- "CI runs the CPU suite with a float-as-double build". CORRECTED: the matrix builds `-DPBRT_FLOAT_AS_DOUBLE=True` but `pbrt_test` runs only when `matrix.config == ''`. (Body text was already right; the task summary overstated it.)
- Secondary-wavelength termination at dispersive events. COULD NOT RE-VERIFY this pass; stays as researcher-VERIFIED.
- CUDA ULP table (sinf/cosf/expf 2, powf 4, divide 0 default, 2 with `-prec-div=false`). CONFIRMED. https://docs.nvidia.com/cuda/cuda-programming-guide/05-appendices/mathematical-functions.html
- OpenPBR v1.1.2 released 5 Nov (2026 inferred from page ordering); changelog (thin-walled subsurface color applied twice, coat transmittance docs). CONFIRMED. https://github.com/AcademySoftwareFoundation/OpenPBR/releases
- Adobe openpbr-bsdf: Apache-2.0, 1.1.1, C++/GLSL/CUDA/MSL/Slang, no furnace tests (listed as wanted contribution). CONFIRMED. https://github.com/adobe/openpbr-bsdf
- Typhoon branch on hold for refactor. CONFIRMED (README quote). 1,200+ tests: README gives no count; slides-only, now REPORTED. https://github.com/NVIDIA-Omniverse/OpenUSD/tree/typhoon/main/pxr/imaging/plugin/hdEmbree
- Remaining claims (Cycles, Embree, FLIP, bpy, Google Benchmark, v1 digests): not re-checked this pass; tags unchanged.
