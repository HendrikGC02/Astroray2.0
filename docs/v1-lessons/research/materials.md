# Materials: microfacets, multiple scattering, glass, layering, thin film, subsurface, hair

Digest of what Astroray 1.0 learned about surface and fibre materials, for a rewrite judged against
physics and the OpenPBR specification (not Cycles pixel parity). Cycles behaviour is recorded as
knowledge of Blender semantics and as a source of reference algorithms. "Furnace" = white furnace
test (uniform white environment, non-absorbing material, expected radiance 1.0). The commands that
produced the v1 numbers are not preserved; re-measure before relying on them.

## Established findings

### Dielectric orientation and radiance convention
- Enter versus exit must come from the geometric front-face flag, never from sign(dot(wo, normal))
  when the stored normal is already flipped toward the viewer (always positive). v1 evaluated
  "entering" 100% of 274,809 calls, including 61% true exits, so eta^2 never cancelled. Furnace:
  clear sphere ior 1.5 0.511 to 0.983; rough glass 0.11-0.82 to 0.997-0.999. The same error hit a
  reflection-pdf Fresnel term (pdf off up to ~20x on internal reflection) and transform wrappers that
  re-ran face-normal setup and forced front-face (scaled glass furnace 0.39 to 0.963). Orientation is
  a stored hit attribute that must survive every transform.
- Radiance convention: transmission carries eta^2 (0.444 entering, 2.25 exiting at ior 1.5); it
  telescopes to 1 on closed solids (maxErr 0.0 over 1.6M walks). Any colour pipeline that clamps
  albedo to [0,1] clips the exit factor (GPU furnace 0.991/0.705/0.604 at ior 1.0/1.5/2.0), and the
  same clamp made reflection tails (eval up to ~40) read 28% dark. Rule: factor any magnitude >1 out
  as a scalar and upsample only the colour.
- A radiance furnace entangles the 1/eta^2 compression; multiply by eta^2 (flux furnace) to isolate
  real loss.
- Smooth glass: with reflection probability R, f/pdf must be 1: reflection f = Fresnel (1 on TIR),
  transmission f = base*eta^2*(1-Fresnel). Dropping the common Fresnel factor gave furnace 1.784 at
  R=0 (fixed to 0.990). Forced-TIR delta pdf must equal the transmission probability (pbrt-v4 s9.5).
- Rough BTDF (Walter 2007 Eq. 21) must include the trailing |cos(wi)| when the integrator adds no
  cosine (else 1/|cos| inflation), and needs the true Smith G1. A combined visibility form G1/(2x)
  used where G was expected injected a constant ~0.25 deficit (rough transmission ~0.30).
- Hard upper clamps on BRDF/BTDF eval (4.0, GPU 10) truncate the GGX tail (D peak ~7800): furnace
  0.178 to 0.9986 at roughness 0.1 once removed; metal rendered black (0.215 vs 0.604) after "+0.001"
  epsilons in D and the pdf denominator were removed. Floor at 0, no cap; clamp only at integrator
  level. The epsilons themselves deflated the pdf, most at grazing; remove, do not compensate.
- VNDF disk warp must be Lerp(t, h, py) = (1-t)h + t*py (pbrt-v4). A swapped form put 100% of sampled
  half-vectors on the wrong azimuth side: sampled transmission peak 167.5-170 deg versus pdf peak
  151.8 deg (Snell 151.87); fixed offset ~0.7 deg. Check a sampler against its pdf by peak position,
  not only chi-squared. Rough dielectric with |eta-1| < 1e-4 must be treated as singular (half-vector
  zero; Cycles does this); a 1.0001 nudge gave pdf ~1e7 and a black pane.

### Multiple-scattering energy compensation
- Kulla and Conty 2017: missing = (1-E)/E; Fms = Fss*Eavg/(1 - Fss(1-Eavg)); darkening (1+Fms*missing);
  scale 1/E. The scalar 1+(1-E)/E is valid only for white Fss. Cycles (4.0+) uses Turquin 2019 albedo
  scaling for the same job; measured agreement is within the A/B band (neutral r0.3/0.6/0.9
  0.997/0.977/0.936 of Cycles), so no switch was warranted.
- Directional-albedo tables must be baked with the G and sampling the renderer uses. A table baked
  for height-correlated Smith + VNDF applied to Smith-Schlick (k=(r+1)^2/8) + NDF sampling left
  furnace at 0.81-0.88. A runtime table of 256 uniform-hemisphere samples per cell gave E near 0 at
  low roughness (0.0407 vs 0.9985, 24.6x). An unpublished weight roughness*(2-roughness)*1.3 was wrong.
  An additive multiscatter term must carry NdotL if eval is expected to include the cosine.
