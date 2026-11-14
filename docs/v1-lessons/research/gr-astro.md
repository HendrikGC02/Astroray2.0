# General relativity and astrophysics: Kerr metric, geodesic tracing, accretion emission, atomic lines, data I/O for science

Carried over from Astroray v1. Written for readers who have never seen v1's code. Everything here is research
output or a measurement; where the v1 notes were thin it says so. v1 never validated absolute radiometry
(photon counts, Jy) for any astrophysical scene; only relative checks against GYOTO and analytic formulas.

## Established findings

### Coordinates and the Kerr metric
- Boyer-Lindquist (BL) coordinates were chosen first: analytic Christoffel symbols, same choice as RAPTOR and
  ipole, easy ISCO / photon-sphere / horizon formulas. Kerr-Schild was deferred "to the GPU port".
- BL is singular at sin(theta) = 0. In v1 the adaptive RK45 integrator stalled at its step floor (h = 0.001)
  for rays crossing the polar axis; those rays were declared captured and produced a dotted dark column at
  image x = 0 (101 of 101 rows captured where about 25-27 were expected).
- Fix that worked: an atlas. Use BL everywhere except Cartesian Kerr-Schild while |sin theta| < 0.1, switching
  back to BL above 0.2 (hysteresis so a ray does not flip-flop). Cap the Kerr-Schild step at 0.1 r so an
  equatorial disk crossing is not stepped over. Termination (capture/escape) tests stay in BL. Method follows
  GRay2 (Chan et al. 2018); Kerr-Schild form per Visser 2007. No code was copied.
- The Kerr-Schild covector map needs the correct sign convention. Verified by J g_BL^-1 J^T = eta - f l l holding
  to 4e-10 at a = 0.94; the seven other sign combinations failed at 1e-2 to 0.5. Cross-check: near-axis
  DOP853 at rtol 1e-12, BL-only vs BL-KS-BL end states agree to 3e-8 (a = 0) and 8e-8 (a = 0.94).
