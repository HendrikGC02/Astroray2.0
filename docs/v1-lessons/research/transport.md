# Light transport: caustics, photon methods, ReSTIR, path guiding, adaptive sampling, fireflies, MIS, RNG and samplers, noise, motion blur

Carried over from the v1 renderer's research. v1 was a spectral path tracer (hero wavelength, 4 lanes) with CPU and
GPU-wavefront paths. Findings are stated as mechanisms. "Cycles" notes record Blender behaviour (semantics and
reference algorithms), not a pixel-parity target. Numbers are collected under "Measured".

## Established findings

### Caustics: specular manifold methods (MNEE, SMS)
- Candidates for a clean prism rainbow: MNEE (Hanika 2015), SMS (Zeltner 2020), photon mapping (Jensen 1996), MMLT
  (Jakob & Marschner 2012; rejected, unstable). Licence drove the choice: SMS reference code is BSD-3 (used as a
  skeleton); per-wavelength Newton residuals were re-derived from the MNEE paper. Cycles' MNEE header was recorded as
  GPL-2.0+ in one note and Apache-2.0 in others; verify before porting any line.
- Cycles' MNEE uses a scalar eta, so its caustic chroma comes only from the per-ray hero-wavelength IOR. A
  per-wavelength SMS extension is unpublished.
- Camera-side SMS/MNEE gives no clean band on a flat (two-planar-face) prism: no focusing; seed vs deviation mismatch
  (38-52 degree bend); spatially chaotic Newton basins (salt-and-pepper that did not shrink from 64 to 16384 spp); weak
  dispersion vs beam width washes band centres to white. A sphere works because it focuses. A prism rainbow is a
  forward-transport effect.
- SMS on triangles fails with a central-difference constraint Jacobian: a +-h tangent step crosses an edge into a
  neighbour with a different normal. Use the analytic 2x2 constraint Jacobian. Seed-vertex tangent frames must be
  orthonormal (raw triangle edges did not converge).
- Single-seed Newton finds only 1 of 2 roots of the single-vertex sphere problem. Specular polynomials (Fan 2024,
  square-form angularity) give a degree-6 polynomial in t = tan(theta/2) and all roots; for a sphere the paper's
  rational sqrt fit is unnecessary (exact); triangle refraction still needs it.
- The seed-area pdf weight in default Newton SMS was biased: use the MNEE geometry (transfer-matrix) weight. A separate
  receiver-cosine double count existed.
- Cycles' MNEE limitations to avoid: caustics classed as direct light; no fallback when the walk fails. Classify
  caustics as indirect and keep brute-force path tracing as the fallback.
- An SMS plus light-sample-cull design for a GPU wavefront (separate connection stage, cull predicate as in Cycles'
  MNEE-success flag) was planned and never built. Its risk is cull correctness (keep or double-count).

### Caustics: forward photon methods
- Forward tracing is the shipped answer for prism rainbows: photons from the collimated sun, refracted with Sellmeier
  IOR (Sellmeier 1871) and Fresnel, deposited with CIE 1964 10-degree CMFs. Deterministic, so few camera samples suffice.
- Evolution: prism-specific 2D floor grid, then a world-space photon map (balanced kd-tree per Jensen's pseudocode,
  k-NN density, receiver BSDF applied per photon). Density L = tau/(np * pi * r^2). pbrt has no kd-tree photon map; its
  SPPM was used only for the disk-area factor. Delta-only receivers (mirror, smooth conductor) are not gathered;
  camera-mirror-glass-lamp paths stay path traced.
- GPU variant: uniform spatial hash grid (SPPM style), not a kd-tree; it matches the CPU density estimate, not the
  neighbour set. Hash primes 73856093, 19349663, 83492791 (Teschner 2003); cell size from the scene AABB diagonal. An
  adaptive k-NN cone gather needs a two-pass build (provisional grid, then kth-nearest radius). Progressive radius
  reduction and visible points were dropped.
- Photon power: Phi_p = S(lambda) W / N_i, W carrying the cone solid angle; directions uniform in one cone toward the
  casters' bounding sphere; per-light count proportional to flux toward casters; wavelength sampled proportional to S
  over 380-720 nm. Use exact Fresnel (Schlick ignores TIR onset, R ~ 0.04 at the critical angle) and strip the eta^2
  radiance factor when transporting power (pbrt TransportMode).
- Fresnel roulette (reflect with probability F, weight 1, no eta^2; pbrt FresnelSpecular) is required: deterministic
  refraction that reflects only on TIR put T-r-T chains in the map, and they also survived in the path-traced estimate
  (double counting). Fix: a split cull that drops a path-traced lamp hit when every hit after the first receiver was a
  caster. Also cull path-traced refractive caustics whenever the gather is on.
