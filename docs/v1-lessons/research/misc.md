# Miscellaneous research notes (carried over from Astroray 1.0)

Scope: five unrelated pieces of v1 research: camera motion blur, a
coincident-geometry investigation, a Blender-settings honour audit, flat and
smooth mesh specular-caustic solvers, and the verification of a hermetic
Blender add-on installer. The digests were thin in places; where a note did not
record an outcome, this file says so. Nothing here is a spec for 2.0; it is
evidence and background.

## Established findings

### Camera motion blur (camera-only phase)

- **Interpolate decomposed transforms, never matrix elements.** Linear
  interpolation of matrix entries shears the camera for rotations above about
  5 degrees. Decompose each keyframe matrix into translation, rotation, scale
  (polar decomposition); lerp translation and scale; slerp the rotation
  quaternion. Reason: slerp keeps constant angular velocity and rigid
  rotation; lerped matrices do not stay orthonormal. A 30 degree rotation over
  the shutter interval is the right acceptance case, because a pan hides the
  shear problem.
- **Slerp falls back to lerp when the quaternion dot product exceeds 0.9995**
  (numerical stability near parallel). Polar decomposition converged to a
  matrix-norm difference below 1e-4 within at most 100 iterations in the
  reference.
- **Time is a per-sample random variable in [shutter open, shutter close].**
  Draw it from its own independent low-discrepancy dimension so it does not
  correlate with pixel position or lens samples. The shutter position (starts
  at / centred on / ends at the frame) is a user-visible option; Cycles'
  default shutter length is 0.5 frame.
- **Zero shutter must reproduce the no-motion-blur image bit for bit.** This
  was kept as an explicit gate so the feature cannot silently perturb stills.
- **Blender integration shape:** the exporter evaluates the camera matrix at
  two subframes (pre and post) per frame and passes both; the renderer
  interpolates. The per-ray `time` must reach the camera ray generator.
- **Proposed acceptance gates (A1-A4):** (A1) horizontal pan renders a streak
  whose SSIM against a 2048 spp reference is at least 0.97; (A2) sampled times
  are uniform within 1 % per bin at 1024 spp; (A3) zero shutter is bit
  identical; (A4) a 30 degree rotating camera gives rotationally symmetric
  streaks. The note does not record whether the feature landed or whether the
  gates were met.

### Coincident faces: a "GPU bug" that was the scene

- A glass tumbler showed a dark wedge on GPU but not on CPU. Root cause was
  **ill-posed input geometry, not the engine**: the glass base disk was
  coplanar with the surface it stood on; the lathe profile rim overlapped
  itself with opposite winding; the solid was wound inward (signed volume
  -0.380) and normal recalculation failed to fix it. With inward winding every
  outside hit is classified as an exit (index 1.5 to 1.0); smooth corner
  normals sat about 42 degrees off the facet normal, past the critical angle,
  producing a sharp-edged wedge.
- **Tie-breaking at equal hit distance is implementation defined.** The CPU
  software BVH and the hardware RT traversal pick different coincident
  triangles. A 400k-ray probe found 0 hit/miss mismatches and 2.4 % rays with
  equal t but different primitive. When the glass base wins, the path exits into
  the floor slab and dies. Cycles' own triangle intersection has no tie rule
  (Embree, OptiX and BVH order decide), so no engine is "right".
- **Once geometry is well posed the residual CPU/GPU difference is Monte Carlo
  noise only** (the note's claim; it was checked on an HDRI cell and a
  sun-caustic cell, see numbers).
- **Acceptance-scene authoring rules that came out of it:** keep at least a
  1 mm gap under glass resting on a surface; wind solids outward and assert
  signed volume is positive; no self-overlapping lathe profiles; no duplicated
  profile points. A builder in v1 violated all of these (duplicated profile
  point, base z equal to the plinth top).
- Low bounce caps look like a bug too: with a cap of 2 or 3 about 60 % of
  glass-scene paths terminate on both backends. Do not read a dark glass
  object as a traversal bug before checking the cap.

### Honouring native Blender settings (a Blender-semantics audit)

A matrix of 25 native Blender render settings was rendered A/B (one setting
changed, pinned seed) through the real render operator to linear EXR on both
Blender 5.1 and 5.2 with identical results. Metrics were per-channel mean, max
and variance (not SSIM). Tally: 8 pass, 13 honest fail (setting dropped), 2
need visual review, 2 documented limitations. The knowledge that matters for
2.0 is which settings are easy to drop or misread:

