# Validation: gate and metric studies, reference corpus design, diagnosis and attribution

Source: v1's validation research (gate scoring, metric studies, corpus design, issue triage, diagnosis write-ups). v1
judged correctness largely by parity with Cycles; the rewrite judges against physics and OpenPBR. The *methods* below
transfer; the Cycles-specific targets are kept only as knowledge of Blender semantics. Digests were thin on some
points; those are marked.

## Established findings

### Image-comparison metrics: SSIM is the wrong gate under Monte Carlo noise
- SSIM >= 0.95 between two independent renders of the same scene mostly measures noise, not bias. Terrace-like scenes
  with floor speckle scored SSIM 0.78 while no tile mean differed; a smooth scene scored 0.96-0.98 while the same
  pipeline had real per-pixel differences. SSIM-of-the-mean failed the negative control on one scene.
- An HDRI scene failed SSIM 0.77 vs a 0.97 pin. Cause was not engine parity: per-pixel blue-channel difference-std
  stalled at ~0.05 from 1024 to 4096 spp because the adaptive-sampling stop used a scalar, colour-blind criterion
  (luminance X+Y+Z, half-buffer variance, Dammertz-style). With adaptive off, blue noise fell 0.0295 to 0.0149 as
  sqrt(N). Reproduced with no GPU: two independent CPU streams at 8192 spp gave SSIM 0.677 (adaptive) vs 0.962 (off).
  Lesson: the stop metric must be per-channel or the comparison must disable adaptivity.
- Per-image max normalisation (arr/max) before comparing makes a single firefly set the scale and caps converged SSIM
  near 0.96. Use a common exposure for both images, never per-image normalisation.
- Diagnostic images must not be exposure-normalised: that hides power, temperature and spread errors.
- Independent-RNG comparison cannot reach high SSIM by construction; SSIM can only pass if both legs share noise or
  are heavily converged.

### Adopted replacement: tile-wise Welch test with multiple-comparison control
- Metric adopted by the owner after the study: partition both renders into 8x8 pixel tiles, RGB; per tile run a Welch
  t-test on N = 8 independent seeds per backend; Holm step-down correction at family-wise alpha 0.05; a 1% relative
  margin so negligible biases are not rejected; pass means zero rejected tiles. A +/-5% ROI channel-mean band is kept
  as a guard; SSIM is informational only.
- Why it works: it compares means, which is what "unbiased" means, and the noise level enters through the per-tile
  sample variance. Candidate "1b" (margin variant) had 0 missed positives on controls; SSIM missed 5, a ROI-ratio test
  missed 13, an SSIM ceiling missed 6, a 4x4 box test missed 12. On three scenes (7920 / 24116 / 5940 tiles) the
  converged pair rejected 0 tiles, negatives scored 0.97-1.00 rejection power.
- Reducer discipline: statuses are computed, never hand-set; the reducer fails closed on missing, duplicate or reused
  seeds and on any digest mismatch between legs. Evidence legs carry a per-leg log and mask, otherwise they are
  invalid.
- Caveats recorded: heavy-tailed fireflies can break tile-mean normality; GPU float-atomics make same-seed GPU pairs
  reproducible only to ~1e-6, so seed-pairing is not a free variance reduction; null calibration was done only at 4 vs
  4 (35 half-splits gave 94-100% pass, but they are not independent so that is not a calibrated false-positive rate);
  only three scenes were studied.
- The metric surfaced two real defects earlier tests missed: a GPU black panel (tile means 0.013 CPU vs 0.0 GPU) and
  procedural swatches (checker, noise, voronoi, wave) that differed pixel-by-pixel between backends. Root causes were
  not investigated in the digest.

### Feature-coverage gate ("does every supported feature actually reach pixels")
- A coverage claim rule: a feature input counts only if it is *active* (a wired Coat Roughness with Coat Weight 0
  earns nothing) and its effect is visible in a defined image region.
