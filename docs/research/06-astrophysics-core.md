# Astrophysics core: what the 2.0 core must leave room for

Workstream: astrophysics-core. Date: 2026-11-14. Tags: VERIFIED = I read the primary
text (PDF text-extracted or page fetched); REPORTED = secondary source or search
snippet; DERIVED = my own arithmetic or reasoning from verified facts; UNVERIFIED.

## Question

How must the core be shaped so that astrophysical rendering (black holes, thin disks and
HMXBs, emission nebulae, non-visible bands, science output) fits natively later, without
retrofitting? Specifically: a spacetime interface with flat space as the trivial case;
geodesic integration and its GPU form; coexistence of curved rays with a triangle BVH;
redshift and invariants; covariant transfer; emission data; science output; validation
numbers; and the minimal hooks the first milestones must leave.

## Findings

### F1. What the existing GR codes do (all read from the papers)

| Code | Hardware / source | Coordinates | Integrator and step control | Licence |
|---|---|---|---|---|
| Odyssey (Pu 2016) | CUDA, double | Boyer-Lindquist, sin(theta) clamped near the pole | RK5 adaptive (Press); "exceeds 1 ns per photon per RK step" | GPL-3 |
| GRay2 (Chan 2018) | OpenCL: CPU and GPU, one source | Cartesian Kerr-Schild (KS) | classic RK4, fixed step in affine parameter; 4th-order convergence shown | not checked |
| RAPTOR (Bronzwaer 2018) | C; one source, OpenMP or OpenACC build | KS, MKS | RK4, step from Noble 2007 / Dolence 2009 formula; Verlet option | GPL-3 |
| ipole (Moscibrodzka 2018) | C + OpenMP; CUDA port 2023 | any; says "coordinate and spacetime independent" | analytic transfer substeps | BSD-3 |
| Blacklight (White 2022) | C++, CPU | Cartesian KS only | Dormand-Prince RK5(4)7M recommended; RK2 and RK4 offered; step subdivision and dense output for transfer | public domain |
| GYOTO 1.0 / 2.0 | C++ plugins, CPU | Boyer-Lindquist Hamiltonian form; arbitrary and numerical 3+1 metrics | RK4 adaptive | GPL-3+ (REPORTED) |
| KPolaris (Zhang & Chen, 22 Sep 2026) | Kokkos (CUDA, OpenMP, serial), double | BL, Cartesian KS, spherical KS, MKS, FMKS | RK4 with step doubling, tolerance 1e-10 adopted | BSD-3 |
| DNGR (James 2015) | C++ CPU render farm | BL, super-Hamiltonian | custom Runge-Kutta-Fehlberg, adaptive | in-house |
| Coport (Huang 2024) | Julia | any spacetime | solves coupled geodesic + polarised transfer directly | not checked |

Sources (VERIFIED unless marked): arXiv 1601.02063 (Odyssey; GPL-3, github.com/hungyipu/Odyssey),
1706.07062 (GRay2), 1801.10452 (RAPTOR; GPL-3 github.com/tbronzwaer/raptor), 1712.03057 (ipole;
BSD-3 github.com/AFD-Illinois/ipole), 2203.15963 (Blacklight), 1109.4769 and 2311.18802 (GYOTO;
licence from gyoto.obspm.fr snippet, REPORTED), 2609.26298 (KPolaris; BSD-3 per paper text),
1502.03808 (DNGR), 2407.10431 (Coport). The KPolaris id in the task is real (22 Sep 2026); the
recon had seen only its title, it is now read.

### F2. Single-source CPU+GPU for GR is established practice

- RAPTOR is "hardware-agnostic", one C code compiled for OpenMP or OpenACC; 10 CPU cores
  gave 10,707 geodesics/s and one GTX 1080 gave 104,900 geodesics/s; a 2000x2000 image
  took 3 min 58 s on one CPU vs 39 s on the GPU (VERIFIED, 1801.10452 appendix B).
- KPolaris compares serial, OpenMP and CUDA builds of "the same double-precision
  implementation" (Kokkos). GPU vs one CPU thread: ~10x to 182x. GPU vs all CPU threads is
  "smaller, workload-dependent"; "GPU execution is faster in eight of the nine cases", and
  a server CPU beat its paired GPU on the AthenaK case. Complete 256^2 fast-light images
  took 2.13-5.09 s on an RTX 5090 workstation (VERIFIED, 2609.26298 section 5.1; the table
  layout in the PDF text is garbled, so I quote only the prose).
- GRay2 runs one OpenCL source on CPUs, GPUs and Xeon Phi (VERIFIED).
- Consequence: the owner's "one kernel source" decision has direct precedent in this field.

### F3. Coordinates and the Hamiltonian form

- Boyer-Lindquist has pole and horizon singularities; GRay2 moves to Cartesian KS, which is
  horizon-penetrating, has no pole, and needs no special-case branches (bad for SIMD).
  Its cost: KS geodesics need ~65% more flops than BL; measured KS single-precision was
  usually <70% slower than BL, but in double KS was up to ~40% faster on most GPUs because
  BL needs trig in double (VERIFIED, 1706.07062 sections 2.1 and 4).