- Always furnace-test in linear, unclamped space: gamma clamping made a 1.77x gain read 0.998.
- In a one-sample-MIS mixture, eval and pdf must use the same normalised selection weights; raw
  weights in eval with normalised selection in pdf gave a flat ~2x gain (furnace 1.975).
- Single-scatter rough glass loses a lot at high roughness and grazing (dead energy 68% at
  r0.85/mu0.1, 85% at r1.0/mu0.1; GPU single-scatter rim loses up to 51% at ior 1.5 r1.0). Options:
  (a) Heitz 2016 random walk: R+T = 1.000, 0% dead over a 4x5 roughness x mu grid at ior 1.45, order
  cap 16 matched a 32 oracle; (b) Cycles-style 1/E albedo scaling; (c) re-route absorbed samples to a
  smooth delta event (furnace 0.909/0.645/0.524 to 0.968/0.958/0.955 at r0.5/0.85/1.0; a hack, but
  the removal of it dropped CPU r1.0 0.997 to 0.788). Walk details: dielectric phase weight is 1
  (perfect importance sampler); alpha floor max(roughness^2, 0.0064); use signed cos_i in the VNDF
  denominator (|cos_i| gave a 1e11 firefly; bound 1/d^2 <= 4.9); MIS pdf = first-event VNDF + 0.05
  diffuse floor, and sample() must report that mixture pdf; stochastic NEE eval (Eq. 42) with a pure
  hash RNG and one walk; N=4 walks cost 8-21% for <= 0.17-point sigma gain.
- Centre over-brightness of the walk (~1.61x Cycles at r0.85) was confirmed physical by two
  independent oracles (explicit heightfield, numpy sphere tracer, MS/SS = 1.608). Cycles' 1/E and the
  walk differ in R:T split by up to 5.5x (r1.0/mu0.1) and at an exit interface 1/E gives 2.9-4.6x less
  internal reflection. Matching Cycles at r >= 0.85 needs 1/E, not the exact walk.
- Thin film on rough dielectric must not take the walk (it double-counted direct light and dropped
  iridescence); film path stays single-scatter.

### Cycles glass and Principled semantics (Blender knowledge)
- Cycles 5.2 default rough glass is MULTI_GGX: isotropic GGX (Walter 2007), VNDF sampling (Heitz
  2018), pdf_reflect = avg(R)/avg(R+T), scale 1/E with E from ggx_glass_E[sqrt(alpha), mu,
  z=sqrt(|ior-1|/(ior+1))] (inverse tables when relative ior < 1); darkening
  (1+Fms*missing)/energy_scale also drives lobe selection. Glass tables cover the combined glass
  closure, not a transmission-only lobe. Cycles' eval evaluates invalid-side refraction (a TODO), which
  is the source of its bright lit-side rim through NEE: a quirk, not physics.
- Glass BSDF tint: reflection = Color*F, transmission = Color*(1-F), f0 from ior untinted. Principled:
  generalized Schlick, specular tint scaling f0, transmission tint sqrt(base colour). Tinting only
  reflection was incomplete; encoding Color^2 into base colour matched only analytically
  (L = cF + c^2(1-F)^2/(1-cF)). Structure is per-closure.
- Principled closure order: emission, sheen, coat, metallic, transmission, specular, subsurface,
  diffuse; each reflective layer attenuates what lies below by closure_layering_weight =
  weight*saturate(1 - max(albedo/weight)). Cycles timeline: 4.0 rewrite; 4.2 thin film; 4.3 EON
  diffuse roughness + physical conductor; 5.0 film on conductors; 5.2 thin wall.
- Bounce class derives from the sampled closure label bits (DIFFUSE, GLOSSY, plus TRANSMIT and
  SINGULAR); diffuse transmission does not set the diffuse-ancestor flag.
- Add Shader: f = fA + fB, no normalisation. Mix: weights (1-fac, fac), fac saturated. Estimator:
  one-sample balance-heuristic MIS (Veach 1997 Eq. 9.15), pick child p=1/2, return summed f and
  pdf 0.5(pdfA+pdfB); delta children returned alone. Mix(Principled,Principled) lowered to lerped
  parameters is an approximation (averages sharp and blurred highlights).