- Counterfactual witnesses: for each feature, render a baseline and a control that neutralises the feature (constant
  re-feed of 0.5 for noise and wave textures; constant for brick; zero for bump; identity for mapping; a control that
  changes nothing must raise). Effect = mean |diff| > 0.01 and >5% of pixels above 0.01, required in both engines. If
  the control is inert in both engines the witness is NO-EFFECT and earns no credit.
- Witness selection rules used: reachable, single-object material, mask of >= 300 px, one witness per variant digest;
  ROI = bbox padded 10%, optionally an object-faces mask eroded 4 px. A rectangle vs mask ROI changed no verdict
  across 27 candidates.
- A witness judges its whole ROI, so any scene-level divergence on that object zeroes every feature under test on it
  (one large-grid witness alone zeroed 9% of the score). A mask-limited or control-differenced Welch would be
  narrower; this was proposed, not frozen or run.
- Credit model: static credit per feature (supported 1, approximated 0.5, dropped 0), a frozen audit sample of
  witnesses rendered on both backends and the reference, eight independent baseline seeds, verdict from the Welch test
  over the witness ROI. A FAIL zeroes covered features; ERROR or a missing leg makes the report "unmeasured", never a
  pass.
- A score computed from a static scan of what the exporter reads can be inflated: a socket credited without being
  read, an output credited when only a restricted linkage is handled. Independent review found two such inflations;
  fix was to cap partially-handled cases at "approximated". Self-review by the implementing lane is not independent.
- Attribution rule: every lost-score cause gets exactly one root cause. Approximations need an attributable warning
  *and* a witness.

### Reference corpus design
- One variable per scene ("isolate the variable"): scenes are generated by code, not authored by hand; the only
  external files are small synthetic CC0 volumes. Each scene has an ROI table: rectangle, reference mean RGB,
  tolerance band (mostly 0.020/channel; wider on bright or noisy ROIs). Whole-sphere ROIs, so highlight position does
  not decide the mean. A ROI over a specular highlight (e.g. a sun reflection) is an artefact generator.
- Scene families used (each paired to what it catches): light tree / MIS bias, dedicated-lamp sampling, closed
  emitters; media (volume NEE, delta/ratio tracking, fire emission); dispersion and caustics (photon power, firefly
  ROI); sky and sun (disc energy, sky NEE, env reflection); thin-film and conductor means; procedural textures; camera
  and geometry (clip planes, instancing, hair, motion blur, mesh volume, orthographic variant). Earlier design had
  seven families (materials, textures/mapping, lighting studio, world/sky, geometry zoo, camera/lens, render settings)
  totalling ~520 matrix rows.
- "Three independent opportunities" world test: a world feature must show in the background, in a reflection, and as
  indirect light into a shaded recess. Black sky is not the only signature of a dropped feature; also test a
  plausible-default variant.
- Geometry with modifiers: three states (live, baked, undeformed base). Render-settings: six-panel pass sheet;
  bounce-limit rig sets all other depth limits high; procedural IES preferred over sourced files (IES encodes
  directional intensity, not a gobo image); adaptive sampling evidence is a per-pixel sample-count heatmap, not a
  before/after.
- Convergence plots need an independent higher-spp reference outside the plotted series: using the highest-spp image
  as its own reference gives a spurious cliff.
- Bless references deliberately on a known-good commit, never automatically. Detail-rich, high-contrast scenes so
  small regressions move the gate; pair SSIM-like image metrics with a domain metric (hue spread, concentration,
  dark-disk, dE). Parameters used: heroes >= 1280x720, gate scenes >= 512^2, reference >= 1024 spp, fixed seed,
  per-scene gate config.
- Documented divergences from the reference engine must be labelled, not gated: where physics differs (dispersion,
  sun/spot caustics through glass, caustic shadow fill, fog below floor) the expectation of the new engine is pinned
  and the difference is recorded. Owner rule: keep the physically correct behaviour where the reference engine
  differs.

### Diagnosing a "uniform X% dim" claim
- A claimed uniform 12-20% darker-than-reference offset was not reproducible: re-measurement on a common linear EXR
  gave 1.02 (backdrop probe), 1.0085 (world), 0.997 (diffuse), 1.016 (glossy); parity floor about +/-2% (green up to
  +5%) at 128 spp. Phase 1 order: check view transform / gamma / exposure / film on *both* legs first (both must be
  Standard, exposure 0, gamma 1, 32-bit linear). A large uniform dim is first suspected to be a display-transform
  mismatch on the reference leg. A uniform brightness multiplier is forbidden as a fix.