- Rebuild the map in rounds, not once per render call: one map per render left ROI std nearly flat in spp and froze
  speckle. Use rounds of max(16, spp/256) with fresh maps (SPPM-style). k-NN radius singularity gives isolated spikes;
  fix per Jensen 2001 App. B (keep dist^2 = max_dist^2 until K photons are found).
- Use the geometric outward normal and derive entering/exiting from it. A loop using the ray-oriented normal made every
  exit take eta = 1/ior and the GPU glass-sphere ROI ~433x the CPU's; the first diagnosis (GPU intersection numerics)
  was wrong. Coplanar faces create false caustics: 97% of one floor ROI was a TIR off a bottom face coplanar with the
  floor (z-fight); lifting the prism 5 mm fixed it.
- Known limits: gather at the primary hit only; reflective caustics path traced; glossy primary receivers get a diffuse
  photon estimate (untested); one cone per light toward the union of casters; lights with radius emit from the centre;
  mesh emitters emit no photons; aimed lamps need |cos| > 0.9995.

### Blender (Cycles) bounce, transparency and path-flag semantics
- Transparent passes do not advance the bounce or per-type counters; only the transparent-bounce counter advances. The
  next surface after the limit emits and terminates. Shadow-ray transparency budget = max(transparent_max_bounce -
  shadow_path.transparent_bounce, 0); blocked past it. Shadow shaders are evaluated with path flag NONE, so Is Singular
  and Is Reflection are blocked on shadow rays.
- Visibility resets at each non-transparent bounce but the shader eval gets RAW ray visibility, so Is Diffuse and Is
  Glossy are set on transmission. DIFFUSE visibility is not sticky. Caustic gate: reflective_caustics =
  caustics_reflective || !(ray_visibility & DIFFUSE). Closures that don't honour the gate, or half-gate glass (zero eval,
  keep pdf), add variance.
- Bounce cap: a non-transparent scatter at bounce >= max_bounce sets terminate-after; the next glass surface gets no BSDF
  so only emission survives. A path through k glass interfaces needs max_bounces >= k-1. At the cap a transmission sample
  keeps weight f/pdf (incl. 1/eta^2) and goes straight through (wi = -wo). Resetting throughput to 1 darkens slabs to
  1/2.25 of the world. Opaque surfaces at the cap are dark by construction.
- Cycles keeps ray direction through transparent passes (moves only tmin), leaves MIS state untouched, gives cryptomatte
  no credit on transparent samples. Motion triangles: lerp each normal, renormalise, blend barycentrically
  (motion_triangle.h). "Indirect only" objects have camera visibility cleared (object.cpp ~line 186), skipped in traversal.

### MIS and direct lighting
- Veach & Guibas 1995; Veach 1997 thesis (power heuristic, beta = 2, Sec. 9.2). Cycles: sample/mis.h, light/sample.h
  (light_sample_mis_weight_nee), shade_surface.h, shade_light.h.
- Bug class: applying only the NEE weight and dropping the BSDF-leg weight for diffuse bounces biases direct light low by
  the integral of w_B f Le. Compact lights hide it; large or near lights expose it. Fix: add throughput * Le * w_B when a
  diffuse continuation hits an emitter; the reverse pdf must use the same light-selection and solid-angle pdf as NEE
  (including light-tree pdf). Test: max_depth = 1 isolates NEE, max_depth = 2 adds the two-sided term; analytic oracle
  L_r = rho L_e (R/d)^2. NEE weight lightPdf^2/(lightPdf^2 + bsdfPdf^2 + eps) matched Cycles term-for-term.
- Light tree (Conty & Kulla 2018): v1's importance used max(distance - bboxRadius, 1e-6), inflating importance up to 1e12
  inside a cluster sphere. Cycles clamps distance >= 0.5 * |centroid - bbox.max| and blends min/max importance. A faulty
  light tree was the largest single noise source vs Cycles. Emissive-geometry shadow rays must apply light-front rules
  consistently (triangle vs sphere lights differed in v1).

### Adaptive sampling
- Zero-knob auto threshold from Cycles: threshold = max(0.001, 1/aa_samples); min_samples = max(4, ceil(16/threshold^0.3));
  then threshold *= 5, check step 16. Metric: error = sum|I-A| * scale / (1e-4 + sqrt(I) or I); checks at samples 15, 31,
  47, ...; dilate the unconverged mask 3x3. References: Dammertz et al. 2010; Christensen et al. 2018.
- v1 used a luminance-only half buffer (halves memory), so the stop metric was colour-blind: chroma noise did not trigger
  more samples. Adaptive sampling corrupts any variance or firefly statistic: turn it off when measuring.