- Alpha/transparent: Cycles bsdf_transparent (wo = -wi, pdf = eval = 1e6); the weight cancels in the
  MIS estimator (Veach 1997 s9.2.4). v1 approximations: transparent bounces consumed max_depth
  (Cycles has transparent_max_bounce, default 8); NEE shadow rays treated alpha as opaque.

### Layering and OpenPBR-style coupling
- Layering albedo is lobe energy times the normalised generalized-Schlick term:
  E*(1+Fms*missing)*mix(f0,1,s). Omitting E*(1+Fms*missing) left diffuse at 0.973-0.981 of furnace
  (base 0.8 read 0.782 vs 0.803); fixed to within 0.3%. Per-channel layering was wrong; exact scalar
  form needed. Coat tint furnace within 0.9%.
- Multiplying a view-angle Fresnel into layering albedo over-attenuated the diffuse at grazing
  (1.2-5.5x over a Monte Carlo oracle at mu <= 0.5). Cycles uses a lobe-averaged Schlick fit (16^3
  table), so Cycles-versus-MC mismatch is inherent (it overestimates 2.07-3.25x at normal incidence,
  high roughness, because the floor is f0).
- Uncoupled diffuse + specular overshoots at grazing (roughness 0.1, cos 0.1: 0.73 + 0.48 = 1.20).
  Albedo-scaling layering f = f_spec + (1 - E_spec(wo))*f_diffuse gives worst case 0.86 (OpenPBR
  glossy-diffuse under base substrate). Non-reciprocity is accepted by OpenPBR and Cycles.
- A material with specular IOR level 0 must derive the Fresnel exponent and layering eta from the
  effective f0 (Cycles ior_from_F0), not keep ior 1.5; else grazing reflection is mirror-like
  (1.2-2.0x Cycles) and diffuse 0.82x. Transmission keeps the real ior.
- Coat: GGX dielectric over base with Beer absorption: cosNT = sqrt(1-(1/coat_ior)^2(1-cosNI^2)),
  weight *= mix(1, tint^(1/cosNT), coat_weight). Cycles sheen is the 2022 LTC model (32x32 table of
  {aInv, bInv, albedo}; eval = (1/pi)*max(z,0)*(a/lenSqr)^2, disk-warp sampling), not Kulla-Conty
  Charlie. Clearcoat has no own LUT; it uses the GGX E path.
- Anisotropy (Cycles): aspect = sqrt(1-0.9*aniso), alpha_x = r^2/aspect, alpha_y = r^2*aspect,
  tangent rotated about N by rotation*2pi; metallic and specular only (transmission stays isotropic
  with alpha = r^2); energy table indexed sqrt(sqrt(ax*ay)); height-correlated Smith lambda(t) =
  0.5(sqrt(1+t)-1), G = 1/(1+lI+lO). Isotropic path must stay bit-identical at aniso 0. Sampling by
  slope stretch h = normalize(-ax*r*cos(phi), -ay*r*sin(phi), 1).
- Thin wall (Cycles 5.2): alpha_T = saturate(alpha*sqrt(3.4(eta-1)(eta-0.5)^2/eta^3)); subsurface
  split diffuse 0.5(1-g), translucent 0.5(1+g). F82-tint conductor (Kutz/Hoffman) and EON diffuse
  (Fujii, OpenPBR) were ported from Cycles; generalized Schlick with negative exponent is
  reparameterised s = saturate(inverse_lerp(F0, 1, fresnel_dielectric_cos)).

### Thin film
- Belcour and Barla 2017: Airy summation Eq. 10 truncated at m=3 (residual O(r123^4)); OPD =
  -2*film_ior*thickness*cos(theta2); polarised dielectric and conductor (Born-Wolf, F82 magnitude,
  physical phase) Fresnel; result saturate(0.5*(Rs+Rp)); film active only past thickness 0.1 nm, with
  smoothstep blend below 1 nm; TIR handled.
- A spectral renderer can use an analytic sensitivity {cos(m*Delta), -sin(m*Delta)} per wavelength,
  no LUT; Cycles uses a per-RGB-channel CIE sensitivity LUT (512x6). Sign trap: exp(+i*Delta) is
  invisible for dielectrics (phi in {0, pi}) but makes conductors look ~70 nm thin (230 nm looked like
  Cycles 300 nm). Verify against closed-form Airy over n+ik, not just dielectrics. The Belcour s4 RGB
  projection is omitted on the spectral leg, so hue differs from Cycles by construction.