- A stale compiled module caused a false "analytic-exact" reading that had to be retracted: rebuild after reverting
  source before concluding anything.
- A harness setup mismatch (camera / world / light units) once accounted for half of a scene's reported gap; fixing it
  moved SSIM to 0.9994. Check the setup before attributing a gap to the engine.

### Attribution of chroma residuals
- Ten minor-channel residuals were all chroma-dependent (a grey albedo removes them); an independent spectral engine
  (Mitsuba 3.9.1 spectral) reproduced each shift within 0.003. The shift is spectral transport of a saturated albedo
  under a coloured light across multiple bounces, not a single-bounce bug. Meaning: an RGB engine and a spectral
  engine legitimately differ in coloured-light, saturated-albedo scenes; the reference must be spectral to be a fair
  oracle.
- Required controls, each receipted and refusing no-ops: grey albedo, flat white world, constant albedo, Lambertian,
  lights-only, HDRI world.
- Raw HDRI caveat: 84% of energy in six sun texels (peak 1.6e5); a spectral env-map in the reference engine was not
  texel-invariant (G 0.982, B 0.966 on a grey albedo). Workaround: replace the sun with a 2-degree Gaussian of equal
  energy (ratio 1.0007).
- Agreement between two implementations shows self-consistency, not ground truth.
- Clipping matters: an unclipped spectral reference (-0.094 in a chroma bowl) vs the film-clipped engine (+0.026);
  with direct light agreement was 0.009. Decide up front whether negative colour channels are clipped before
  comparing.

### CPU/GPU residual diagnosis (energy)
- A 0.9957 CPU/GPU ratio was not triangle-specific (sphere and triangle walls gave identical 0.99570). Two effects:
  (A) a universal ~0.6% energy loss per diffuse bounce from an additive epsilon in the denominator, f/(pdf + 1e-3),
  exactly 2*pi*eps = 0.628%; epsilon of 1e-6 restores 0.500 furnace reflectance. (B) a GPU-only single-bounce deficit
  caused by up-sampling a pre-scaled value (spectral up-sampling of a throughput that had already been multiplied by a
  factor); after the fix GPU/CPU = 1.00005 for every albedo.
- Fix for (A): guarded division, f * (pdf > 1e-8 ? 1/pdf : 0), rejecting degenerate samples; cites pbrt-v4. Brightens
  every diffuse bounce ~0.628% compounding; re-pin tests against the analytic prediction and a 0.5 furnace wall (Veach
  1997 for the 0.5 expectation).
- Ruled out as cause: epsilon (for the ratio), normal/cos-theta, JH upsampling tables (neutral match 1.000000),
  diffuse closure weight, GPU contribution clamp, cosine samplers (byte-identical), CMF / XYZ conversion tables
  (shared), D65 normalisation, occluder in the ceiling.
- A residual multi-bounce divergence (reproducers: floor 1.0096, corner 1.0145, box 1.0368 at depth 4 / 1.063 at depth
  2; GPU ~6% more bounce-1 escape events; bounce-1 GPU/CPU throughput 1.118) was left open; hypothesis was ray-origin
  / continuation-ray geometry sampling parity or precision. A shared -1.7% blue-channel deficit vs analytic across all
  legs was flagged, not investigated.

### ULP-pin and float-parity tests
- A max-ULP pin of 4 on a spectral wavelength array dated from a linear sampler (ULP 2). After switching to
  importance-sampled wavelengths (logistic CDF inversion: log/exp on CPU vs logf/expf on CUDA) relative error ~1e-6
  gave 8-13 ULP at 380-780 nm. CUDA's sinf/expf are not correctly rounded. Proposal: pin ULP per field, use a relative
  bound (p99.9) for transcendental-derived fields, or ULP ~16. Not run (GPU lock) at write time.