### Fireflies
- Definition: Zirr, Hanika, Dachsbacher 2018; path notation Heckbert 1990. (The Blender manual's clamping text could not
  be verified; fetch returned 403.)
- To test an indirect clamp, make a real firefly population: a tiny very bright emitter outside the room, sealed by a
  thin glass pane (delta vertex), so the only route is diffuse, delta, emitter. That makes the channel NEE-invisible and
  bounce-indexed as indirect. Design window: R >= 10, n_ff <= 0.001 N, share s < 0.02; N >= ~6000 pixels (5e4
  comfortable); ~10 fireflies costs ~5e6 primary samples. Measure in linear space (a gamma clamp to 1.0 destroys the
  tail). A glass-sphere caustic layout fails: a focused emitter in frame is a deterministic fake tail.

### Path guiding
- Müller, Gross, Novák 2017 (SD-tree) gotchas: train from a frozen previous guide MIS-mixed with BSDF (BSDF-only training
  left zero-support holes); refine structure before the final splat; splat Li/pdf, not f Li/pdf; equal-area map
  x = cos theta, y = phi/2pi; refine one level per iteration, splitting leaves with flux share > rho (~0.003-0.01);
  one-sample balance-heuristic MIS, alpha ~ 0.5; training budget should double per iteration.
- Integration gotchas: fold training passes into the image (still unbiased) rather than discard; the 1/(1-alpha)
  undercover penalty gives a dark bias growing with alpha, so add a coverage floor; verify the warp is
  measure-consistent and MIS weights sum to ~1; E[1/pdf] equals the support area (not worth re-checking). Gate on mean
  ratio, not SSIM. Radiance guiding converges to the BSDF you already have: Lambertian oracle ceiling ~1.1-1.3x.
- Licences: practical-path-guiding and robust-vmm-guiding GPL-3.0 (reference only); OpenPGL Apache-2.0 (CPU,
  SD-tree/vMF); Vorba source no stated licence; the METU wavefront-guiding thesis CC BY-NC-ND. No permissive public SVO
  implementation exists; that route is paper-only. Suggested order: CPU first (SVO build, PDF query, MIS). Wavefront
  risks: shade-kernel register budget; the SVO paper's cone tracing uses OptiX; GPU atomics contention.

### ReSTIR
- Bitterli et al. 2020 DI: merge uses p_hat re-evaluated at the destination pixel; temporal invalidation if normal dot
  < 0.9, depth difference > 10%, out of bounds, or invalid history; spatial reuse from N = 5 random neighbours in a
  (2R+1) window (R = 15, R = 5 on CPU); double-buffered history; ship biased first, add shadow-ray correction after
  measuring bias.
- M-cap bug class: cap the SOURCE reservoir's M before merging (Bitterli Sec. 5.2). Capping the combined M after the
  merge freezes stored M while w_sum keeps growing, W rises linearly and the mean drifts up. It was invisible on the CPU
  path and visible on GPU; an m_cap sweep discriminates. A GPU gate once measured the CPU path because the test never
  selected the GPU backend: tests must assert which backend ran.
- Wavefront design: Bitterli-style DI (not path-space GRIS) in SoA; reservoir core as a shared host/device template over
  the RNG type; stages: initial RIS (M-cap 20x M), temporal, spatial, resolve with shadow ray. At transparent surfaces,
  keeping the surface with probability alpha vs Cycles' sample-weight scaling: same expectation, different variance.
- GRIS (Lin et al. 2022): github.com/DQLin/ReSTIR_PT is BSD-3-Clause (NVIDIA copyright 2020; attribute as
  "DQLin/ReSTIR_PT <file> (BSD-3-Clause)"), a structure reference only (Falcor/Slang). Offline recipe: temporal off, 32
  candidates, 3 spatial rounds, 6 neighbours, 10 px radius. NVIDIA RTXDI is disqualified (proprietary licence s4(e)).

### Neural radiance cache (prototype)
- tiny-cuda-nn (BSD-3) works on Ada (sm_89) and Blackwell (sm_120) with fixes: explicit include dirs under FetchContent;
  v1.3 FullyFusedMLP gave illegal memory access on sm_89 (use master with TCNN_CUDA_ARCHITECTURES set explicitly, 89;120);
  TCNN_MIN_GPU_ARCH static_assert; width multiple of 16; batch multiple of 128 (256 on Blackwell); set_params before
  forward; new CUDA runtime needs a new driver; the VS CMake generator ignores CMAKE_CUDA_COMPILER (use Ninja or NMake).
- Setup: FullyFusedMLP width 64, 2 hidden layers, identity encoding, RelativeL2 loss, Adam lr 1e-3, targets clamped at 10,
  inference at 5. Not performance-positive: per-sample queries were slower than the spectral path tracer.