- Film replaces only the single-scatter F; energy compensation stays film-free. Film scales eval
  only (sampler and selection pdf unchanged, so chi-squared invariance holds). Cycles also applies
  film to smooth glass. Film ior equal to base ior gives no iridescence in either engine.

### Subsurface
- Random-walk SSS (Chiang, Kutz, Burley 2016): sigma_t = (1/radius)/(1-g); alpha from van de Hulst
  inversion (d'Eon 2016 Eq. 53.7, g >= 0) or arctan remap (g < 0); clamp alpha < 0.2 by scaling
  throughput; RGB channel MIS; at exit continue with a diffuse lobe; walk per RGB and upsample exit
  throughput as colour only. It is an integrator seam, not a closure, and needs an object-local
  intersector (the biggest dependency). Dwivedi guiding: eval p(mu) = 1/((v-mu)*log((v+1)/(v-1))).
- v1's baseline was a Lambertian lobe with diffuse scaled by (1-subsurface): no transport.

### Hair and curves
- Fibre model: Chiang et al. 2016 Principled Hair with Cycles' coefficients: v_R=(0.726b+0.812b^2+
  3.7b^20)^2, v_TT=0.25v, v_TRT=4v, v_TRRT+=4v; s=(0.265b+1.194b^2+5.372b^22)*sqrt(pi/8); delta phi =
  2p*gamma_t - 2*gamma_o + p*pi; A0=f, A1=(1-f)^2 T, A2=(1-f)^2 T^2 f, A3=A2*Tf/(1-Tf); eta=1.55;
  eumelanin sigma_a (0.506,0.841,1.653), pheomelanin (0.343,0.733,1.924); colour to sigma_a via
  D(b)=5.969-0.215b+2.532b^2-10.73b^3+5.574b^4+0.245b^5, sigma=(ln c/D)^2. Cuticle tilt R:2a, TT:1a,
  TRT:4a with Cycles' sign (negated). Coat -> m0_roughness=(1-coat)*v on R only. Residual N3=1/(2pi).
  h = 2v-1 (v = cross-section parameter). Frame X = curve tangent, Y = normalize(cross(X,wo)), Z =
  cross(X,Y), rebuilt per call (view dependent).
- Spectral hair: derive sigma_a per wavelength from reflectance as pbrt-v4 (albedo, then
  SigmaAFromReflectance); a piecewise-linear sigma_a upsample made channels read 1.24/1.26/1.33 vs
  Cycles (fixed 1.007/1.000/1.005). Other defects found: cuticle-tilt sign; mirrored leaf v flipping h
  and thick-curve normal; thick curves shaded on the axis (belongs only to thick mode).
- Intersection: pbrt-v3 recursive Bezier clipping chosen over Cycles curve_intersect (Embree Newton
  refinement) and Reshetov 2017 (exact for any cubic including straight, self-contained, gives u/v);
  Newton and cylinder pruning deferred as performance work. Catmull-Rom to Bezier: B0=P1,
  B1=P1+(P2-P0)/6, B2=P2-(P3-P1)/6, B3=P2; phantom endpoints are clamped duplicates.
- Self-intersection: pbrt-v3's flattened test puts the hit inside the tube, so continuation and shadow
  rays re-hit the same segment (0.38 of 0.44 radiance in a probe); the fibre BSDF already covers
  transport through the fibre, so this double counts. Cycles' rule: rays skip the last primitive, and
  since the BVH primitive is the whole curve, a ray leaving any segment never re-hits any segment of
  that curve (closest-hit and shadow, native and Embree); other strands are not skipped. Skipping
  only the exact segment left adjacent-segment re-hits (bent strand image-sum ratio 0.33 vs 1.00).
- Test hygiene: a probe whose up vector was parallel to the view gave a NaN basis, so two "miss"
  tests passed as false greens. Check for NaN before trusting a miss.

## Sources and reference implementations