### Issue triage as a gate
- Rubric (owner-ratified 2026-09-07): high = wrong image, crash, or a native setting silently ignored; medium =
  degraded but flagged; low = cosmetic, tooling, perf-only. Gate green only with live snapshot reconciled and zero
  high.
- Process: two blind raters (different model families); keep the higher rating on a high/not-high split unless
  evidence is cited. One rater (Codex) rated liberally (any unflagged mean offset is high) and produced 21 highs vs 5
  from the other on the same snapshot; later multi-model panels (DeepSeek, Sonnet, Opus) with an independent check per
  pass converged. A green status was later re-reduced to red (0 -> 8 highs) when new issues landed; the manifest
  snapshot lagged the live list.
- Downgrades need cited evidence: variance with matching means, an export verified faithful, a reference-engine
  artefact, a single firefly on one compiler/seed.
- Owner physics-over-look rule (2026-09-29): keep caustic shadow fill that the reference engine omits.

### Gate scoring and progress over time (record of what moved)
- Viewport latency gate: GPU p95 102.1 ms (limit 100) / p99 116.0 (limit 150), then 78.2/109.7, then 62.9/100.6 after
  targeted fixes; cancel p95 33.1, p99 38.3 ms. One RED reading came from an edit captured without a
  presented-generation chain; the clean re-capture is the record.
- Denoise gate: mean relative error max 0.0049, detail preservation 0.9996.
- Eight-material production score: PASS needs >= 6/8 on CPU and GPU with zero silent pairs; went 2/8 and 1/8 -> 2/8
  and 2/8, bottlenecked by procedural bump, a shader-stack scene, a glass scene and a texture-roughness chain. A
  GPU/CPU inconsistency on one material (13/16 vs 2/16) is exactly the kind of thing a CPU-only gate would miss.
- Clean-install gate (second PC): requires Windows 10/11 x64, Blender in the default location, and *no* compilers,
  Windows SDK, CUDA toolkit, CMake, Ninja or checkout (a driver alone is fine); the script refuses ineligible hosts by
  design; procedure = capture (fresh isolated profile, install ZIP via the extension installer, enable add-on, 64x64
  F12 render, hashed probes) then validate (GREEN/RED with reasons). Release artefact measured ~1.06 GB.

### Viewport interactivity diagnosis (pan feels slow)
- Hypotheses and verdicts at 256x256, 1 spp, depth 4, 30 frames: (H1) scene upload during camera-only pan - rejected
  (0 calls), but confirmed for object-transform edits (+~50 ms CPU, +~80 ms CUDA at 100k triangles); (H2) accumulation
  reset every pan tick - confirmed (spp always 1; Cycles resets only on scene mutation); (H3) denoiser cost +86-110
  ms/frame on CPU, +1-2 ms CUDA, CPU figure suspicious (re-initialisation?); (H4) megakernel register pressure
  dominant (100k CUDA mean 101 ms vs CPU 56 ms; 158 regs/thread, 1 block/SM; Laine, Karras, Aila 2013); (H5) CUDA
  init: first frame 12,079 ms, then 13-14 ms.
- Implication: keep the accumulator across camera ticks; warm up CUDA inside scene sync; persist the denoiser; split
  kernels per material.

## Sources and reference implementations