### RNG and samplers
- For a stateless wavefront keyed by (pixel, sample, dimension): PCG32 XSH-RR with pbrt-v4 keying: seq =
  (pixel*max_samples + sample)<<32 | dim; stream = MixBits(seq); inc = (stream<<1)|1; init state 0, advance, state +=
  seed, advance. Tests: reseed determinism, stream disjointness (correlation < 0.01). Not cryptographic (acceptable).
  Reserve the dimension counter for QMC. Battery: PractRand 0.96 (TestU01 abandoned: unbuildable on Windows/MinGW).
- Progressive low-discrepancy: hash-based Owen-scrambled Sobol' (Burley 2020) with pbrt-v4's fast scrambler fits a
  stateless design (~2 bit reversals, ~5 integer ops, XOR fold of up to log2(spp) terms; direction-vector table in
  constant memory). PMJ02 rejected: needs tables and per-pixel state.
- Per-path RNG and tile-shared RNG are statistically equivalent but byte-incompatible. Fix the draw order (filter u,
  filter v, lens, wavelength, then path draws). A seed of 0 meaning "random device" was a flake trap for tests.

### CPU/GPU agreement testing
- Bit-exact CPU-to-GPU equality is impossible (FMA contraction, vendor sin/exp, PTX intermediate rounding, fast-math).
  Proposed gates: ULP bound on early stages, p99.9 relative error later, final SSIM. CPU-to-CPU can demand exact 0.