- GRay2 also found it cheaper to write the equations through the metric-derivative tensor
  than through the 40 Christoffel symbols (VERIFIED, same paper section 2.1).
- Blacklight integrates only in Cartesian KS, which keeps covariant energy exactly conserved
  (stationary chart) and avoids pole problems; the price is that L_z is not exactly
  conserved (VERIFIED, 2203.15963 section 2.2). KPolaris supports Cartesian and spherical KS
  plus BL, MKS, FMKS (VERIFIED, rechecked); ipole is chart-agnostic and in practice consumes
  MKS/FMKS-family GRMHD data (CORRECTED: it is not a "Cartesian KS" code). So Cartesian KS is
  the choice of GRay2 and Blacklight and one supported chart of KPolaris, not a universal standard.
  GRay2 recheck: in double, KS was faster than BL on most GPUs (up to ~40%) but NOT on the
  GTX 780 (6.96 vs 6.44 ns); on CPUs KS double was ~19% slower than BL (Table 2).
- Kerr-Schild form: g_mu_nu = eta_mu_nu + f l_mu l_nu, with f = 2Mr^3/(r^4+a^2 z^2) (GRay2
  eq. 2-4, VERIFIED). Hamiltonian H = (eta^{mu nu} p_mu p_nu - f (l.p)^2)/2 is what v1
  implemented, citing Visser arXiv:0706.0622 (read in the v1 header comment; Visser not
  re-read). Flat space is M=0, so f=0 and the Hamiltonian is Minkowski. DERIVED: the flat
  case is not a separate code path, only a parameter value.
- GYOTO uses a Hamiltonian formulation because it removes sign indeterminacies at turning
  points (VERIFIED, 2311.18802 section 2.1). DNGR calls its super-Hamiltonian form "well
  behaved and robust at turning points" versus the usual R/Theta square-root form
  (VERIFIED, 1502.03808 appendix A.1).