| Source | Used for | Notes |
|---|---|---|
| Jung, Hanika, Dachsbacher, 2020 (Welch bias test; internal note, no URL recorded) | Tile-wise unbiasedness test for the CPU/GPU gate | Holm step-down correction for multiple comparisons |
| Holm step-down procedure | Family-wise error control across tiles | No full citation in digests |
| Dammertz et al. (half-buffer adaptive stop; no full citation given) | Explains the colour-blind stop metric that broke comparisons | |
| Pharr, Jakob, Humphreys, *Physically Based Rendering* 4th ed. (pbrt-v4), sec. 13.3 (SampleLd, beta update: break on pdf == 0, else beta *= f*abs(cos)/pdf) | Guarded pdf division | No URL recorded |
| Veach 1997 (no title or URL recorded) | 0.5 expectation for a furnace wall | |
| Laine, Karras, Aila, "Megakernels Considered Harmful", HPG 2013, sec. 6 | Register pressure vs wavefront | |
| Walter et al. 2007 (microfacet transmission; pbrt-v4 also; no URL) | Geometrically invalid refraction pairs | Reference engine handles them via NEE only |
| Mitsuba 3.9.1 (scalar_spectral / scalar_rgb) | Independent spectral oracle for chroma attribution | Agreement = self-consistency |
| Jakob-Hanika spectral upsampler (independent implementation) | RGB-to-spectrum | |
| Cycles `intern/cycles/blender/session.cpp` (BlenderSession::reset, view_draw), `intern/cycles/integrator/path_trace.cpp` (PathTrace::path_trace_iteration); Apache-2.0, pattern only | Viewport reset / progressive-refine semantics | Cycles resets only on scene mutation |
| Cycles `tests/render/` ("isolate the variable" test scenes; no URL) | Scene-design convention | |
| Cycles `transparent_bounce` / `transparent_max_bounces` semantics | Transparent pass-throughs: Cycles counts them separately from path bounces; shadow-ray transparent walks | v1 diverged; semantics worth confirming for the rewrite |
| Blender 5.2 Sky Texture docs (Multiple Scattering, Nishita single scattering, Hosek/Wilkie legacy; no URL) | World/sky scene design | |
| Poly Haven CC0 HDRIs `syferfontein_18d_clear`, `kloofendal_48d_partly_cloudy` | Candidate world assets | |
| CUDA note: sinf/expf are not correctly rounded | ULP pin reasoning | Recorded in a threshold file header |

## Measured numbers worth keeping

- Welch tile test, converged CPU vs GPU, 8 seeds: 0 of 7920 / 24116 / 5940 tiles rejected on three scenes; ROI
  channel-mean max gap 0.61-0.73%. SSIM on the same pairs: 0.964 / 0.978 / 0.778 (and as low as 0.769). Cost: CPU legs
  dominate (27 s CPU vs 1 s GPU per leg on the gallery scene; Welch N = 8 + 8 about 224 s; analysis 50 s); cache one
  set of CPU legs per build.
- Energy bookkeeping: additive pdf epsilon = exactly 2*pi*eps = 0.628% per diffuse bounce; furnace wall reflectance
  0.500 after the guarded divide; CPU luminance [0.49699, 0.49777, 0.48845] before.
- Spectral/chroma attribution: ten residuals within 0.003 of a spectral oracle; dilution factors 0.58-1.75;
  saturated-cyan red channel 1.19x. Example residuals: curves inner-backface r 0.857 (grey albedo 1.002; flat white
  world 0.457); wood grain centre b 0.941.
- Glass limb/centre ratio, reference engine vs v1 (r = 0.0 / 0.2 / 0.5 / 0.85): 1.603 vs 1.547; 1.743 vs 1.559; 1.919
  vs 1.868; 1.748 vs 1.370. Remaining limb deficit at r >= 0.85 because the reference evaluates geometrically invalid
  refraction pairs only through NEE.
- Reference-engine behaviour worth remembering: narrow-band lamp R/G was 14% below the CIE integral of its own SPD (a
  v1 bug); sun-lit floor was 12-16% bright vs two other engines; spot shaft through fog 0.70 CPU / 0.75 GPU; prism
  caustic 0.74 of Mitsuba (GPU 0.92); GPU vs CPU caustics up to 6.8x; indirect clamp removes 9-10% in media vs the
  reference's 2%; low-sun disc 1.24x, glow 0.91x; Disney half-metal 0.91-0.93x.
- Witness study: 27 candidates -> 11 pass, 8 NO-EFFECT, 8 fail (same on both backends). Bump effect 0.0000 (v1) vs
  0.056 / 0.015 / 0.009 (Cycles); brick 0.029 vs 0.320; displacement 0.000 vs 0.009. Hero-object witnesses at 64 spp
  gave SSIM 0.58-0.73, dE 3.7-4.9, i.e. pure MC noise. Dim cards (signal 0.018 / 0.038) fell under the absolute 0.01
  effect floor despite 14-17% relative effect. Provisional score S = 0.004 (rep) / 0.058 (fan-out).