| Topic | Source (as recorded) |
|---|---|
| Multi-scatter compensation | Kulla and Conty 2017, "Revisiting Physically Based Shading at Imageworks", SIGGRAPH Courses, DOI 10.1145/3084873.3084893 |
| Albedo scaling | Turquin 2019, ILM tech report, blog.selfshadow.com/publications/turquin/ms_comp_final.pdf |
| Rough dielectric | Walter, Marschner, Li, Torrance 2007, "Microfacet Models for Refraction through Rough Surfaces", EGSR (Eq. 20, 21, 33, 34; s5.3 Eq. 38-41) |
| VNDF | Heitz 2018, "Sampling the GGX Distribution of Visible Normals", JCGT 7(4), https://jcgt.org/published/0007/04/01/ |
| Masking-shadowing | Heitz 2014, JCGT 3(2), s3.2 Eq. 72 (height-correlated G2) |
| Multiple-scatter walk | Heitz, Hanika, d'Eon, Dachsbacher 2016, "Multiple-Scattering Microfacet BSDFs with the Smith Model", ACM TOG 35(4), DOI 10.1145/2897824.2925943, https://jo.dreggn.org/home/2016_microfacets.pdf , https://eheitzresearch.wordpress.com/240-2/ |
| LTC / sheen | Heitz 2016 (LTC); Zeltner, Burley, Chiang, SIGGRAPH 2022 Talk; tizian/ltc-sheen (Apache-2.0), table fitting/python/data/ltc_table_sheen_approx.cpp |
| Disney | Burley 2015 course notes (no DOI) |
| Thin film | Belcour and Barla 2017, ACM TOG 36(4) art. 65, DOI 10.1145/3072959.3073620; Gulbrandsen 2014, JCGT (metallic n,k); Born and Wolf s14.4.1; OpenPBR thin-walled case; Kulla and Conty p.40 (thin-glass roughening) |
| Hair | Chiang, Bitterli, Tappan, Burley 2016, CGF 35(2), DOI 10.1145/2775280.2792559; Marschner et al. 2003, ACM TOG 22(3), DOI 10.1145/882262.882345; d'Eon et al. 2011, EGSR/CGF 30(4), DOI 10.1111/j.1467-8659.2011.01976.x; Huang, Wu, Meng, Yan 2022, EGSR/CGF 41(4) (not adopted); Zinke and Weber 2007 (non-goal) |
| Curves | Reshetov 2017 "Phantom Ray-Hair Intersector"; pbrt-v3 src/shapes/curve.cpp (BSD-2-Clause, mmp/pbrt-v3); pbrt-v4 Curve |
| Subsurface | Chiang, Kutz, Burley 2016, SIGGRAPH Talks, DOI 10.1145/2897839.2927433; d'Eon 2016 Hitchhiker's Guide Eq. 53.7; Krivanek and d'Eon 2014 Talks; Meng, Hanika, Dachsbacher 2016, EGSR/CGF 35(4) (Eq. 9/10); d'Eon and Krivanek 2020 Courses (zv2020.pdf, Eq. 67); Henyey-Greenstein 1941 |
| MIS / misc | Veach 1997 thesis (s5.2 eta^2; s9.2.4; Eq. 9.15); Laine, Karras, Aila 2013 (wavefront); Mitsuba 3 chi2.py (BSD-3); Steele-Lea-Flood OOPSLA 2014 (splitmix64); O'Neill 2014 (PCG32) |
| Conductor / layering | Kutz and Hoffman F82-tint; OpenPBR Surface spec (ASWF); adobe/openpbr-bsdf (Apache-2.0, reviewed, not used); Fujii EON |

Reference engine files (as recorded). Cycles v5.2.0 / main; commits cited: a738579c (microfacet),
eaa5f63ba20e64a439af48a1600cb9ed7bf9bdf0 (tables, 2025-07-09). Paths were recorded both as
`intern/cycles/kernel/...` (blender/blender) and `src/kernel/...` (github.com/blender/cycles); pin a
commit before reuse.
- `bsdf_microfacet.h` (BSD-3-Clause per later SPDX check; one note said Apache-2.0): preserve_energy,
  estimate_albedo, VNDF, glass, thin glass, microfacet_fresnel, generalized_schlick_setup,
  adjust_thin_film_ior_at_backface. Other Cycles files Apache-2.0: `bsdf_util.h`
  (closure_layering_weight, fresnel_dielectric_cos, fresnel_iridescence_channel), `svm/closure.h`
  (Principled assembly, melanin, ior_from_F0), `svm/types.h`, `kernel/types.h`, `bsdf_oren_nayar.h`,
  `bsdf_sheen.h`, `bsdf_transparent.h`, `bsdf_principled_hair_chiang.h` (481 lines),
  `lookup_table.h`.
- Tables in `scene/shader.tables`: table_ggx_E 32x32, table_ggx_Eavg 32, ggx_glass_E 16^3,
  ggx_glass_Eavg 16^2, glass inverse tables, ggx_gen_schlick_ior_s 16^3 (4096 entries),
  table_thin_film_cmf[512][6]. Generators: `app/cycles_precompute.cpp`,
  `scene/shader.cpp` (compute_thin_film_table, device_update_common),
  `doc/precompute/thin_film_table.py`.