- **Per-type bounce limits** (diffuse, glossy, transmission, volume,
  transparent) were forwarded on one backend and silently reduced to a single
  total depth on the other. Lesson: every option must be threaded through every
  backend, and tested per backend.
- **Wrong property read for the world max-bounces control.** The exporter read
  the world *light-settings* (ambient-occlusion) bounces property, so the
  default of 1024 always won. The real Cycles property is the world Cycles
  settings' `max_bounces`. Blender-semantics trap: similarly named properties
  live in different property groups.
- **Stored-but-never-read settings:** filter glossy, pixel filter type and
  width, and the native reflective/refractive caustics toggles were captured on
  the host side and never consumed by the GPU path. Caustics on GPU were gated by
  a different, photon-based flag, so the native toggles did nothing there.
- **Film transparent left GPU alpha at 1.0.**
- **Denoiser backend selector:** choosing OIDN vs OptiX produced byte-identical
  output, so the selector may not switch backends (flagged minor, unconfirmed).
- **Sample clamp indirect was inconclusive:** no test scene could be built whose
  fireflies classify as indirect (clamp indirect only affects bounces after the
  first non-specular one, so a scene must produce fireflies strictly from
  indirect paths).
- **The Blender "use light tree" flag was not read at final render**; the
  exporter used its own sampler tri-state instead. A real mismatch with native
  semantics.
- **Degradation honesty:** a loud warning listing dropped off-default controls
  was added and tested. This is the model the 2.0 "fail loudly" rule wants.
- **Method note:** this audit was verify-only; none of the findings were fixed
  in the same pass. A pass sentinel printed by the script, not a visual
  impression, was the gate.

### Specular caustics from meshes (flat then smooth)

- **Flat-triangle single-vertex specular solver.** Coplanarity of the path
  vertices is linear in the unknown point `p`: `p . ((x2-x0) x N) = (x0 x2) . N`.
  Intersect with the triangle plane to get a line; the squared form
  `Ci^2 Ro^2 - eta^2 Co^2 Ri^2 = 0` is a degree-4 polynomial in the line
  parameter, matching the degree the paper tabulates for flat-normal
  refraction. Reuse the existing real-root solver; accept roots by residual
  (about 1e-3), and return "no solution" for degenerate cases (zero normal,
  plane parallel to the line, edge parallel to the normal). The rational square
  root fit and Bezout resultant of the paper are not needed for this case.
- **Flat polynomial roots converge only linearly in facet edge** (mean
  error/edge 0.409), so a uniform mesh needs about subdivision 12, roughly
  3.3e8 triangles, to hit a 1e-4 rad oracle gate. Flat shading is therefore
  not a production caustic path; it only finds basins.