- Score structure of a feature-coverage gate: matrix-only ceiling 0.42-0.43; lost weight causes: no pixel witness for
  supported rows 27%, approximated Principled sockets 18%, output sockets without matrix row 15%. With three causes
  repaired ceiling 0.687 / 0.726 (0.78 / 0.81 with Principled lifted). Later audited model: ceiling 364/474 = 0.768; S
  0.477 CPU / 0.464 GPU; seed spread 0.0 CPU / 0.0105 GPU; largest single witness 0.0918; proposed threshold = ceiling
  - (spread + largest witness) = 0.676 CPU / 0.665 GPU; re-audit 0.570 / 0.559. Audit witnesses: CPU 8 pass / 11 fail,
  GPU 4 / 15.
- Corpus scale: reference at 512x384, 256-512 spp took 5-10 min per CPU render; gate runs used 64 spp, reference 1024
  spp, 5-8 independent seeds; after the matrix scanner expansion 586 rows (122 supported, 61 approximated, 403
  dropped-silent); 13 duplicate keys from scanner double-counting.
- Triage: 78 issues -> 21 high / 29 medium / 25 low; 79 -> 10 / 32 / 26; several later snapshots down to 0 high then
  back to 8.
- Viewport: 16-config table (10k and 100k triangles, CPU/CUDA, denoise on/off); reference-engine viewport numbers were
  never measured (a 5 ms/frame figure is an unverified estimate). Unmet targets: 99k pan-frame p99 <= 1.2x
  reference-CUDA; 100k pan frame <= 30 ms with denoise.
- ULP: wavelength array max-ULP 13 vs pinned 4 after switching samplers.

## Dead ends and negative results

- SSIM as the CPU/GPU gate: not usable under independent noise (see above).
- Matrix-only gate scoring: scored 0.000 because the single registered witness failed the frozen predicate (SSIM
  0.846/0.839, dE 6.3/6.6) even though the feature matched; the whole-ROI rectangle metric was at fault. Had it passed
  it would have been worth 0.002.
- Witnesses alone cannot reach a high score (fan-out ceiling 0.062 / 0.127 even if all pass); node-level
  counterfactuals prove the output path, not each input; per-socket controls multiply case count.
- Witness limits: bump has zero effect in v1 (a feature bug, not a test bug); brick control did not neutralise v1
  because the exporter read mortar from a socket name that does not exist on the current Blender node; some curve-card
  controls differ (magenta vs grey); mapping/bump cannot reach full credit with screen-space witnesses when the use is
  in a world tree, a node group or a tiny (237 px) object.
- A first "flat white world" control left the environment-texture node in the scene and the exporter still read it
  (found by node type); those runs were discarded. Controls must be verified to actually neutralise.
- Heavy corpus reskin of every scene rejected; only one scene was reskinned because it fixed a named "flat and boring"
  complaint. Light/shadow linking rejected as required scope; UDIM for image textures deferred; scanner expansion
  belongs in its own work item.
- Blackbody/Wavelength nodes were once over-claimed as spectral rendering; they are colour converters in the
  reference. Corrected.
- Caustic reference: a scaled mesh was invisible to all rays because the intersection discarded scale on a normalised
  direction (t off by ~1/scale); spheres were unaffected. Fixed by scaling t-bounds, guarded by a test. Cycles shadow
  caustics show only on smooth-shaded casters, so both sides must shade smooth when blessing a caustic reference.
- A gate row reading RED turned out in part to be a path separator bug on Windows in a manifest row, and a test-path
  ordering bug that made one row recompute RED; both were tooling defects, not engine defects.
- A parity runner that depended on a third-party image library failed when the library's new major version dropped EXR
  writing; the parity runner became unusable. Avoid dependencies for the core EXR path.

## Design ideas worth reconsidering in the rewrite

- Define the validation oracle per layer: analytic (furnace walls, white furnace, closed-form lighting), published
  OpenPBR reference values, a spectral reference engine for chroma, and an independent high-spp reference render of
  the same engine; avoid "matches engine X" as a pass criterion.