- Curves: `kernel/geom/curve_intersect.h`, `bvh/util.h` (intersection_skip_self), `bvh/shadow_all.h`,
  `bvh/traversal.h`, `integrator/intersect_closest.h`, `light/sample.h`, `bvh/build.cpp`
  (add_reference_curves), `device/cpu/bvh.h` (Embree filter maps segment to curve),
  catmull_rom_basis_eval.
- SSS: `integrator/subsurface_random_walk.h`, `closure/bssrdf.h`. Lights: `kernel/light/{area,spot,
  point,distant}.h`, `scene/light.cpp:127-131`, `math_base.h` smoothstepf. Graph curve nodes (branch
  blender-v4.5-release): `svm/ramp.h`, `svm/checker.h`, `brick.h`, `noisetex.h`, `magic.h`.
- pbrt-v4 (github.com/mmp/pbrt-v4): `bxdfs.h/.cpp` (DielectricBxDF, HairBxDF, layered), `util/
  scattering.h` (Sample_wm), `math.h` Lerp. Licence recorded as both BSD-3-Clause and Apache-2.0;
  verify before copying code.
- Rejected on licence: Heitz supplemental MicrosurfaceScattering.cpp (unstated), Heitz Mitsuba 0.5/0.6
  plugin (GPLv3); Mitsuba 3 roughdielectric (BSD-3) is single-scatter only.
- Blender PRs: 107958 (Turquin MULTI_GGX, commit 888bdc1), 118477 (thin film), 123345 (EON),
  123616 (physical conductor), 157469 (thin wall).

## Measured numbers worth keeping

- Lambertian furnace baseline 0.985. Cycles MULTI_GGX glass furnace 0.951-1.001; port within 0.008 of
  it over 15 IOR x roughness rows; pdf integrates to kept fraction within 0.001.
- Rough glass furnace after fixes (ior 1.5, r 0.05/0.1/0.3/0.6/1.0): 0.997-0.999 (band [0.92,1.03]).
  Single-scatter plus compensation at r1.0: 0.903 CPU (single scatter alone 0.717), 0.930 GPU.
  Walk: CPU 0.992-1.000; GPU stub 0.9958/0.9688/0.9612/0.9570 at r0.2/0.5/0.85/1.0.
- Cycles glass factor 1+Fms(1-E)/E stays 1.000-1.031 at ior 1.5 (max 1.0306, r1.0, mu0.3): it cannot
  close a 1.2-11x deficit; if furnace is outside [0.92,1.03] the cause is not microfacet multiscatter.
- Realized fallback rate (%), r by theta 0/30/45/60/75: r0.3 0.086/0.110/0.153/0.360/1.265; r0.6
  1.266/1.408/1.774/2.382/2.532; r1.0 7.026/5.529/4.839/4.088/2.789. The raw un-gated below-horizon
  VNDF reflection rate is ~20-23% at r1.0: a different quantity.
- Single-interface flux efficiency 0.77-1.00 uncompensated (r0.85: 0.773 @75, 0.825 @60, 0.944 @0;
  r0.5 @75 0.924): one interface cannot explain a 2x deficit.
- Metal furnace (albedo 1) r0.15/0.3/0.6/0.9: 1.643/1.253/1.407/1.769 before, 0.882/0.851/0.809/0.880
  after a Kulla-Conty replacement. Spectral vs RGB at r0.9: chromatic 1.0743 vs neutral 1.0074 (RGB
  upsampling non-linear in f*darken(f)). Metal reflection vs Cycles 5.1 (64 px, 128 spp): 12 legs
  within [0.85,1.15], GPU within ~1% of CPU. Worst Disney reflectance 1.0159 (gate 1.02); clearcoat
  1.0206 unresolved.
- Monte Carlo albedo oracle (ior 1.5, f0 0.04, 300k samples) r/mu, MC vs old term: 0.30/0.20 0.2480 vs
  0.3195; 0.50/0.20 0.1341 vs 0.3035; 0.85/0.20 0.0574 vs 0.2660; 1.00/0.20 0.0435 vs 0.2386; all agree
  at mu 1. Cycles table 0.85/0.20: 0.082 (diffuse weight 0.734 to 0.918). ggx_E Eavg: r0.30 0.9763,
  0.50 0.8820, 0.85 0.5527, 1.00 0.4091.