- Bit-identity needs the same code, not a replay. v1's three divergences: hand-counted RNG-dimension replay instead of one
  live generator; a second BVH trace to recover the material, with a different tmax; rebuilding a Ray from SoA scalars
  (the constructor renormalises, 1-ulp drift). -ffp-contract=off is only a symptom patch. Recommended: one shared
  per-bounce kernel for oracle and wavefront carrying the live RNG and hit record (matches the rewrite's one-kernel rule).
- Independent Monte Carlo streams cannot reach SSIM >= 0.97 between backends; gate on ROI energy plus a visual check.
  Prefer formula parity (|wt - power_heuristic| <= 1e-5) over raw values from different samples.
- Welch bias test (Jung, Hanika, Dachsbacher): per-tile two-tailed Welch t per channel; one sample = tile mean of one
  render (8x8 px, 128-256 spp), N = 8 renders per backend (df ~ 14); add Holm-Bonferroni, Benjamini-Hochberg, a KS test of
  the p-histogram vs U(0,1), optional equivalence margin. Paper code has no stated licence; reimplemented with
  scipy.stats.t. A high p proves nothing; a uniform histogram does not prove unbiasedness; very low noise makes it
  hypersensitive.

### Noise relative to Cycles; spectral noise; motion blur
- Heavy tails: 0.1% of pixels hold 95-100% of variance in media and dispersive-caustic scenes. Gate at equal time on the
  same device class. A white-noise hero wavelength gives strong chroma noise on sky/emitters (Cycles RGB: exactly 0);
  stratifying the hero-lambda uniform across a pixel's samples cuts sky noise ~75x at 64 spp (model). v1 used plain MC
  while Cycles' Sobol-Burley gain went unclaimed. Blue-noise Sobol variant: perceptual only.
- Motion blur: time is a fifth sampling dimension (Cook, Porter, Carpenter 1984); v1 carried a ray time that stayed 0.
  Cycles BVH uses a per-primitive time split with prim_time early-out, not per-node bounds. pbrt-v4: AnimatedTransform
  with T/R/S decomposition and slerp (Shoemake 1985), MotionBounds; ~696 B vs 128 B per instance; Shoemake & Duff 1992
  polar decomposition is the Mitsuba-style alternative (not chosen). Slerp from day one; no default-argument getRay.
  Normals must be interpolated at ray time like vertices (two motion steps suffice). Motion blur and motion-vector
  denoising are mutually exclusive (Cycles rule). Planned phases: camera, object (baked into deformation), deformation
  plus motion BVH, wavefront per-slot time. Follow-up: STBVH (Woop et al., HPG 2017).

## Sources and reference implementations

Papers (as recorded):
- Hanika, Droske, Manakov 2015, EGSR, DOI 10.1111/cgf.12681 (MNEE). Zeltner, Georgiev, Jakob 2020, DOI
  10.1145/3386569.3392408 (SMS); code tizian/specular-manifold-sampling (BSD-3, Mitsuba 2). Jakob & Marschner 2012, DOI
  10.1145/2185520.2185554 (Manifold Exploration/MMLT).
- Fan et al., "Specular Polynomials", SIGGRAPH 2024, ACM TOG 43(4), arXiv 2405.13409 (CC BY 4.0), DOI 10.1145/3658132;
  mollnn/spoly has no licence (do not copy). Yuksel 2022 root finding; cyCodeBase/cyPolynomial (MIT). Deferred: batch SMS,
  Springer Visual Computer 2025, DOI 10.1007/s00371-025-03955-0; SMBS, Jhang 2022, DOI 10.1111/cgf.14673.
- Wilkie et al. 2014, EGSR, DOI 10.1111/cgf.12419 (hero wavelength). Arvo 1986, Backward Ray Tracing, SIGGRAPH course notes.
- Jensen 1996 EGWR, DOI 10.1007/978-3-7091-7484-5_3; SIGGRAPH 2000 Course 8 (Fig. 7 balance, Fig. 10 locate, Eq. 8); 2001
  book (Sec. 5, 7.1, 7.2, 9, 9.1-9.3, App. B, cone norm 1-2/(3k)). Bentley 1975, CACM 18(9). Hachisuka & Jensen 2009
  (SPPM); Knaus & Zwicker 2011 (PPM); Teschner et al. 2003; Sellmeier 1871; Schlick 1994; CIE 1964 10-degree CMFs; Duff
  et al. 2017.
- Veach & Guibas 1995 SIGGRAPH; Veach 1997 thesis Sec. 9.2; Conty & Kulla 2018; Dammertz, Hanika, Keller, Lensch, WSCG
  2010; Christensen et al. 2018, DOI 10.1145/3182162; Zirr, Hanika, Dachsbacher 2018, CGF 37(6), DOI 10.1111/cgf.13335;
  Heckbert 1990.
- Müller, Gross, Novák 2017, CGF 36(4), DOI 10.1111/cgf.13227; Müller 2019 course, DOI 10.1145/3305366.3328091 (page range
  disagrees across sources); Vorba et al. 2014, ACM TOG 33(4), DOI 10.1145/2601097.2601203; Yalçıner & Akyüz 2024,
  Computers & Graphics 121, DOI 10.1016/j.cag.2024.103945, arXiv 2405.06997 (CC BY 4.0); Ruppert et al. 2020, DOI
  10.1145/3386569.3392421; Neural Importance Sampling, ACM TOG 38(5), DOI 10.1145/3341156.
- Bitterli et al. 2020, DOI 10.1145/3386569.3392481 (Sec. 5.2, Fig. 7, Alg. 2, Alg. 4); Lin, Kettunen, Bitterli,
  Pantaleoni, Yuksel, Wyman 2022, ACM ToG 41(4), DOI 10.1145/3528223.3530158 (GRIS); Wyman 2023,
  https://intro-to-restir.cwyman.org/ ; tatran5/Reservoir-Spatio-Temporal-Importance-Resampling-ReSTIR; Müller et al.,
  SIGGRAPH 2021 (neural radiance caching); tiny-cuda-nn (BSD-3); instant-ngp.
- Burley 2020, JCGT 9(4), https://jcgt.org/published/0009/04/01/paper.pdf; Christensen, Kensler, Kilpatrick 2018, CGF 37(4)
  (PMJ02); Ahmed & Wonka 2020 (ZSobol, follow-up); O'Neill, HMC-CS-2014-0905, ACM TOMS 47(3) art. 26 (2021),
  https://www.pcg-random.org/paper.html.
- Rath 2022 (EARS; cited from memory, verify); Meyer 2024 (MARS, 1.6x over EARS on BDPT); West et al. 2020 SIGGRAPH
  (continuous MIS over wavelength); Cook, Porter, Carpenter 1984, DOI 10.1145/800031.808590; Shoemake 1985; Shoemake &
  Duff 1992; Woop et al. HPG 2017; Jung, Hanika, Dachsbacher, JCGT 9(2) 2020, https://cg.ivd.kit.edu/welch.php;
  Goldberg 1991 (ULP); Wang et al. 2004, DOI 10.1109/TIP.2003.819861 (SSIM).

External-engine files read (as recorded):
- Cycles (Blender; Apache-2.0 unless noted): intern/cycles/kernel/integrator/mnee.h (seed 29-44; ManifoldVertex 59-88;
  mnee_compute_constraint_derivatives 248-365; mnee_compute_transfer_matrix 663-731, G = clamp(dw0_dx1 . dx1_dxlight, 2);
  manifold walk 824-903; mnee_path_contribution ~870-900, 1008+). Also integrator.cpp, adaptive_sampling.cpp,
  film/adaptive_sampling.h, state.h, init_from_camera.h, intersect_closest.h (light_sample_from_intersection),
  shade_surface.h, shade_light.h, path_state.h, surface_shader.h, kernel/light/sample.h, kernel/sample/mis.h (BSD-3-Clause),
  kernel/random.h (Sobol-Burley), util/hash.h (hash_pcg3_uint), tree.h (light tree), closure_principled.h,
  motion_triangle.h, object.cpp, device/cuda/queue.cpp. Blender v5.2.0 for bounce and path-flag semantics. Refs: commit
  1fb0247497 and T94120 (MNEE), T96992 (MNEE caustics classed direct), T96991 (no fallback), c4890cd354 (motion BVH timings).
- pbrt-v3 src/integrators/sppm.cpp (BSD-2); pbrt-v4 (Apache-2.0): SPPM in integrators.cpp ~L2763-3229, util/rng.h,
  util/hash.h (MixBits), lowdiscrepancy.h (fast Owen scrambler), ZSobolSampler, AnimatedTransform.
- Mitsuba 3 (BSD-3): ptracer.cpp (photon power), src/render/tests/test_renders.py (statistical-test precedent); Mitsuba
  3.9.1 used as a spectral oracle; Mitsuba 3 dropped motion blur. The sorecords Blender add-on (GPL-3) is reference only.
- OpenPGL (Apache-2.0); DQLin/ReSTIR_PT (BSD-3); imneme/pcg-c-basic (Apache-2.0 OR MIT); PractRand 0.96 (public domain).

## Measured numbers worth keeping

Caustics (GPU/CPU luminance ratio to an analytic or oracle reference):
- After the Fresnel split: floor_rainbow 1.006 -> 0.998; tir_beam 1.395 -> 0.998; reflection_beam 1.141 -> 1.001;
  prism_sun_glint 0.978 (CPU 0.987). Production-style corpus: prism floor 1.005; spot floor 1.009 (CPU 0.0782 vs Cycles
  MNEE 0.0783); sphere limb 1.022. Prism lifted 5 mm off the floor: oracle 7.03, CPU 6.93, GPU 6.87.
- One map per render, ROI std at 16/64/256/1024 spp: 0.0018/0.0014/0.0012/0.0013 (ideal 8x drop); k-NN singularity spike
  ~1435; path-traced refractive caustic double count (16-spp max 3340 vs photon-only 1.5); a 3M-photon CPU map per render
  froze speckle at 256+ spp.
- Photon flux (CPU): slab furnace patch/direct 0.926 vs T 0.9216, flux 2.355 vs 2.369; ball lens point lamp 1.7246 vs
  numpy 1.7409 (~1% band drop), radial fractions to 0.1%; caustic core flux vs 16384-spp PT: area 1.00, sun 1.06,
  point/spot 1.06-1.07; PT at 4096 spp ~15% low.
- Glass-sphere GPU/CPU ROI 1.09x after the normal fix (433x before), SSIM 0.962; deposit RMS spread 0.83 GPU vs 0.15 CPU
  before it. Prism hue spread: forward tracer 0.754 (gate 0.7), bright coverage 0.88; general stochastic loop on a solid
  prism 0.52-0.60 (non-monotonic in photon count), deterministic solid prism 0.475.
- Kd-tree k-NN matched brute force on 300/300 queries; density within 1.6% at 40k photons. Chain Jacobian vs finite
  differences ~1e-11 (MNEE geometry term ~7.6e-11 positional, ~2e-4 collimated limit); Newton 3-4 iterations on a
  2-refraction prism chain. Degree-6 roots matched a 200k-sample scan to <1e-4 rad; polynomial ROI energy 0.65x the biased
  Newton weight. MNEE ~2-3x slower than SMS.

Blender 5.2 semantics: three stacked transparent sheets lit only for transparent_max_bounces >= 4; N sheets between floor
and sun lit only if the limit >= N; camera-side 3 sheets black for limits 3-5, lit at 6 and 8; shadow probe Is Shadow Ray
lit 0.89/0.81. Bounce-cap stack ROI at cap 4: 0.46 throughput reset vs 0.89 weight kept vs 1.0 uncapped; within 0.004 of
Cycles at caps 1-64. Adaptive auto-threshold: 64 spp -> min 56, thr 0.078; 256 -> 85, 0.0195; 1024 -> 127, 0.005. Cycles'
adaptive reads heavy-tailed caustics 10-25% low at 2048 spp.

Noise:
- Light tree off cut the variance ratio vs Cycles 8-14x on a multi-light interior (wall 83 -> 7.7, floor 200 -> 14, D 145
  -> 10; 32 spp, filter 1.5 px). Residual 4-9x luminance, 50-120x chroma (suspects: 4-lambda chroma noise, pixel filter;
  unmeasured). Removing a practical bulb changed nothing; progressive Sobol 8-36%.
- Equal-time efficiency vs Cycles (1.0), GPU 60 s, 1024-spp Cycles reference, seeds 278-282, RTX 5070 Ti: camera 0.134,
  light_tree 0.261, media 0.006, dispersion 0.014, sky 0.067. Per-spp slowdown 1.4-3.8x. Dispersion variance 248x Cycles;
  media relVar 10x and rising with spp. Later corpus (64 spp, 5 seeds): relMSE ~7-40x Cycles OptiX at equal time;
  3.8-9.1x slower per sample on GPU, 3.0-4.6x CPU; equal-spp bulk variance 0.6-5x. N x relVar slope 4 -> 64 spp: Cycles
  0.24-0.71, v1 ~1.0. Sky per-channel relVar at 64 spp: measured R 0.0024/G 0.00018/B 0.0026 (model 0.0021/0.00029/0.0034);
  Cycles exactly 0.
- Versus a 16384-spp Mitsuba 3 oracle both v1 backends matched within ~1% on most ROIs; Cycles lacks dispersion (63-75% of
  floor beams). Exact IEC 61966-2-1 film conversion with per-channel clip fixed desaturated out-of-gamut sodium light (R/G
  4.52 -> 3.90). Sun-lit floor analytic within -0.3%.
- Firefly scene: peak/p99.9 = 22.85x (gate >= 10); negative control 1.07x (gate <= 3); best earlier scene 1.82x.

Guiding: SD-tree core without NEE: truth 0.04308; BSDF variance 9.8e-3; guided 2e-5 (~490x); MIS(guided, BSDF) 9e-5 (~110x,
bias -2.6%). Integrated, equal budget: hard_transport ~1.3x (alpha ~0.3, K = 8-10), moderate_indirect 0.7-0.9x,
veach_door 0.4-0.6x; undercover: alpha 0.1 ratio 1.00, 0.9 ratio 0.37. SVO paper Table 3 (Sponza, SVO 128^3): 3.96 MiB vs
SD-tree 19.0/26.0/37.0 MiB at 16/32/64 training spp; VeachDoor 1.26 MiB; update-exitance 4-6.5 ms/sample; ~180 ms vs
74 ms path tracing (~2x).

ReSTIR M-cap bug: GPU mean 0.283 -> 0.378 over 40 frames (onset ~frame 20 at cap 80); prototype buggy 2.359 (std 0.421)
vs fixed 1.082 (std 0.021), truth 1.0; m_cap sweep slope +0.00485 at cap 2, +0.00407 at 80, ~0 at 1e5; CPU flat; a
mis-selected backend read 0.397 (CPU) vs 0.283 (GPU). Neural cache (RTX 5070 Ti): mean luminance 0.2841 (frame 1) -> 0.4040
(16) -> 0.4317 (50).

CPU/GPU gate placeholders: CPU-to-CPU max_abs_diff 0.0, SSIM 1.0; proposed <= 4 ULP early stages, p99.9 relative error 1e-4
(provisional), SSIM 0.985 visible / 0.97 NIR. v1 wavefront vs megakernel: 1.494-1.539x median-of-5 (a 0.90x overnight
reading was a single-sample artifact; use median-of-N); megakernel registers 125 -> 188 crossed a 158-register occupancy cliff.

Motion blur: BVH time steps 0 -> 3 gave 46 -> 27 -> 18 -> 15 s, 260 -> 826 MB (Cycles commit c4890cd354); moving lit vane
0.74 with stale normals vs 0.998 static; emission-only vane coverage 1.67 vs Cycles 1.71.

## Dead ends and negative results

- Camera-side SMS/MNEE on a flat prism; smoothed-normal SMS (no precedent, biased); MMLT (unstable); a radiance-based SMS
  variant gave chromatic noise at 160^2, 256 spp, not a rainbow.
- General stochastic photon loop on flat-prism casters: a 2-quad caster deposits nothing; a solid prism scatters into
  chromatic noise; more photons made it worse (bottom/cap scatter desaturates the ROI). Resolved with a deterministic
  two-face path for flat prisms and the general loop elsewhere. Hue-spread and bright-coverage gates passed on dense
  chromatic noise: always look at the caustic render.
- Blaming GPU intersection numerics for the glass-sphere caustic gap (an adaptive gather did not change the 428x ROI; it was
  a normal-orientation bug). Resetting throughput to 1 at a bounce-cap passthrough.
- BSDF-only guide training; radiance guiding on Lambertian scenes: 2x never materialised in the real integrator, adding
  leaves often hurt, spatial resolution bounded by 2^(K-1) leaves.
- Neural radiance cache: loss plateau (~0.43 after 16 warmup frames); queries slower than the path tracer.
- CPU ReSTIR temporal reuse accumulated almost nothing (~0.3% variance change); suspected cause (temporal-validity
  rejection under AA jitter or pixel rounding) never verified.
- RTXDI (proprietary licence); TestU01 (unbuildable on Windows); PMJ02 (not stateless); Mitsuba 3 has no motion blur.
- Bit-exact CPU/GPU comparison and tolerance-based snapshot diffs that hide a different computation; one reference trying
  to be byte-equal to both a tile-RNG and a per-path-RNG implementation.
- Keeping a path-traced refractive caustic estimate alongside the gather; trusting Cycles adaptive sampling on heavy-tailed
  caustics.

## Design ideas worth reconsidering in the rewrite

- Caustics: forward photon transport with per-round fresh maps (SPPM-style progressive radius was dropped and never
  evaluated), exact Fresnel, power-not-radiance transport, Fresnel roulette, caustics classified indirect, one explicit
  cull rule shared by NEE and the photon map. Deterministic polynomial specular solvers (Fan 2024), analytic Jacobians,
  MNEE geometry term instead of the seed-area pdf.
- Wavelength: stratify the hero-lambda uniform across a pixel's samples; wavelength splitting on non-dispersive paths;
  lambda-splitting at dispersive vertices; EARS/MARS-style RR with lane-count cost; continuous MIS over wavelength (West
  2020); spectral path guiding; guiding plus QMC. Chroma gate (R/B relVar <= 0.1x on a white emitter); equal-time gates on
  the same device class.
- Sampling and testing: Owen-scrambled Sobol' default from the start with PCG32 keyed by (pixel, sample, dimension); inverse-CDF
  pixel filter; chroma-aware adaptive stop metric; one shared bounce kernel for CPU and GPU with per-stage snapshots; thresholds
  pinned from a CPU-to-CPU baseline; Welch-t tiles plus formula parity, always with a looked-at render.
- Light tree with Cycles' distance clamp and min/max importance blend; test that importance never blows up inside a
  cluster sphere. If guiding is revisited: product guiding (f cos Li), multi-level splits, coverage floor, and a scene where
  NEE fails and transport is learnable. Motion blur: time in rays from day one, per-primitive time-split BVH, T/R/S slerp,
  normals at ray time.
- Blender semantics to honour on import: transparent-bounce budget applies to shadow rays; rays keep direction through
  transparent passes; bounce-cap passthrough keeps weight; opacity-weighted denoise-guide blending (Cycles) left as a
  follow-up.

## What no longer applies

- Matching Cycles' MNEE seed/Newton details or energy for pixel parity (algorithm reference only).
- Cycles shadow-ray budget, bounce-cap, caustic-gate flags, film clamp and adaptive thresholds as parity targets
  (Blender-semantics knowledge only); Cycles ROI parity matrices; Cycles lacking dispersion as ground truth.
- v1's wavefront-vs-megakernel speed gates and megakernel deletion plan; RGB megakernel two-sided MIS; RGB-only integrators.
- Register-saturation work on v1's shade kernel (254 regs, stage splits) and rebuild cost of a widely-included device header.
- v1 per-scene acceptance thresholds (prism hue_spread 0.7, bright_coverage re-bless 0.45 -> 0.13).
- Replay-based CPU wavefront oracle design (hand-counted RNG dimensions, second BVH trace).

## Open questions

- Is SMS plus NEE-cull (caustics classified indirect) better than forward photon mapping? Not built or measured. Does a
  per-wavelength SMS converge well enough for dispersive caustics (unpublished)?
- Cone/k-NN gather on GPU, progressive radius reduction, and gather at the first diffuse vertex after a specular chain were
  deferred and unevaluated. Photon emission for mesh emitters, radius lights and multiple casters is unsolved.
- Why did CPU ReSTIR temporal reuse accumulate almost nothing? Unverified.
- Is the large-light bias of one-sided MIS visible in user scenes? Needs a converged brute-force ground truth.
- Unmeasured noise suspects: RR (v1 p = min(0.95, Y) from bounce 4 vs Cycles min(1, sqrt(max|T|))); hero pdf/4 at
  dispersive vertices; pixel filter. Remedies for dispersive tails, media variance rising with spp, and heavy-tailed
  caustics under adaptive sampling were proposed but not measured.
- Guiding: inverse-variance vs sample-count combination (sample-count first recommended); does guiding ever give >= 2x on a
  real scene with NEE? Can a neural radiance cache become performance-positive (warmup 32-64 frames, hash-grid encoding)?
- Motion-blur shutter curve and per-object step count were deferred. Licence of Cycles' MNEE header is recorded
  inconsistently (GPL-2.0+ vs Apache-2.0).
- Thin in the digests: final SMS quality numbers, motion-blur gate results beyond the normal fix, measurements of the
  Owen-scrambled Sobol' sampler (research input only), guiding vs NEE on production scenes.