- Use the Welch / Holm tile gate (or a successor) for any two-implementation comparison, with a mask-limited or
  control-differenced variant so a scene-level divergence does not zero unrelated feature credit. Add a relative
  effect floor (effect / signal) and an effect-parity check between engines (would have flagged bump and brick as
  non-equivalent).
- Witness fan-out: a counterfactual control per feature, a control that must verify non-neutrality, and a corpus
  manifest generated from the feature list, not hand-curated.
- World witness (sky rays) and group-instance regions for features not visible on objects.
- Bundle every gate input with provenance sidecars (build digest, seed, spp, engine settings) and let a reducer
  compute status; never hand-set status.
- Mean-vs-noise decomposition as a standard probe: if a ratio is flat in sqrt(spp) it is a deterministic matrix/units
  bug, not MC noise.
- One canonical test-results layout: ignored `_runs` tree plus a curated, tracked tree promoted only by a human; a
  fixed area taxonomy (16 areas such as materials, textures-nodes, lights, world, volumes, caustics, spectral, camera,
  geometry, passes, integrator, viewport, perf, parity, addon, astro); CPU-vs-GPU and engine-vs-reference as sheet
  *columns*, not areas; kebab-case feature names; no batch, lane, date, ticket or model names in paths; provenance in
  a JSON sidecar. Evidence sheet colours by role (reference grey `#52514e`, CPU `#2a78d6`, GPU `#eb6834`, third series
  `#1baf7a`; light surface `#fcfcfb`); gate tolerance as a band; image caps <= 2 MB / <= 1600 px; scene files <= 1 MB;
  blank, near-black or constant images are never evidence. Curated evidence replaced in place; history is the archive.
- Keep a deliberate bless step and hash-pinned reference assets (sha256, licence, source URL in the manifest).
- Triage as a gate: two blind raters, evidence required for downgrades, snapshot reconciled with the live list at
  scoring time, severity rubric as above. Consider whether an issue-count gate is the right shape at all for a rewrite
  with fewer legacy defects.
- Pre-warm, accumulation persistence and a per-frame budget are testable viewport requirements (a presented-generation
  chain for latency capture).

## What no longer applies

- Matching Cycles per-pixel look in scenes where Cycles lacks the physics (dispersion, sun/spot caustics): superseded
  by physics-first.
- Whole parity-corpus scene names, ROI tables pinned to Cycles 5.2 means and the associated tolerance bands (values
  are Cycles-specific).
- The coverage matrix, scanner, audit and op-VM socket classification (translation-layer artefacts of the old add-on).
- Cycles-parity harness protocols (leg runners, view-transform checks) except the "check display transform first"
  lesson.
- Issue-specific triage ratings, the 21/10/0/8 high-count history and the acceptance-manifest snapshot.
- GPU megakernel register tuning and viewport numbers (will be re-measured on the new architecture).
- Readme-showcase tile feedback (hero, HDRI, convergence tile, AOV tile, denoise tile, material sheet) and the old
  FOV-mismatch investigation.

## Open questions

- Does the rewrite keep the Welch tile gate unchanged, and at what N (8 vs 4), margin (1%) and ROI band (5%)? Owner
  call; null calibration beyond 4 vs 4 never done; behaviour with heavy-tailed fireflies unknown.
- Whether a mask-limited / control-differenced Welch for witness ROIs works and what effect floor to use (absolute
  0.01 versus relative).
- Credit granularity for witnesses (one per variant vs fan-out) and eligibility (300 px floor).
- Where to store gate evidence (about 40 MB of arrays in git vs LFS or a release asset) - undecided.
- Whether to run an Astroray-independent oracle in CI (Mitsuba-class spectral engine) and how to handle HDRI sun
  peaks.
- Root causes of the observed CPU/GPU procedural-texture per-pixel difference and the GPU black panel were never
  found; same for the residual multi-bounce CPU/GPU divergence and the shared -1.7% blue deficit. In a
  one-kernel-source architecture these may vanish; verify.
- Clipping of negative colour channels in film output was left as an owner decision.