- Rough glass sphere, engine/Cycles centre/limb (ior 1.45): r0 0.963/0.815, r0.2 0.948/0.666, r0.5
  0.873/0.490, r0.85 0.742/0.556 (after the dead-sample reroute). Limb/centre: Cycles
  1.60/1.75/1.96/1.74, engine 1.36/1.23/1.10/1.30. Cycles seed noise <= 1.4%. Walk vs Cycles centre
  1.110/1.635/1.717 at r0.5/0.85/1.0; GPU single scatter 0.994/0.926/0.891; shape RMSE at r0.5 walk
  0.123, GPU 0.188, Cycles GGX 0.221. Cycles BSDF-only vs engine RMSE 0.033/0.037, Cycles default
  0.23/0.45. Cycles' indirect clamp (10) cut the lamp term to 12-43%; direct light sampling type had no
  effect in 5.2.
- Thin film vs Cycles 5.2 CPU (128 px, 256 spp, thickness 100-1000 nm x film ior 1.2/1.5/1.8):
  dielectric 18/18 in [0.85,1.15], max |ratio-1| 8.96%, hue mean 6.0 deg (max 13.4), max dE2000 2.42;
  conductor 18/18, max 2.09%, hue mean 10.1 (max 25.4), dE2000 1.39. Analytic phase vs exact Airy
  ~1e-6.
- Hair: single fibre vs Cycles 1.006/1.000/1.005; melanin tuft whole-frame 0.786 to 1.002; corpus tuft
  0.997/0.939/0.935/0.954 to 0.980/0.973/0.972/0.975. Strand self-skip image-sum ratios: bent 0.33 to
  1.00, front-lit 1.19 to 1.01, hairpin 1.37 to 1.00, behind-lit TT 1.00; corpus tuft 0.9705 to 0.9762.
  Residual ~2.1% under Cycles thick mode. I0 series with 10 terms gives 0.987 at h=0.9 vs 1.000 with 40.
- SSS walk: furnace 1.0000; absorption throughput 0.500/0.841/0.992 at albedo 0.3/0.6/0.9;
  transmittance 0.798/0.483/0.179 at 0.5/2/8 mfp (R+T=1); gray variance classic 2e-5 vs Dwivedi 7e-5;
  Dwivedi on a coloured medium (10.7, 5.6, 1.3), max walk ~8e4; guided fraction 1 - max(0.5, |g|^0.125).
- Sample/pdf harness: diffuse live fraction 0.7514 vs 0.750; Lambertian chi2 818.06, dof 789, p 0.23;
  glass chi2 143,140,779 to 34,988 after the sampler fix, ~90% of the residual an integration
  resolution artifact (ires 4 to 8: 35107 to 3942; use ires >= 8).
- Glass tint centre ratio 1/Color (1.11, 1.00, 1.05) vs measured (1.119, 0.9985, 1.057).

## Dead ends and negative results

- Kulla-Conty table for rough transmission as fix for the furnace deficit: rejected (deficit worst at
  low roughness, wrong direction; a calibrated table moved r0.1 by +0.003). Real causes were
  orientation, a clamp and the eta^2 clip. Transmission multiscatter as such was never confirmed.
- Removing the rough-to-smooth fall-through collapsed furnace to 0.000; returning dead samples
  pbrt-style also ~0.0. Wo-facing VNDF frame: bit-identical (no-op).
- Bounce caps (12 vs 64), filter_glossy, Russian roulette moved rough-glass ratio < 0.5% at r0.85.
  Depth sweep 0.749-0.768 (depth 4-256) pointed to a per-bounce leak.
- Dead-sample redistribution is not the mechanism of the limb deficit (dead fraction only 0-4%); the
  "3x dead-sample regression" (7.1% vs 22.9%) was a gated-vs-raw measurement artifact.
- Forcing reflection candidates to fail worsened furnace (0.085 at r0.1); a second |cosI| and a
  firefly clamp at 1e6 changed nothing.
- Direction-only Dwivedi guiding for coloured media (needs joint distance stretching); backward
  guiding deferred.
- Per-NEE stochastic eval with a thread-local RNG: rejected (impure, signature churn, extra walk per
  NEE). A thin-film-aware walk was never built.
- Closed-form layering albedo in place of Cycles' 16^3 table: rejected (invention risk).
- A 44% central ROI gave a spurious thin-film fail on a dim dielectric (0.59 vs 0.959); whole-frame
  ROI fixed it. ROI over a specular highlight is dominated by the highlight.