- Closed-form alternative: Kerr null geodesics have a complete explicit solution in Legendre
  elliptic integrals and Jacobi elliptic functions (Gralla and Lupsasca,
  https://arxiv.org/abs/1910.12881; abstract and text read, VERIFIED). Useful as a test
  oracle and for precomputed lens maps; it does not carry matter, disks or arbitrary metrics.

### F4. Step control and accuracy

- Blacklight recommends Dormand-Prince RK5(4)7M and cites Velasquez-Cadavid et al. 2022 as
  finding DP "far more accurate" than other common integrators for null geodesics around
  black holes (comparison paper not read, REPORTED). Its fixed-step mode scales the step as
  f_step (r - r_horizon) (VERIFIED, 2203.15963 section 2.2). DERIVED: with step proportional
  to r the step count grows only logarithmically with the outer radius.
- RAPTOR step (VERIFIED, eq. 21-24): dlambda = 1 / (1/dx1 + 1/dx2 + 1/dx3), shrinking near
  the horizon and the axis.
- KPolaris: tolerance 1e-10 adopted; RIAF max-component image NMSE 1.5e-7 and <1.8e-3 for
  two GRMHD inputs against a finer KPolaris reference; geodesics and parallel-transported
  basis converge at 4th order under fixed-step refinement (VERIFIED, sections 4.1, 4.3).
- Interstellar's DNGR: bundle evolution "triples the time per integration step" compared with
  the central ray alone; 30 minutes to several hours on 10 CPU cores per 23-megapixel IMAX
  frame (VERIFIED, 1502.03808 appendix A.4-A.5).
- Constants-of-motion monitoring as an error control works: a Monte Carlo curved-space path
  tracer halves the step when conserved quantities drift past a threshold and terminates the
  ray if they drift further (VERIFIED, Mandal et al. 2021,
  https://www.scitepress.org/Papers/2021/102176/102176.pdf, algorithm section).

### F5. Precision and the RTX 5070 Ti

- GRay is a single-precision GPU code (per ipole-GPU authors' private communication,
  REPORTED via https://arxiv.org/abs/2302.02733; GRay2 benchmarks give single and double).
  GRay2 table 2 (ns per RK4 step per ray, BL, AoS): GTX 780: 0.174 single vs 6.44 double;
  Titan Black: 0.167 vs 1.65; GT650M: 2.49 vs 70.6; text: single is "up to a factor of 28"
  faster than double on consumer cards (VERIFIED, 1706.07062 section 4, table re-read). CORRECTED:
  the table itself gives 28x (GT650M), 37x (GTX 780) and 9.9x (Titan Black), so the text's "28"
  understates GTX 780; these are Kepler-era cards (1/24 FP64 on GTX 780), not Blackwell.
- Odyssey: ~1 ns per RK step per photon in double on a GTX 780 Ti, below 1 ns for large
  images (VERIFIED). Steps per ray are not tabulated in what I read.
- ipole CUDA port (Moscibrodzka and Yfantis 2023): speedup vs one CPU thread up to 1279
  (semi-analytic) and 980 (GRMHD) on an A100 (9.7 TFLOPS FP64), and "of the order of 100" on
  a GeForce RTX 3070 desktop whose double-precision rate is lower by "a factor of 30"; against
  a 16-24-thread CPU the gain is roughly speedup/N (VERIFIED, 2302.02733 sections 3-4). They
  say single precision could add speed but "a hardware limit" from data transfer exists.
- RTX 5070 Ti FP64 is 1/64 of FP32, about 0.69 TFLOPS vs 43.9 TFLOPS (REPORTED, spec
  aggregators e.g. https://fpsbench.com/gpu/nvidia-geforce-rtx-5070-ti; NVIDIA's own spec page, rechecked, lists no FP64 figure; one aggregator gives 45 TFLOPS
  FP64, an evident error; remains REPORTED, S2 decides). DERIVED: a consumer-GPU double-precision geodesic engine is likely
  only a small multiple faster than the 16-thread CPU, so CPU must stay a first-class GR
  backend; GPU float would be the viewport-speed route, if its accuracy is acceptable.
- DERIVED, from KPolaris: it runs double on an RTX 5090 (also a 1/64-FP64 GeForce; VERIFIED
  that it is double and RTX 5090) and a full 256^2 image takes 2.13-5.09 s including I/O and
  GRMHD input. That is ~13-31k pixels/s end to end, i.e. minutes per 1080p frame in double,
  so double GR on GeForce is a batch tool, not a viewport tool (overheads included, order of
  magnitude only).
- DERIVED budget: at the ~1 ns/step/ray scale and S steps per ray, a 1920x1080 frame costs
  about 2 ms x S if every pixel integrated. Only pixels whose rays enter the curved region
  need integrating, and the lens map is static while the camera is static.

### F6. Curved rays against a triangle BVH

- Piecewise-linear geodesics: v1 tested the scene against the chord of every accepted step
  (v1 gr_integrator.h, #1063, citing Groller 1995, "Nonlinear ray tracing", Visual
  Computer 11, REPORTED via search hit https://doi.org/10.1007/BF01901044; paper itself not
  read). v1 header read directly: VERIFIED.
- A naive non-linear Monte Carlo path tracer (OpenCL, RK4, steps decreased "until we
  converge to an intersection", no BVH) takes about 80 minutes for a 1024x768 Cornell box
  with a Schwarzschild hole on a 4-thread CPU at 1000 spp, 100-200 min for Kerr (VERIFIED,
  Mandal 2021 section 5.4). Its Minkowski render "exactly matches" a normal path tracer.
  CORRECTED: the paper (re-read) reports the times on a 2.4 GHz i5 with 4 threads and says
  nothing about BVH absence; its Cornell box holds a mirror and a glass sphere, so the 80 min is
  dominated by 1000 spp x RK4 x error smoothing, not shown to be due to a missing spatial
  structure. It supports "curved path tracing is expensive", not "a BVH would fix it".
- A community note says the BVH/curved-ray problem has no neat answer: cast short linear
  segments (slow, needs tuning) or pre-warp the scene (REPORTED,
  https://forums.developer.nvidia.com/t/a-real-curve-ball-non-linear-ray-tracing/106982).
- DERIVED: a chord of length L on a path with geodesic curvature kappa departs from the arc
  by at most kappa L^2 / 8, so the segment length can be chosen from a geometric tolerance;
  a segment whose bounding box misses the scene bounds needs no BVH query. In flat space
  kappa = 0, a segment can be infinite, and the loop degenerates to one BVH trace.
- DERIVED, weak field: deflection ~ 4M/b (geometric units). For a 10 solar-mass hole
  (GM/c^2 ~ 1.5e4 m) at b = 1 AU this is ~4e-7 rad; for an HMXB companion (~0.2 AU
  separation, order 1e6 gravitational radii) the companion is in the weak-field region. The
  triangle geometry that matters to astrophysical scenes is almost never in the strong-field
  zone; the strong-field zone holds analytic matter (disk, torus, jet) and volumes.
- DERIVED, next-event estimation: a light-connection segment between two surface points is
  a two-point geodesic boundary-value problem inside a curved region, so NEE and bidirectional
  methods are valid only where the connection is flat. No source I read treats this; I flag
  it as an inference to test.

### F7. Redshift, Doppler, invariants

- Photon frequency seen by a four-velocity u is proportional to -u.k; fluid-frame frequency
  nu = (-u.k)/(-u_obs.k_obs) times observed frequency (VERIFIED, KPolaris eq. 5). So
  g = nu_obs/nu_emit = (k.u_obs)/(k.u_emit) covers gravitational redshift, Doppler and
  aberration in one expression; flat-space Doppler is the same code.
- Invariants (VERIFIED, Blacklight eq. 24, KPolaris eq. 4, ipole): I_nu/nu^3, j_nu/nu^2,
  nu alpha_nu are Lorentz invariant, so transfer along the ray is frame-free:
  dI/dlambda = j_I - alpha_I I with I = I_nu/nu^3. DERIVED: in wavelength units the
  invariant is I_lambda lambda^5 (since I_nu = I_lambda lambda^2/c and nu = c/lambda).
- A null geodesic is achromatic in vacuum GR, so several frequencies share one trajectory
  and carry separate Stokes vectors and radiation-step controls (VERIFIED, KPolaris
  section 2.4). Spectral samples therefore ride on the ray; they are not part of the
  geodesic state.
- Units used by the field: geometrized G=c=1 for ray geometry, physical units for
  emission coefficients (VERIFIED, KPolaris section 2). The photon momentum is normalised so
  that -k.u_obs equals the observed frequency; the affine parameter scale then carries the
  physical path length (ipole text, KPolaris section 2.2).

### F8. Covariant transfer, polarisation, observers

- Covariant polarised transfer in a parallel-transported basis: dS/dlambda = J - K S with
  Stokes S, emissivity vector J and 4x4 propagation matrix K (absorption plus Faraday rotation
  and conversion); parallel transport of the basis e_A is d e/dlambda = -Gamma k e
  (VERIFIED, KPolaris eqs 1-7). ipole uses a split scheme with analytic emission/absorption
  steps and Faraday half-steps, and tests it in flat space with "snake" Cartesian coordinates
  and near the Kerr horizon with second-order convergence (VERIFIED, 1712.03057 section 4).
  KPolaris uses symmetric Strang splitting F/2 A F/2, second order for constant coefficients,
  with separate caps on geometric step, radiation step, absorption depth and Faraday depth
  (VERIFIED, section 2.3 and appendix A). Coport solves the coupled equation without
  splitting (VERIFIED abstract).
- Two-pass GPU scheme (ipole GPU): trace the geodesic backward from the camera to the boundary
  with negative increments, then recompute it forward while solving transfer, so persistent
  storage is proportional to pixels, not path length (VERIFIED, KPolaris section 2.2).
- Camera: position, four-velocity and an orthonormal tetrad; pinhole rays launch from one
  event, plane-parallel rays from an image plane (VERIFIED, KPolaris section 2.1). Emitter
  frame: tetrad built with e_0 = plasma four-velocity (VERIFIED, ipole step list).
- Ray bundles (DNGR): evolve the geodesic-deviation equations along the central ray to get
  an elliptical footprint on the sky, giving filtered, flicker-free lensing; ~3x per-step
  cost (VERIFIED, 1502.03808).
- Time: slow-light (finite light-travel time through time-dependent plasma) needs the
  coordinate time t along the ray and streamed snapshots; KPolaris interpolates linearly
  between two bracketing snapshots (VERIFIED, section 2.4). Slow-light flux differs
  from fast-light by <5% for the 2-D GRMHD models tested in RAPTOR (VERIFIED).

### F9. Emission physics and where the data comes from

- Thermal synchrotron fits: Pandya 2016 and Dexter 2016; power-law and kappa: Pandya 2016,
  Marszewski 2021; Faraday coefficients: Shcherbakov 2008 with Dexter 2016 correction;
  absorption by Kirchhoff's law (VERIFIED as used in KPolaris section 3; original papers not
  read). Plasma models: RIAF, magnetised torus, GRMHD snapshots in iharm3D HDF5, BHAC,
  KHARMA and AthenaK formats (VERIFIED, KPolaris section 3).
- Thin disk: Novikov-Thorne is in v1. Stellar-mass (HMXB) disks need a colour correction;
  f_col ~ 1.7 from Shimura and Takahara 1995, range 1.7-3 in later work (REPORTED, search
  snippets only; papers not read).
- Compton scattering in GR is done by Monte Carlo photon packets, not by ODE transfer: MONK
  (Zhang, Dovciak, Bursa 2019, https://arxiv.org/abs/1903.09241) traces packets along Kerr
  null geodesics with Klein-Nishina scattering (REPORTED; paper not read, code access by
  contact only). So an HMXB corona is a forward Monte Carlo workload, structurally different
  from backward camera ray transfer.
- Line and nebular data (licences matter, see F11):
  - NIST ASD v5.12, data updated November 2024; the page says the data are "copyright by the
    U.S. Department of Commerce on behalf of the United States" and that NIST "reserves the
    right to charge" (VERIFIED, https://www.nist.gov/pml/atomic-spectra-database). No public
    domain statement on that page.
  - CHIANTI: "freely available", users asked to acknowledge; redistribution and commercial
    terms not stated on the front page; IDL routines v11.0.5 dated September 2026 (VERIFIED,
    https://www.chiantidatabase.org/). ChiantiPy is ISC-licensed (REPORTED, search snippet).
    Recon said 11.0.1 (April 2025); that is stale.
  - Cloudy: repository page labels the code zlib; C25 (2025 release) adds j-resolved Ly-alpha,
    updated Stout C-like data, an X-ray-binary 1 keV feature (VERIFIED, repo page
    https://gitlab.nublado.org/cloudy/cloudy and https://arxiv.org/abs/2508.01102; no data
    licence stated).
  - ExoMol: CC BY-SA 4.0, citation requested (VERIFIED, https://www.exomol.com/data/licence/).
    Share-alike applies to adapted data, so tables derived from it inherit the licence.
- Pre-tabulation (DERIVED from how the codes consume data): none of the GR codes solve
  ionisation or level populations at render time; they take emissivity and opacity as
  functions of local state. The same holds for a renderer: bake j(nu; T, n, ...) and
  alpha(nu; ...) offline into tables, load tables at render time, evaluate by interpolation.
  SKIRT 9 is a precedent for per-component wavelength grids and a separate instrument
  grid (REPORTED, https://arxiv.org/abs/2003.00721 snippets).

### F10. Science output

- FITS standard is still 4.0 (language-edited 13 Aug 2018); no newer version listed
  (VERIFIED, https://fits.gsfc.nasa.gov/fits_standard.html).
- X-ray instrument modelling uses OGIP response files: RMF (redistribution matrix with
  EBOUNDS extension) and ARF (SPECRESP extension), plus PHA spectra that point to them
  (REPORTED via search results of https://heasarc.gsfc.nasa.gov/docs/heasarc/caldb/docs/memos/cal_gen_92_002/cal_gen_92_002.html
  and OGIP 92-007; memos not read in full).
- Photon counts: N = integral of I_nu / (h nu) over area, solid angle, bandwidth and exposure
  (DERIVED, definition). The film stage is where spectral radiance becomes counts, band flux,
  or display RGB.
- Existing codes write HDF5 (KPolaris) per its pipeline description (VERIFIED, section 5.1).
  I did not find a GR code that writes FITS cubes; that is a gap, not a convention.

### F11. Validation problems and published agreement numbers

| Comparison | Result | Source |
|---|---|---|
| EHT unpolarised, BHOSS/ipole/grtrans/RAPTOR/Odyssey/GRay2 etc. (Gold 2020) | total flux ~1% with production parameters; images converge with suitable parameters and distant camera; largest differences from camera placement near the hole | VERIFIED abstract via Crossref https://doi.org/10.3847/1538-4357/ab96c6 |
| EHT polarised codes (Prather 2023) | analytic model worst NMSE 0.012; GRMHD snapshot worst NMSE 0.02 (I), 0.04 (Q), 0.04 (U), 0.12 (V) | VERIFIED abstract, https://arxiv.org/abs/2303.12004 |
| RAPTOR vs another code on identical data | integrated flux relative difference <0.01%; fast- vs slow-light light curves differ <5% | VERIFIED, 1801.10452 abstract |
| GYOTO 2.0 vs ipole | NMSE <1e-4 for all configurations and Stokes parameters except Stokes U, radial field, high inclination (~1e-3); pixel errors typically <=0.1% | VERIFIED, 2311.18802 section 3.3 |
| KPolaris vs ipole, 256^2, 230/345 GHz | fast-light NMSE(I) 9e-6 to 1.9e-5, largest component 1.1e-4; slow-light largest 7.6e-4; flux differences 0.113% and 0.126% | VERIFIED, 2609.26298 sections 4.1-4.2, table 1 |
| Convergence | geodesics and transported basis 4th order (RK4); transfer 2nd order; constant-coefficient emission, absorption, Faraday reproduce analytic solutions to floating-point precision | VERIFIED, KPolaris section 4.1; ipole section 4 |

Analytic checks needing no other code (textbook items DERIVED, not looked up): Minkowski render
equals the flat path tracer (Mandal 2021 did this, VERIFIED); flat space in non-trivial
coordinates (ipole "snake" test, VERIFIED); Schwarzschild critical impact parameter 3 sqrt(3) M;
redshift sqrt(1 - 2M/r) for a static emitter; weak-field deflection 4M/b; Kerr closed forms
(Gralla-Lupsasca, VERIFIED); Hamiltonian and p_t drift as a per-ray diagnostic.

### F12. Lessons from v1 GR code (read-only)

Read: gr_integrator.h (487 lines), metric.h, gr_types.h, plugins/metrics (kerr 220,
schwarzschild 53, minkowski 22 lines).
- Metric is a virtual class returning `float gamma[4][4][4]` Christoffels and a float
  `inner_product` while the state is double: unusable in a CUDA kernel, and a precision leak.
- Integrator: DP45, atol 1e-8, rtol 1e-6, r_max 200, maxSteps 5000; rays that run out of steps
  are silently classed "captured". That violates "fail loudly" for science output; use a
  diagnostic channel instead.
- BL plus a Cartesian-KS chart near the axis with hysteresis (#897) was added because BL went
  stiff at sin(theta)=0; Cartesian-KS-only codes (F3) avoid the atlas.
- Scene hits by per-step chord callback (#1063) work with the BVH in practice.
- Disk crossing stores r, phi, g only: emission is a surface event, not a volume integral. GR
  used the oldest spectral type (380-780 nm, double, mt19937).

## Options and tradeoffs

These are the real forks; I do not force a count.

1. How the core represents spacetime.
   - Compile-time policy (template or concept) exposing Hamiltonian RHS, flatness flag,
     capture test, maximum safe step, and chart-to-Cartesian map: compiled into CPU and GPU
     from one source, flat space inlines to nothing. Cost: header-heavy; every spacetime
     added is a recompile of the GR integrator (acceptable, there will be 2-3).
   - Runtime virtual interface: v1's approach; incompatible with the GPU single-source goal
     unless devirtualised by a switch. Rejected on evidence (v1 CPU-only outcome).
   - Table-driven (precomputed lens map from the closed-form Kerr solutions, or from a
     one-time integration): fastest viewport, valid only for a static camera and for the
     vacuum region; cannot carry matter or moving emitters. A complement, not a replacement.

2. Metric form: Kerr-Schild Hamiltonian (analytic, three terms, no Christoffels; flat is
   f=0) vs generic g_{mu nu,lambda} supplied by the spacetime (needed only for numerical or
   exotic metrics). Primary should be the former; the interface should also admit the latter
   so GYOTO-style numerical metrics are not foreclosed.

3. Integrator: DP5(4) adaptive (Blacklight, v1) vs fixed RK4 with step doubling (KPolaris,
   GRay2, RAPTOR). Adaptive has divergent control flow per ray on a GPU warp; fixed RK4
   with a step rule tied to r has uniform flow but needs a tolerance study. Both are cheap to
   offer behind the same RHS; the spike decides the default. Symplectic or Verlet schemes
   exist in RAPTOR; no evidence they beat DP5(4) here, so skip.

4. Precision of the integrator: double everywhere (ipole, KPolaris, Odyssey; EHT-grade; GPU
   slow on a GeForce) vs float on the GPU for the viewport (GRay; 10-37x faster per GRay2 table 2 on
   Kepler GeForce) with double CPU as reference. The accuracy loss near the photon ring in float is
   not quantified by any source I found; it is the single most important spike (S1).

5. Curved region and the BVH.
   - Piecewise-linear segments tested against the BVH (v1, Groller): general, handles
     triangle geometry inside the curved region; cost is segments x queries; mitigated by a
     bounding region and by step lengths growing with r.
   - Exit-ray teleport: integrate inside a bounded region containing only analytic matter,
     then hand a straight ray (origin, direction, accumulated frequency shift) to the BVH.
     Cheapest; requires that no triangles live inside the region. Fits HMXBs and BH + distant
     scene; fails for the "black hole in a Cornell box" pedagogical case.
   - Both are one loop: advance by min(max safe step, distance to scene bound); a flat
     spacetime returns an infinite step. So the fork is not architectural; it is a limit
     that the integrator applies.

6. Transfer algorithm: backward camera ray + analytic emission/absorption segments
   (ipole/Blacklight/KPolaris; deterministic; polarisation-ready) vs Monte Carlo photon
   packets (MONK; needed for scattering). Different integrators sharing the spacetime and
   medium interfaces; the path-tracer core serves scattering but not the first. v1's lesson of
   one fat shared struct (+52% regression, recon section 2) says: separate per-integrator
   state structs, shared interfaces only.

7. Wavelength sampling. Hero-wavelength sampling suits RGB spectral rendering but fits narrow
   lines and science cubes poorly (a thermal H-alpha width ~0.1 nm is DERIVED, not sourced).
   The core needs a spectral sample type that is an arbitrary tagged frequency set, not a
   hard-wired 360-830 nm hero set; binned or per-line importance sampling can sit above it.

8. Output: display RGB only vs a float spectral-cube side channel. FITS 4.0 is stable and a
   hand-written writer is small (CFITSIO licence not checked). Blender N-channel pass
   behaviour is unverified here.

## Recommendation for Astroray 2.0

Evidence: F1-F8. Nothing here requires more than a thin spacetime and medium layer in the
first milestones; the rest is deferred but fits.

**R1. Spacetime = compile-time policy; Cartesian Kerr-Schild Hamiltonian as the one real
implementation; flat space is the same code with M=0.** Rests on F3 (GRay2 and Blacklight use
Cartesian KS, KPolaris supports it among several charts; flat is f=0) and F2 (single source compiled for CPU and GPU is
established). Interface: `rhs(x, p)` in Hamiltonian form, `isFlat()`, `captured(x)`,
`maxStep(x, p)`, `tetradAt(x, u)`, and a `Cartesian(x)` map. No Christoffel arrays, no
virtuals, no float metric on a double state. Keep an optional `dg(x)` for non-KS metrics.

**R2. Ray state hooks (the minimum that prevents retrofit).**
- Position as four numbers (t, x, y, z) in the integrator's scalar; keep t even in flat
  renders (slow-light, moving emitters, light-travel-time).
- Covariant momentum p_mu normalised at the camera so -p.u_cam equals the observed frequency
  (F7); the flat fast path stores only (origin, direction) and rebuilds p when entering a
  curved region.
- Spectral samples carried on the ray as observed frequencies (any positive value, X-ray to
  radio), never baked into the geodesic; emitted frequency per sample = observed times g.
- Throughput in the invariant I_nu/nu^3 (equivalently I_lambda lambda^5), converted at
  emission and at the film; in flat space with static emitters the factor is 1.
- Diagnostics channel per ray: terminated reason (escaped, captured, out of steps,
  numerical failure), steps used, Hamiltonian drift. Never fold failures into "captured".
- Polarisation and ray-bundle payloads are not added now; the rule is that GR integrators
  own their extended state struct (parallel-transported basis, Stokes vector, Jacobi
  matrix) and do not enlarge the shared path-tracer ray. Justification: v1's shared-struct
  regression and DNGR's 3x per-step cost for bundles.

**R3. Units and scale.** Ray geometry in geometrized units of one declared length scale
(M or gravitational radius) for GR scenes; scene import converts from Blender metres by a
declared factor; physical SI/cgs used only inside medium evaluation and at the film
(KPolaris pattern). Astrophysical dynamic range (km to parsec) breaks single-precision
absolute positions: store world positions relative to a per-scene origin and keep the
integrator state in its own scalar. Radiance scale: carry an explicit per-render exposure
factor so float stays in range across bands; each band documents it.

**R4. Wavelength range.** The spectral type takes any frequency domain; the visible
CIE/upsampling machinery is one registered sensor, defined only on 360-830 nm, and queries
outside a material's defined domain fail loudly. Do not hard-wire hero wavelengths into the
ray type.

**R5. Medium interface.** `Medium::eval(x, k, u_out) -> {j_nu, alpha_nu, (J_vec, K_mat)}` in
the comoving frame, plus the fluid four-velocity and a step-limit hint (absorption-depth cap).
The integrator chooses the scheme (analytic exp(-tau) segments as in Blacklight eq. 26 and
ipole; delta tracking for sparse volumes). The same interface serves a flat Blender volume
(u = 0, k.u = nu) now and a GRMHD or Cloudy-derived grid later. Do not store scattering in
this interface yet; MONK-style scattering is a later integrator.

**R6. BVH coexistence = one segment loop.** advance by min(maxStep, distance to scene bound);
test the chord with the normal BVH; position the hit on the arc at the same parameter; infinite
step in flat space so flat rendering pays one trace and nothing else (acceptance: within noise of
a plain path tracer; spike S3). Rests on v1 #1063 only (Mandal's cost is not evidence for BVH
benefit, see F6). Treat next-event estimation as valid only on flat connections
(`isFlat(a,b)`); curved scenes take BSDF/emission-sampled paths. Rests on F6 (v1 #1063 works).

**R7. First GR milestone, when it comes.** Double CPU reference integrator in Cartesian KS
with DP5(4); Minkowski, Schwarzschild, Kerr; the analytic checks in F11; then the same header
compiled for the GPU in float and double, with the S1 error table deciding which the viewport
uses. Backward camera transfer with analytic segments; thin-disk (Novikov-Thorne) and a
simple optically thin torus as the first media.

**R8. Science output.** Film stage owns the conversion: spectral radiance, band-integrated
flux, photon counts, display RGB; the core emits per-pixel spectral samples to a float cube
buffer. A small FITS 4.0 writer (image HDUs, WCS spectral axis, units keywords) is a tool
beside the engine. OGIP RMF/ARF convolution is a post-process script, not engine code.

**R9. Emission tables are an offline pipeline.** A data-build tool turns NIST/CHIANTI/Cloudy/
ExoMol into binary emissivity/opacity tables (T, n, ... x frequency) in a documented layout;
the engine reads tables only. Do not commit derived tables until licences are settled (F9:
ExoMol CC BY-SA propagates; NIST copyright notice; CHIANTI terms not stated).

**R10. Licensing posture for borrowing.** ipole, KPolaris (BSD-3) and Blacklight (public
domain) can be read and, with attribution, reused; RAPTOR, Odyssey, GYOTO are GPL. The cleaner
route per CLAUDE.md rule 5 is to implement from the papers' equations and cite them, as v1 did.

## What a measurement spike must settle

All on the RTX 5070 Ti and the 16-thread CPU; named commands, numbers kept in the spike's
own output, none carried by assertion.

- S1. Float vs double Cartesian-KS geodesics (DP5(4) and fixed RK4): error vs a long-double
  reference for Schwarzschild and Kerr (a=0.9), rays near the critical impact parameter, photon
  ring orders n=0,1,2; ns/step/ray; steps/ray at tolerances 1e-6, 1e-8, 1e-10; register and
  occupancy counts. Decision output: which precision the viewport may use, and the angular
  error in pixels at 1080p.
- S2. FP64:FP32 throughput on this card (FMA microbenchmark) to confirm 1/64.
- S3. Segment loop overhead: flat scene with 1M triangles, segment loop with maxStep = infinity
  vs plain trace; curved sphere radius sweep (30 M to 1e5 M) with a BVH scene outside it: segments
  per ray and cost; confirm step-grows-with-r keeps the count near logarithmic.
- S4. Single source: one spacetime header compiled for CPU and CUDA; per-ray agreement between
  the two, and between float GPU and double CPU as a distribution.
- S5. Warp divergence of adaptive vs fixed step on a lensed viewport frame.
- S6. Spectral: float dynamic range X-ray to radio with an exposure factor; hero vs binned noise
  on a narrow line.
- S7. Emissivity table as a 3-D/4-D GPU texture vs CPU; interpolation error vs table size.
- S8. Blender 5.3: N-channel float passes plus a side-writer; cost of a 64-band cube per frame.
- S9. Ray-bundle (geodesic-deviation) per-step cost here vs DNGR's reported ~3x.

## Unverified or conflicting

- NVIDIA's own FP64 figure for the 5070 Ti (aggregators say 1/64; one aggregator lists "?"). S2.
- No source I found measures float32 accuracy of Kerr-Schild geodesics near the photon ring.
  GRay used single precision for shadows and rings (REPORTED) but I did not read its accuracy
  section; the 2022 thick-disk photon-ring paper (https://arxiv.org/abs/2206.12066) has an
  integration-accuracy appendix whose precision I could not confirm.
- Steps per ray for Odyssey, GRay2 and KPolaris production images are not in the text I read, so
  per-frame cost cannot be derived from ns/step. S1.
- KPolaris Table 2 layout is garbled in the PDF text; I cite the prose ranges only.
- The Gold 2020 list of compared codes comes from the recon (via the EHT Sgr A* paper); I verified
  the abstract through Crossref but not the full code list or the arXiv id.
- Velasquez-Cadavid et al. 2022 (DP most accurate), Groller 1995, Shimura and Takahara 1995,
  MONK internals, OGIP memo contents, GYOTO's licence and the NVIDIA forum thread are REPORTED.
- Licences not checked: GRay2, Coport, CFITSIO, Kokkos; NIST copyright page and CHIANTI tarball
  licence text unread.
- Blender 5.3 pass and colour-management behaviour for float cubes: not covered here.
- NEE invalid across curved connections (F6) is my inference; no source addressed it.
- Whether Astroray 2.0 will be GPL or permissive is not stated in CLAUDE.md; R10 depends on it.

## Verification pass

Independent check 2026-11-14 (primary PDFs text-extracted locally; abstracts fetched).

- Single-source CPU+GPU GR (RAPTOR OpenMP/OpenACC, 10,707 vs 104,900 geodesics/s; KPolaris
  Kokkos; GRay2 OpenCL): CONFIRMED. https://arxiv.org/abs/1801.10452 (abstract: "hardware-agnostic";
  OpenMP/OpenACC and rates in appendix), https://arxiv.org/abs/2609.26298 (KPolaris: BSD-3-Clause
  stated in text, github.com/zelinzh/KPolaris, double precision, RTX 5090 workstation, 10-182x vs
  one thread, GPU faster in 8 of 9 cases; arXiv abstract page lists only CC BY 4.0 for the paper),
  https://arxiv.org/abs/1706.07062.
- Cartesian KS "standard chart (GRay2, Blacklight, KPolaris, ipole)": CORRECTED. GRay2, Blacklight
  yes; KPolaris one of several charts; ipole chart-agnostic. g = eta + f l l and f=0 flat: unchanged.
- GRay2 "65% more flops", KS up to 40% faster in double on most GPUs: CONFIRMED with caveat (not
  GTX 780; CPUs KS slower). https://arxiv.org/pdf/1706.07062
- GRay2 single vs double (2.49/70.6, 0.174/6.44): CONFIRMED; CORRECTED the "28x" ceiling (table
  gives up to 37x) and the old "22-38x" phrasing.
- ipole CUDA ~1000x on A100, ~100x RTX 3070 vs one CPU thread: CONFIRMED (1279 and 980; "order of
  100"; FP64 lower by factor of 30; 9.7 TFLOPS A100). https://arxiv.org/abs/2302.02733
- RTX 5070 Ti 1/64 FP64 (~0.69 TFLOPS): COULD NOT VERIFY from NVIDIA; spec aggregators agree
  (storagereview.com, fpsbench.com, hmc-tech.com, flopper.io); NVIDIA page has no FP64 row.
  https://www.nvidia.com/en-us/geforce/graphics-cards/50-series/rtx-5070-family/ Stays REPORTED.
- Float accuracy near photon ring unquantified: COULD NOT VERIFY any source (none found in this
  pass either); S1 remains the key spike.
- Invariants I_nu/nu^3 etc., g = (k.u_obs)/(k.u_emit), achromatic geodesic: CONFIRMED in
  KPolaris sec 2.3 (invariant Stokes vector S = (I,Q,U,V)/nu^3, eq. 4); achromatic follows from
  vacuum null geodesics. Blacklight eq. 24 not rechecked.
- EHT agreement numbers: Gold 2020 ~1% total flux at production settings CONFIRMED
  (https://doi.org/10.3847/1538-4357/ab96c6, via https://open.bu.edu/items/415de586-43e7-4b25-8a9e-6ae87d84a592);
  Prather 2023 NMSE 0.012 analytic, GRMHD worst 0.12 (V) CONFIRMED (https://arxiv.org/abs/2303.12004);
  RAPTOR <0.01% flux, <5% slow vs fast light CONFIRMED (abstract 1801.10452); KPolaris vs ipole
  flux 0.113%/0.126% CONFIRMED (text), NMSE 1e-5 order not rechecked; GYOTO 2.0 vs ipole
  "perfect agreement" in abstract, NMSE <1e-4 figures COULD NOT VERIFY (https://arxiv.org/abs/2311.18802).
- Mandal 2021 "~80 min per Cornell box frame": CONFIRMED number (1024x768, 1000 spp, 4 threads,
  i5; Kerr 100-200 min; wormhole ~250). CORRECTED inference: the paper does not attribute cost to
  a missing BVH, so it is no evidence for the segment loop; that rests on v1 #1063 and DERIVED
  geometry. https://www.scitepress.org/Papers/2021/102176/102176.pdf
- Recommendation: still holds (compile-time Cartesian-KS policy, flat = f=0, one segment loop,
  double CPU reference first, float GPU only if S1 passes). Strengthened: double GR on a GeForce
  (KPolaris on RTX 5090: seconds per 256^2) is batch-only, so viewport GR depends on S1.