- Null-geodesic integration must start from the correct root for p_t. A camera ray is past-directed (traced
  backward from the observer); taking the future-directed root mirrored the shadow (34/88 px instead of
  GYOTO's 87/34 px).
- Capture threshold: v1 first used r < 2.5 M ("relaxed from 2.02 M to avoid BL coordinate stalling") and later
  replaced it by r < (r_+ + min r_photon)/2, which equals 2.5 M at a = 0. The earlier r_+ + 0.5 M clipped the
  prograde shadow edge in Kerr. The threshold "must not drift" without re-validating against a Kerr reference.
- Precision: double precision is required for the integrator near the horizon (float failed in the original
  Python prototype). Float is fine outside the integrator.
- Integrator: Dormand-Prince RK4(5), step factor 0.9 err^-0.2 clamped to [0.2, 5], |h| clamped to [0.001, 50]
  (integrating backward). A disk crossing is detected as a sign change of theta - pi/2 and interpolated.
- Shadow verification (512^2 image, camera inclination 90 deg, a = 0.94) against GYOTO 2.0.2: left/right/top/
  bottom radii 89/33/62.5/61.5 px vs GYOTO 87/34/61.5/61.5 (max error 2.9%). Polar shadow radius
  sqrt(eta + a^2) = 4.884 M corresponds to 62.9 px. Analytic equatorial critical impact parameters at a = 0.94:
  b_c = 6.90 M (one side) and 2.64 M (other).
- Shadow size at a distant camera: when the exterior is treated as flat, a camera "scaled" to an effective
  distance gives a capture radius ratio R/D = 5/12 = 0.417 for D_eff = D r_obs/(R M) (captured if b < 3 sqrt(3) M).
  Measured ratios were 0.355-0.386. The capture radius measured at three observer distances (67.07, 33.44,
  16.74 px) matched the closed form within 0.3%. The earlier "unexplained 0.37x textbook size" was a
  camera-scaling effect, not a metric bug. Scaling of shadow with r_obs was verified (ratios 0.483, 0.475 vs 0.5).

### Redshift and invariant radiative transfer
- Thin disk, momentum-based redshift (replaces inclination-based): g = 1 / (u^t (1 - Omega lambda)), with
  lambda = -k_phi / k_t, Omega = sqrt(M) / (r^1.5 + a sqrt(M)),
  u^t = (r^1.5 + a sqrt M) / (r^0.75 sqrt(r^1.5 - 3 M r^0.5 + 2 a sqrt M)). The first v1 formula used
  sin(i) sin(phi) from the declared camera inclination and ignored photon momentum, so at i = 90 deg g was
  azimuthally uniform: Doppler asymmetry about 0 vs GYOTO +0.203 (about 100% wrong). After the fix
  (512^2, i = 90): a = 0 delta +0.2131 vs GYOTO +0.2156 (1.2%); a = 0.94 +0.2021 vs +0.2029 (0.4%). Disk-only
  band R/L flux ratio at a = 0.94 went from 0.998 to 2.38. Absolute side medians remained about 2% below GYOTO.
- Convention: g = nu_obs / nu_emit, lambda_obs = lambda_emit / g. Killing energy E = -k_t is conserved; per-step
  shift g_step = (-k_t_step)/(-k_t_start). Observer-at-infinity redshift for a static point is g = sqrt(-g_tt).
- Thin-disk invariant transfer: observed I_lambda = g^5 B_lambda(g lambda)-form (equivalently I_nu / nu^3 is
  invariant). Verified in the 1% regime (see Measured numbers). The old clamp g <= 10 (later 20) had no
  physical basis and was removed.
- Volumetric emitters (jets, ADAF, plasma): work in the fluid frame. nu_fluid = -k.u. Evaluate emissivity j_nu and
  absorption alpha_nu there, then transport the invariant quantity per affine parameter:
  dI_inv/dlambda = J - A I_inv, with J = j_nu / nu^2, A = nu alpha_nu, I_inv = I_nu / nu^3.
  Per piecewise-uniform segment: I_f = I_i exp(-dtau) + (J/A)(1 - exp(-dtau)) with a Taylor branch for small
  dtau. Front-to-back marching T *= exp(-dtau_seg) is exact for piecewise-uniform media. Reason: redshift
  and beaming are produced automatically by the invariant form.
- Do not apply an explicit Doppler D^3 (or g) factor on top of invariant transport. The old jet code multiplied
  D^3 by the lab-frame path length, a double count that produced an extra power of D (measured D^4 vs D^3).
  Correct tests: ratio observed/rest-frame equals g^3/D^3 within 1%. For steady-jet / blob closed forms see
  Lind & Blandford 1985 (D^(2+alpha) steady, D^(3+alpha) blob).
- Deliberately not taken by v1: bending of the volumetric chord, gravitational redshift of volumetric emitters,
  orbital/inflow ADAF kinematics (ADAF fluid treated as static, g = 1 from the fluid), polarisation, Faraday.

### Accretion emission models
- Thin disk: Page-Thorne flux integrated from the ISCO; T = (F/sigma)^(1/4). v1's first disk used Schwarzschild
  E, L, Omega, with ISCO 6 M, so it went dark for Kerr ISCO < 3 M; for Kerr spin the disk needs the Page-Thorne
  / Bardeen-Press-Teukolsky (BPT 1972 eq. 2.12-2.13) expressions. v1 never completed this (flux, T(r) and r_in
  stayed at the a = 0 values).
- Slim disk: no closed form exists (Sadowski 2009 is tabulated/numerical). v1 used an analytic fit:
  Novikov-Thorne times an advective correction. A linear f_adv clamp killed emission at high accretion rate;
  replaced by f_adv = 1 - exp(-(mdot/mdot_Edd)(r_ISCO/r)^2), which gives T about 0.33 T_NT at mdot = 10,
  r = 1.5 r_ISCO. Acceptance: T_slim/T_NT in [0.95, 1] at mdot 0.1, below 0.5 at mdot 10.
- ADAF (Narayan & Yi self-similar, Yuan & Narayan 2014 review): static fluid, thermal synchrotron. Sgr A*-like
  parameter checks are in Measured numbers.
- Synchrotron: thermal (relativistic Maxwell-Juttner) emissivity/absorption using the Pandya et al. 2016 fit
  formulas (eqs 29-34) plus a power-law fit set for non-thermal electrons; polarised variants (Pandya 2018)
  deferred.
- Jet: relativistic Doppler boosting is an emergent result of the invariant transport; for a gamma = 10 head-on
  geometry the approaching/counter-jet brightness ratio would be about 1e5 / 1e-8 (design-note estimate).
- HII regions (design-level, not built in v1 as far as the notes show): H alpha 656.3, H beta 486.1,
  [OIII] 500.7 / 495.9, [NII] 658.4 / 654.8 nm via a CLOUDY-derived lookup (pyCloudy) over several parameters;
  acceptance: line ratios within 10% of CLOUDY.
- Roche-lobe geometry for an HMXB would use Eggleton 1983; the notes say nothing in v1 implemented it.

### Hybrid propagation: black hole inside an ordinary scene
- Architecture used: ordinary path tracing until a ray enters a black hole's influence sphere; convert to BL,
  integrate the geodesic in double, collect disk crossings, then capture or exit as a remapped direction with a
  frequency shift. The black hole is a direction remapper. NEE through the hole is disabled (the ray is treated
  as specular); geodesics are not BVH-accelerated; disk importance sampling was rejected because the geometry is
  curved. Inclination derives from the camera-to-hole direction against the spin axis, not a render setting.
- Scene intersection inside the influence region: v1 originally never tested scene geometry during the geodesic
  march, so objects inside the influence radius were invisible. Fix: each accepted RK step becomes a world-space
  chord; the scene BVH is queried on the chord; the first real surface ends the march (Groeller 1995 polyline
  approximation, first hit wins). Chord sag error is about R dphi^2/8. Limits: straight shadow rays ignore bending;
  the volumetric march must also be truncated at the hit depth (found later).
- Colour/redshift at the influence boundary: owner decision used observer-at-infinity g = sqrt(-g_tt) (Cunningham
  1975), accepting about a 23% brightness step at the sphere boundary (observer at 20 M). A "local reference"
  option was rejected. Test: g^4 = (1 - 2M/r)^2 within 5%.
- Metric dispatch in a general tracer: option chosen was a virtual "is the space flat" check with a single call
  site, guarded so flat-space renders stay within 5% of baseline time and SSIM >= 0.999. CRTP only if that
  budget breaks. In the end v1 did not do per-step metric advance in the path tracer; it dispatched per
  black-hole object, running one full geodesic per segment. No scene-level metric existed. Reason to keep: simple,
  but see Dead ends (respawn artefact) and Design ideas.

### Spectral light sources and atomic lines
- Sampling narrow lines on a coarse grid: with a 5 nm grid, the sodium D lines (588.995 nm, relative intensity 2;
  589.592 nm, intensity 1) fall between bins; a zero-bin deposit gave a degenerate light-sampling CDF and a
  vapour lamp that emitted nothing at the hero wavelength.
- Fix: energy-normalised Gaussian line shapes, FWHM 15 nm (3 x grid, sigma about 6.37 nm), unit area so D2:D1 = 2:1
  is preserved. At 580 nm (9.2 nm from 589.2) the SPD reads about 0.35 of peak instead of 0. Honest caveat: 15 nm
  is about 150x the physical 0.1 nm linewidth; it is a presentation / grid choice (any FWHM in 10-15 nm was
  considered acceptable). Full Voigt (Armstrong 1967) is irrelevant at 5 nm sampling; only a Gaussian limit was
  needed (reference: SciPy `voigt_profile`, BSD-3, not a runtime dependency).
- Mercury: the stored SPD integrated to a magenta chromaticity (0.314, 0.311), below the spectral locus. The v1 notes do not
  record the exact old line list. Corrected from NIST persistent Hg I
  lines: 404.66 (relative 200), 435.83 (300), 546.07 (400, strongest), 576.96 (160), 579.07 (200); no continuum
  (a clear lamp is a line source). Result (0.335, 0.369) greenish-white, target (0.33, 0.38), delta < 0.02,
  evaluated with CIE 1964 10 deg observer functions. Sodium chromaticity (0.583, 0.417) unchanged.
- Earlier superseded Hg line set (kept for context): 404.66 (400), 435.83 (1000), 546.07 (500) with 5% phosphor
  continuum; the 578.0 nm line is not in NIST's persistent lines (a 577/579 nm yellow doublet exists).
- Seven commonly requested illuminants (CIE F2, F3, LED-B3/B4/B5, Na, Hg): CIE data from colour-science
  (BSD-3); atomic lines from NIST ASD; normalise to peak 1.0; zero outside 380-780 nm. Whether a "light_source"
  category slot was free in the profile database was left open.

### Data I/O for science (design-level; v1 notes are design, not measurement)
- Common intermediate: a dense 3D grid (flat C-order, nx/ny/nz, bounding box, field name) feeding a volume.
- Formats: FITS via cfitsio (public domain, NASA; 4.6.4 as of April 2026); HDF5 with HighFive (MIT, header-only);
  NumPy .npy via an in-house parser of about 80 lines (accept only '<f4'/'<f8', fortran_order False, 3D);
  .npz handled in Python only. yt's chunked-read pattern (BSD-3) as the model for large AMR data.
- SPH to grid: cubic B-spline kernel (Monaghan 1992; Price 2012) with footprint splatting (Westover 1990) and
  Shepard normalisation; for particle-level rendering Wendland C2/C4 kernels are recommended (Dehnen & Aly 2012).
  Cost O(N (2h/dx)^3). Tests use synthetic data created at runtime; uniform SPH: max/min < 1.01 (boundary < 1.05).
- A later audit judged the bespoke grid route superseded by OpenVDB/NanoVDB volumes (owner decision, 2026-09-13);
  the earlier data-I/O notes had no NanoVDB awareness. Treat the .npy/FITS/HDF5 loaders as adapters into a
  sparse-volume representation.

## Sources and reference implementations

| Topic | Source (authors, year) | Used for |
|---|---|---|
| Kerr geodesics | Bardeen, Press & Teukolsky 1972, ApJ 178, 347, DOI 10.1086/151796 | ISCO, photon sphere, Omega_H, disk E/L (eq 2.12, 2.13, 2.16) |
| Kerr geodesics | Carter 1968, Phys. Rev. 174, 1559 (sec 6); Chandrasekhar 1983, ch. 7 sec 62-63 | Constants of motion, null-geodesic RHS |
| Spin limit | Thorne 1974 | a <= 0.998 M |
| Axis chart | Chan, Medeiros, Ozel & Psaltis 2018, GRay2, ApJ 867, 59, DOI 10.3847/1538-4357/aadfe5, arXiv:1706.07062 | BL / Cartesian Kerr-Schild atlas |
| Kerr-Schild | Visser 2007, arXiv:0706.0622 | KS form |
| Redshift | Cunningham 1975, ApJ 202, 788, DOI 10.1086/154033 | g for disk emitters |
| Transfer | Rybicki & Lightman 1979 (ISBN 0-471-82759-2), sec 1.4-1.5, 4.9, ch. 6; Lindquist 1966, Ann. Phys. 37, 487 | Invariant I/nu^3, synchrotron |
| Transfer | Younsi, Wu & Fuerst 2012, A&A 545, A13, eqs 29-31 | GR transfer |
| Jet beaming | Lind & Blandford 1985, ApJ 295, 358 | D^(2+alpha) / D^(3+alpha) |
| GR codes | Vincent et al. 2011 (GYOTO); Bronzwaer et al. 2018, A&A 613, A2 (RAPTOR I) and 2020, A&A 641, A151 (RAPTOR II); Moscibrodzka & Gammie 2018, MNRAS 475, 43, arXiv:1712.03057 (ipole) | Method cross-checks |
| Synchrotron fits | Pandya, Zhang, Chandra & Gammie 2016, ApJ 822, 34, arXiv:1602.08749, DOI 10.3847/0004-637X/822/1/34 (eqs 29-34) | Thermal and power-law fits |
| Synchrotron (pol.) | Pandya et al. 2018, ApJ 868, 13, arXiv:1810.05646 | Deferred |
| Slim / thin disk | Sadowski 2009, ApJS 183, 171, arXiv:0906.0355; Abramowicz et al. 1988, ApJ 332, 646; Abramowicz & Fragile 2013, LRR 16, 1, arXiv:1104.5499; Page & Thorne 1974, ApJ 191, 499; Novikov & Thorne 1973 | Disk models |
| ADAF | Narayan & Yi 1994 and 1995 (ApJ 452, 710); Yuan, Quataert & Narayan 2003, ApJ 598, 301; Yuan & Narayan 2014, ARA&A 52, 529, arXiv:1401.0586 | ADAF |
| Photon ring | Cardenas-Avendano 2022 | Design reference |
| Binary geometry | Eggleton 1983 | Roche lobe (unused) |
| Scene intersection | Groeller 1995, The Visual Computer 11(5), 263-274; Weiskopf et al. 2004 (cited, not verified) | Polyline ray bending vs scene |
| Spectral sampling | PBRT-v4 (Apache-2.0) hero wavelength | Wavelength sampling |
| Atomic lines | Armstrong 1967, JQSRT 7(1), 61-88, DOI 10.1016/0022-4073(67)90057-X; Voigt 1912; AstroBaki line profiles; NIST ASD and NIST Handbook of Basic Atomic Spectroscopic Data (mercury table 4), public domain | Line shapes and intensities |
| Illuminants | CIE 15:2018 Table 10; CIE 224:2017; colour-science v0.3.9/0.3.15 (BSD-3); CIE DS DOI 10.25039/CIE.DS.dhcw57sd | F2/F3/LED SPDs |
| SPH | Monaghan 1992, Annu. Rev. A&A 30, 543, DOI 10.1146/annurev.aa.30.090192.002551; Price 2012, arXiv:1012.1885; Westover 1990, SIGGRAPH, DOI 10.1145/97879.97919; Dehnen & Aly 2012, arXiv:1204.2471; Shepard 1968 | SPH to volume |

Note on Pandya 2016: a task brief once cited DOI 10.3847/0004-637X/823/2/100; the ADS record is 822/1/34. Verify.

External-engine files read, exactly as recorded:
- ipole (BSD-3, commit 7f7a482c): src/radiation.c (Bnu_inv, get_fluid_nu), src/geodesics.c (second-order
  symplectic push_photon), src/geometry.c, model_radiation.c (jar_calc_dist: jI = j/nu^2, aI = alpha nu),
  ipolarray.c (approximate_solve), src/symphony/maxwell_juettner_fits.c and power_law_fits.c (Pandya fits).
  No code copied; pattern only. ipole has no slim-disk file (checked against master 2024).
- RAPTOR (GPLv3, pinned 08cb9a2b): metric.c, numbers-only cross-check, no code shape mirrored.
- GYOTO 2.0.2 (CeCILL, pin 94863d06a36958e65074c632231c12f943b8d868): PolishDoughnut.C, ThinDisk.C, numerical
  cross-check only. A GPL-3 note in other places: treat as reference-only.
- GRay2 (GPL-3): published method only, no code. symphony standalone (GPLv3): not used. yt (BSD),
  CLOUDY (GPL, preprocessing only), CFITSIO, EleFits (LGPL-3), HighFive (BSD-3 per one note, MIT per another),
  WebbPSF/STPSF (BSD): tooling candidates.
- Cycles kernel path code (branch model) was consulted as a structural comparison for the tracer.

## Measured numbers worth keeping

Kerr/Schwarzschild analytic test values:
- ISCO: a = 0 -> 6 M; a = 0.998 prograde 1.237 M, retrograde 8.995 M.
- Photon sphere a = 0.998: 1.073 M and 3.998 M. Horizon r_+ = 1.0632 M. Omega_H(0.998) = 0.4694 / M.
- Polar shadow radius sqrt(eta + a^2) = 4.884 M (a = 0.94).

Thin-disk transfer check (source g = 0.8205396856615398 from GYOTO): max spectral relative residual 2.4158e-5;
max bin residual 3.3141e-3; bolometric error 8.28e-5; control g^4 error 8.95e-5. All under a frozen 1% limit.

Kerr vs GYOTO 2.0.2 before fixes (a = 0.94, i = 90): prograde photon ring mismatch 94% (34 px vs 66 px), mean
radius +9.0%. Against GYOTO at a = 0 the ring agreed within 1.5%. After fixes see Established findings (shadow
within 2.9%, disk asymmetry within 0.4%-1.2%). (Cause of the initial failure was that spin was never
passed to the metric.)

Synchrotron checks: Sgr A*-like B = 30 G, T_e = 5e10 K gives Theta_e = 8.46, nu_c = 8.4e7 Hz, X at 230 GHz about
172. Analytic J_S(X = 1, pi/2) = 0.5023; j_nu^I = 1.083e-23 erg / (s cm^3 Hz sr) at B = 1 G, n_e = 1.

Slim disk: H/r at ISCO = 0.75 for mdot = 1; 0.0149 at 10 r_ISCO. At r = 9 M, mdot = 0.1, T_NT about 9.1e7 K.
Landed implementation matched Page-Thorne 1974 + Sadowski 2009 at r = 9 M, mdot = 1: T = 7.45e6 K.

ADAF (Sgr A*-like): at 10 R_s n_e about 9.94 cm^-3, B about 2.46e-5 G, T_i 1.2e11 K, T_e 1e9 K; at 2 R_s
n_e = 6.86e4, B = 1.45e-4 G. Scene targets: shadow about 52 microarcsec, 30-50% frame-drag asymmetry,
about 3 Jy at 230 GHz (acceptance targets, not verified radiometry).

Atomic/illuminant: Na D2 588.995, D1 589.592 nm. Hg persistent lines above. Gaussian sigma 6.37 nm at FWHM 15 nm.
Hg chromaticity (0.335, 0.369); Na (0.583, 0.417).

Performance: original Python prototype 160x120 render in 1.6 s on CPU. Estimated (not measured) GR cost per
ray 0.1-1 ms; 5-30 s for the GR part at 1080p 4 spp. A 15.5x render-time growth in a regression suite came from a
scene redefinition (resolution 256^2 -> 512^2, samples 16 -> 64, max depth 4 -> 5), only +4.8% to +6.7% from code.
Dark-ring artefact (below) covered 8-10% of a 74-78 px annulus.

Regression metrics: per-channel ratios within about 1.5% of a reference bank; SSIM was used only as a
diagnostic (Kerr 0.934, Schwarzschild 0.850) and nothing was re-blessed.

## Dead ends and negative results

- Float precision in the integrator: fails near the horizon.
- Capture radius 2.02 M: abandoned (BL coordinate stalling). Capture r_+ + 0.5 M: clipped prograde Kerr shadow edge.
- Literal future-directed p_t root for camera rays: mirrored shadow.
- Seven wrong Kerr-Schild covector sign conventions: fail at 1e-2 to 0.5.
- Inclination-based disk redshift: gives azimuthally uniform g at i = 90 and ~100% Doppler-asymmetry error.
- Explicit D^3 on top of invariant transport: double counts (D^4 measured).
- Linear f_adv clamp for the slim disk: zeroes emission at high accretion rate.
- g clamps of 10 and 20: no physical basis; removed.
- Respawning the path-traced continuation from the *entry* point of the influence sphere rather than the exit
  point: rays ping-pong across the sphere, producing an 8-10% dark mask in a partial ring (74-78 px at 512^2),
  and with max depth 5 they end black. The cure needs the geodesic result to return the exit point as well as the
  direction (three dispatch sites needed changing in v1). Not fixed during the research window.
- Disk model hard-wired to Schwarzschild E, L, flux while the metric was Kerr: disk goes dark for ISCO < 3 M.
- Slim-disk closed form from Sadowski: does not exist; tabulated path deferred.
- Hg 578.0 nm line from a spec: not a NIST persistent line.
- Zero-bin deposit of narrow lines on a 5 nm grid: degenerate sampling CDF.
- A flat continuum under Hg: removed in the corrected version (a clear lamp is a line source); the old stored SPD with
  its mix of lines integrated below the locus.
- Full Voigt profiles: no value at 5 nm sampling.
- Emission-line importance sampling: deferred, not tried.
- Fix of 15.5x render slowdown by "optimising code": not a code regression; do not chase.
- Forking the tracer into a GR-specific copy was rejected for drift risk.
- An earlier audit worried that the whole Kerr/BH path might no longer render after six months with no
  evidence; recommended a CPU regression of Schwarzschild/Kerr/ADAF scenes before any new astrophysics work.

## Design ideas worth reconsidering in the rewrite

- Treat each phenomenon (metric, emitter, volume source, telescope) as a plugin behind a stable interface;
  a metric registry with one virtual `isFlat`/metric evaluator avoids forking the tracer. v1 realised this only per
  black-hole object; consider a scene-level metric (still unresolved there).
- Integrator interface: double precision, GPU-portable fixed-size array (e.g. `double[4]`) rather than heap vectors;
  keep a hook for the Carter constant (a precision-boundary check was an open item).
- Atlas switch (BL + Kerr-Schild near the axis, with hysteresis) was the clean fix for the axis singularity; build
  it in from the start rather than as a patch. Consider Kerr-Schild throughout (GPU-friendly) and compare cost.
- Single invariant transfer law I/nu^3, dI/dlambda = J - A I, for every emitter (thin disk, ADAF, jet, nebula),
  with one fluid-frame emissivity/absorption interface; no emitter ever applies D or g itself.
- Return the full geodesic result (exit position, exit direction, accumulated redshift, optical depth) so the
  host tracer continues from the exit point without ping-pong.
- Per-step scene intersection with chord segments for geometry inside the strong-field region; remember volumetric
  segments also end at the first hit. Estimate chord sag R dphi^2/8 to choose the step cap.
- Spectral line sources: store line positions and relative strengths analytically (not on the 5 nm grid) and sample
  the line explicitly; use a reduced-width Gaussian only as a visual fallback. Document the real linewidth.
- Hero-wavelength spectral pipeline with double-precision wavelength shift: `lambda *= g` per emission, with
  emission at the observed hero wavelength. v1's GR note left open how wavelengths outside the 380-780 nm CIE table
  are handled; the rewrite should state that rule.
- Page-Thorne flux for Kerr from BPT 1972 closed forms (a pre-requisite for a believable Kerr disk), evaluated in
  tables with a spin-dependent r_in.
- Data I/O: do not rebuild FITS/HDF5/NPY loaders into bespoke dense grids; load into the volume representation the
  engine uses (OpenVDB/NanoVDB, as v1 later decided). SPH: Wendland C4 over cubic spline.
- Test first against analytic anchors listed above (ISCO table, photon sphere, shadow radii, g^3/D^3, g^4) and GYOTO
  pinned versions for relative checks. Keep the SSIM style metric diagnostic only.
- Measurement discipline that worked: pinned external reference versions, tolerance tables (1% transfer, 2.9% shadow),
  and frozen limits stated before measuring.

## What no longer applies

- Page-Thorne rendering in the original Python prototype (single-ray RK45 in NumPy-free Python, OpenMP per pixel): v1 code structure.
- Cycles-driven GR respawn logic and light-sampler CDF repair for the sodium lamp: tied to v1's tracer.
- Reference-bank SSIM re-blessing, and the "scene re-author timing" regression analysis: v1 test management.
- Blender-addon property group / operator wiring for black holes: v1 integration code.
- Package-by-package pause/thaw decisions on the Pillar 4 audit (e.g. which loader to merge into which): v1 planning only.
- DensityGrid / SimulationVolume loader specifics: superseded by the OpenVDB route in v1 itself.
- Cycles `kernel_path.h` branch model comparison: structure of one engine, not physics.

## Open questions

- Absolute radiometry: no v1 scene was validated for photon counts or Jy; the 3 Jy / 52 microarcsec targets are
  acceptance numbers only.
- Kerr thin-disk flux, temperature and inner radius (Page-Thorne with spin) were not finished.
- Causal arrival-time delay along geodesics, and a matched Kerr image against GYOTO beyond shadow radii.
- Whether one metric per scene is sufficient (binaries, moving holes).
- Precision boundary for double vs float, GPU port of the integrator, and use of Cartesian Kerr-Schild near the axis.
- ADAF / synchrotron emitters in orbital or infalling kinematics (v1 used a static fluid, g = 1).
- Polarisation (Faraday rotation, Pandya 2018) and non-thermal electrons: left out entirely.
- HMXB, diffraction-grating BSDF, cluster/NFW lensing, telescope PSF (WebbPSF/STPSF): named in the design, not built.
- How samples outside 380-780 nm are treated when converting to colour (unspecified in the old design).
- Whether Windows/MinGW can build cfitsio / HighFive / OpenVDB (unverified in v1 notes).
- Which FWHM to use for rendered line sources, since real lines are about 0.1 nm wide.