- A uniform-Halton reflectance integrator was measurement-broken; use an importance-sampled estimate
  (pbrt-v4 s14.1.6). Re-tuning a fudge scale against a broken integrator is a trap.
- Metal compensation with Fss = albedo instead of Cycles' mix(f0,1,1/21) (exact: albedo +
  (1-albedo)/21) was chosen to share one compensation; centre MSE 0.02474 to 0.00353.

## Design ideas worth reconsidering in the rewrite

- Build Principled as OpenPBR directly (EON diffuse, dielectric specular, F82-tint metal,
  transmission, coat, fuzz/sheen, thin film, emission, subsurface), layered by albedo scaling with the
  exact scalar albedo-times-lobe-energy form.
- Be spectrally native: evaluate Fresnel, thin film and hair absorption per wavelength, upsample only
  colours, and factor every scalar above 1 (eta^2, compensation) out of upsampled colour. RGB-Disney
  shortcuts caused three separate bug classes in v1.
- For rough dielectrics, prefer the Heitz random walk as the physics reference and keep 1/E scaling
  (Cycles/Turquin) as a cheaper tier; pdf reported by sample() must be the mixture pdf. Decide the
  default under "physics first".
- Bake energy tables with the renderer's own G and sampler, or adopt Cycles' height-correlated
  Smith + VNDF so Cycles' tables (or the generator) can be reused.
- Orientation as a stored hit flag, checked to survive every transform/instance.
- One shared body for thin film (analytic sensitivity, m=3), film scaling eval only, with a
  thickness cutoff.
- Subsurface as an integrator-level walk with an object-local intersector; RGB walk, channel-MIS
  default, Dwivedi opt-in for gray media.
- Hair: Chiang model with per-wavelength sigma_a; whole-curve self-skip; thick/ribbon modes explicit.
- Single place for lobe selection weights shared by eval and pdf (avoids the 2x gain bug).
- Standing gates: linear furnace per material, importance-sampled albedo oracle, sample/pdf
  chi-squared at ires >= 8 plus peak-position checks, orientation/transform furnace.
- Consider Cycles' separate transparent bounce counter for Blender-compatible alpha.

## What no longer applies

- Matching Cycles' invalid-side refraction rim through NEE (a Cycles quirk).
- Tuning to Cycles' per-RGB thin-film CIE LUT or its RGB projection.
- Color^2 base-colour encoding for glass tint; Cycles-style per-RGB hair sigma_a.
- Pixel-parity chasing of Cycles rough-glass limb brightness at r >= 0.85.
- v1 GPU register/stack budgeting, inlined principled paths, compile-time isolation of principled code.
- Lerp lowering of Mix(Principled,Principled) and v1 closure-slot limits.
- Disney-to-Principled compatibility, the dead-sample reroute to smooth glass, and Disney-specific
  energy tables.
- Quarantined measured+0.03 gate bands for grazing configurations.

## Open questions

- Cycles bsdf_microfacet.h licence was recorded as Apache-2.0 in one note and BSD-3-Clause in others
  (SPDX check 2026-07-24: BSD-3-Clause); pbrt-v4 as BSD-3-Clause and Apache-2.0. Verify per file.
- Cycles source path inconsistency (src/kernel vs intern/cycles/kernel); pin a commit.
- Meaning of Emission Strength inside glass (basic radiance vs medium radiance): the 1/eta^2 factor
  put emission inside glass at 0.476 of Cycles.
- Physics (walk) versus Cycles (1/E) as default rough-glass multiscatter; centre brightness 1.1-1.7x
  Cycles is recorded as physical.
- Clearcoat energy (1.0206) and sheen target (LTC vs Kulla-Conty) were unresolved.
- A device implementation of the walk (register pressure; keep behind a runtime flag) was designed
  but never built; a thin-film-aware walk was never built.
- Transmissive soap-bubble parity was untested; rough glass with |IOR-1| < 1e-4 takes mirror film
  reflection while Cycles keeps GGX film reflection.
- Per-wavelength conductor film refinement (<= 10-25 deg hue at dE < 1.4) judged low priority; no
  spectral ground truth was compared.
- Whether the Turquin and Kulla-Conty forms differ on real scenes (never measured to a distinction).
- RGB illuminant (D65-weighted) versus unbounded upsampling for emission was unresolved.
- The digests are thin on: transmission multiscatter beyond the single-scatter closures, GPU emission
  from emissive geometry, and anisotropy beyond reflect lobes.