- **Smooth (interpolated-normal) triangles do not need the paper's
  degree-inflated polynomial.** Use the flat quartic to locate the basin, then
  Newton-polish on the half-vector residual (manifold exploration form, as in
  Cycles' MNEE). Polish is quadratic in edge length; about 80k triangles reach
  the oracle gate where flat needs about 3e8. The polish needs per-vertex
  normals carried with each caster triangle, a step clamp of +-0.5 (in
  barycentric units), rejection of polished points that leave the facet, and
  about 8 iterations.
- **Gotcha: Snell sign.** A hand-derived "textbook" index inversion gave 2/8
  oracle matches; passing the index of refraction unmodified matched the
  analytic sphere oracle 8/8. Gate any solver port on the analytic oracle, not on
  a hand-derived check.
- **Product decision at the time (owner, 2026-09-04):** the mesh path is
  visual-only and oracle gated; the analytic sphere solver stays the
  research-grade path.

### Hermetic add-on installer (verification discipline)

- A hermetic installer for the Blender add-on (optional allowed-root so tests
  cannot touch a real profile) landed with 62 focused tests, a full CI run of
  2173 passed / 277 skipped / 15 xfailed / 4 xpassed, zero new lint findings,
  and a real-Blender smoke render (mean luminance 0.117186, nonblack fraction
  1.0, 7.4 to 8.3 s). A hash check over 471 live-profile files confirmed nothing
  outside the target changed.
- **Stated limits (keep them honest):** rollback does not cover partial
  deletion of the backup; installs are not crash-atomic; filesystem checks are
  no defence against hostile concurrent substitution.
- **Windows gotchas:** Windows PowerShell 5.1's execution policy blocked the
  first run (fixed with a process-local bypass); native stderr warnings were
  mishandled as errors until handled explicitly. Both PowerShell 7 and 5.1 must
  be exercised.
- A smoke render is a liveness check (renders, not black), never a parity
  claim.

## Sources and reference implementations

| Source | Used for | Notes as recorded |
|---|---|---|
| Shoemake, K., 1985, "Animating Rotation with Quaternion Curves", SIGGRAPH '85, DOI 10.1145/325334.325242 | Quaternion slerp for camera rotation | |
| Cook, R., Porter, T., Carpenter, L., 1984, "Distributed Ray Tracing", SIGGRAPH '84, DOI 10.1145/964965.808590 | Shutter-time sampling | |
| Pharr, Jakob, Humphreys, "Physically Based Rendering", 4th ed., section 2.9 "Animating Transformations" | Transform animation | The note says Mitsuba 0.6 and Cycles use the same decomposition approach |
| PBRT-v4 (Apache-2.0), `util/transform.{h,cpp}`, `AnimatedTransform::Decompose` | Polar decomposition reference | |
| PBRT-v3 `core/quaternion.cpp` (`Slerp`), `core/transform.cpp` | Slerp with lerp fallback at dot > 0.9995 | |
| Cycles `intern/cycles/kernel/geom/triangle_intersect.h` | Confirmed no tie rule for coincident hits | Embree, OptiX and BVH order decide |
| Fan et al., "Specular Polynomials", ACM TOG 43(4) (SIGGRAPH 2024), article 126, DOI 10.1145/3658132, arXiv:2405.13409 | Flat-normal specular polynomial; paper sections 3.2-3.5, 4.1-4.2, 6; Table 2 degrees | Paper text is CC BY 4.0 |
| github.com/mollnn/spoly | Paper authors' code | Unlicensed; deliberately not read or copied |
| Hanika et al., 2015 (full title not given in the note), as implemented in Cycles `mnee.h` | Newton polish on the half-vector residual | Title and venue not recorded in the digest; verify before citing in code |

## Measured numbers worth keeping

Motion blur reference values: polar decomposition tolerance 1e-4 / 100
iterations; slerp-to-lerp threshold dot > 0.9995; Cycles default shutter 0.5.

Coincident-face probe (320x240, bounce cap 12, GPU dead-path fraction):

| Configuration | Dead fraction |
|---|---|
| As authored, hardware traversal | 6.2 % (CPU 0 %) |
| Software traversal | 0 % |
| Base lifted 2 mm / 10 mm | 0 % / 0 % |
| Outward winding | backends agree (diff-mean 0.0036 / 0.0048 vs 0.0116 as authored) |
| Bounce cap 4 / 6 / 12 / 64 on GPU | 8.1 % / 7.0 % / 6.5 % / 6.5 % |
| Bounce cap 2 or 3 | about 60 % on both backends |

- HDRI cell: CPU 0.219 vs GPU 0.131 as authored; 0.157 vs 0.170 once lifted.
- Sun-caustic cell: CPU max 430 vs GPU max 37 (fireflies; clamped at 3 the
  means are 0.057 and 0.056). A max-pixel comparison on a caustic scene is not
  a meaningful gate.
- Equal-t different-primitive rate between traversal back ends: 2.4 % of rays;
  0 hit/miss mismatches in 400k rays.

Honour matrix (RTX 5070 Ti, 2026-08-14): max-bounces luminance mean 0.0344 to
0.4304 (12.5x); clamp-direct luminance max 28.37 to 0.508; exposure ratio
2.000; MC noise at 16 spp 0.00781 vs 64 spp 0.00398 (about 1/sqrt(4), the
expected scaling); same-seed repeat max difference 7e-7; denoising lowers
variance to 0.788x.

Specular solvers: 8 oracle roots, flat solver worst error 5.65e-06 rad against
a 1e-4 gate on its best configuration but only linear convergence (0.409
error/edge); global sweep at subdivision 5 found 448 raw real roots of which 3
survived; smooth polish reaches the gate on all 8 roots (worst 1.45e-05) with
gains from 105x to about 1.9M x; flat reaches the gate on none at 6 subdivision
levels; refraction root 0 with flat finds no in-facet root while polish from
the centroid seed gives 2.22e-06.

Installer verification: see counts above (62 / 2173 / 471 files; 7.4 and 8.3 s
smoke). An outsourced worker produced the change in 452 s / 57,228 tokens;
the coordinator verified rather than trusted.

## Dead ends and negative results

- **Chasing a "GPU bug" before auditing the scene.** Hours went into CPU/GPU
  traversal differences; the cause was coincident, inward-wound geometry. No
  engine change was made. A software traversal switch hid the symptom but is
  not a fix.
- **Matrix-element interpolation for camera motion** (shears); rejected by
  construction in favour of decomposition.
- **Flat-normal solver on a uniform mesh** as a production caustic path: needs
  about 3.3e8 triangles; abandoned in favour of polish.
- **Inverting the IOR from a textbook Snell check** broke the solver (2/8
  matches vs 8/8).
- **Sample-clamp-indirect test scene:** could not be built with indirect-only
  fireflies, so the setting stayed unverified.
- **Reference-bank smoke (2/3 gates passing) during installer work:** cause
  never diagnosed in these notes.
- **PowerShell 5.1 execution policy** as an install blocker (worked around, not
  removed).
- **Mesh solver prototype** lived only in a scratch location and may not have
  survived; the derivation in the note is the durable artefact.

## Design ideas worth reconsidering in the rewrite

- **Geometry validation at import/build time:** reject or warn on inward
  winding (negative signed volume), coincident coplanar faces between
  different materials, and self-overlapping profiles. This turns a baffling
  backend difference into a loud message and fits the "fail loudly" rule. A
  closed-solid check for glass is the highest-value piece.
- **Deterministic coincident-hit tie-break** in the shared traversal (for
  example by material priority or primitive id) so CPU and GPU agree by
  construction; with one kernel source this is a design choice rather than an
  accident. Blender itself has no such rule, so decide deliberately.
- **A settings-honour matrix as a permanent test:** every Blender property the
  add-on exports gets an A/B test plus an explicit unsupported-control warning.
  Generate the rows from the single settings mapping so the matrix cannot drift
  from the exporter.
- **Time as a first-class ray attribute** from the start (camera now, object
  and deformation motion later), with decomposed-transform interpolation and a
  dedicated sampler dimension.
- **Caustic solver architecture:** analytic or polynomial solve to find basins,
  Newton polish on the shading-normal residual, oracle-gated against an analytic
  sphere. This composes with manifold-style caustics (Hanika) and is compatible
  with a single kernel source if it avoids dynamic allocation.
- **Hermetic installers/test harness:** an `allowed_root` style parameter so
  tests never touch a real Blender profile; test under both PowerShell
  versions.

## What no longer applies

- Camera integration points specific to v1's ray generation and add-on file.
- Backend-specific plumbing gaps (per-type bounces dropped on one backend,
  stored-but-unread settings): consequences of two separate code paths; 2.0's
  single kernel removes the class, though the Blender-semantics findings remain.
- The traversal-selection environment switch and the OptiX-vs-software
  default: v1 infrastructure.
- Mesh solver wiring into v1's caustic triangle struct and manifold chain.
- Pixel-parity framing of the honour matrix versus Cycles; only the semantics
  survive.
- The installer's script and its PowerShell wrapper as such (2.0 will have its
  own packaging); only the verification lessons survive.
- Delegation-cost anecdote for the installer worker: process detail.

## Open questions

- Did camera motion blur land in v1 and did gates A1-A4 pass? Not recorded.
- Hanika et al. 2015: full title and venue are missing from the digest.
- Should coincident-hit resolution be a rule in 2.0 (and what order), or should
  validation simply forbid such scenes?
- Does a denoiser backend selector actually switch backends in Blender 5.x
  Cycles semantics? v1 saw identical output (unconfirmed).
- How to build a scene whose fireflies are classified strictly as indirect, to
  verify clamp-indirect behaviour.
- Honour-matrix metrics used means and maxima, which are weak; what image
  metric is appropriate when there is no reference renderer to compare to?
- Whether the mesh caustic polish path is worth building at all in 2.0 given
  it was judged visual-only in v1.
- The reference-bank smoke failure (2/3 gates) during installer work was never
  diagnosed.
