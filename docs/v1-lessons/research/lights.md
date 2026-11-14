# Lights, sky and environment

Carried over from the v1 renderer's research: sky models, sun disc, IES, light
tree, HDRI/environment sampling, light energy calibration. No v1 code names
appear. "Cycles" is cited as knowledge of Blender semantics (what an artist's
scene means in Blender) and as a source of reference algorithms, not as a
pixel-parity target. Where v1's notes were thin, this file says so.

## Established findings

### Sky models

- **Preetham/Perez was v1's first sky**: `F(theta, gamma) = (1 + A exp(B/cos theta))
  (1 + C exp(D gamma) + E cos^2 gamma)`, coefficients A-E linear in turbidity for Y, x, y.
  Zenith `Yz = 1000((4.0453 T - 4.9710) tan(chi) - 0.2155 T + 2.4192)`,
  `chi = (4/9 - T/120)(pi - 2 theta_s)`; zenith chromaticity cubic in sun zenith
  angle. Chosen because a permissively licensed reference existed. It has no
  sun disc and no multiple scattering; ground albedo and horizon differences
  were the dominant error against Blender's Nishita sky.
- **Mapping Blender's sky node onto single turbidity is lossy.** v1 used
  `T = clamp(2 + 2*aerosol, 1.7, 10)`. Air density was deliberately not folded
  in: more air makes Nishita bluer but higher turbidity means more haze, so the
  mapping would invert. Unhonoured sockets must be named in a visible warning.
  The aerosol map `0.10 + 0.045*(T-2)` is a fitted constant, not from a paper.
- **Nishita licensing**: an early note treated all Cycles sky code as GPL; later
  corrected: single scattering Apache-2.0, multiple scattering MIT, shader-side
  sky evaluation Apache-2.0; only the sky math helper and the Nishita header
  are GPL. Check licences per file.
- **A bake plus a bridge constant is the wrong architecture.** v1 baked an
  equirect sky and multiplied by 1/1766, which decomposes as `(1/683)*(1/2.586)`:
  683 lm/W is CIE 018:2019 luminous efficacy, but 2.586 is a Cycles Nishita
  units choice, not derivable from physics. One constant cannot match across
  models or elevations (Nishita/Preetham ratio: Nishita 1.22 at 10 deg, 0.90
  at 28, 0.56 at 60; Preetham 7.5 to 15). Refitting on another statistic moved
  it (1/1333 on full-sky mean; 1/120 was 14.7x too bright). Conclusion:
  evaluate a spectral sky in the engine so exposure follows from physics.
  Per-bake zenith normalisation was rejected: it flattens correct haze/sun behaviour.

### Sun disc

- **Dedicated distant light, not disc pixels**: a 0.53 deg disc is ~1.5 px in a
  1024x512 equirect.
- **Irradiance must not depend on the artist's sun size.** v1: reference solid
  angle fixed to the physical sun (0.545 deg, ~6.8e-5 sr),
  `E_sun = L_sun0 * Omega_ref * intensity`, `L_sun0 ~ 1.6e9 cd/m^2`; `sun_size`
  only sets apparent size and penumbra. Cycles confirmed (luminance ratio 1.001
  for 0.545 vs 2.2 deg). Scaling by disc solid angle over-brightened large
  discs (direct:diffuse 61:1 vs Cycles 6.4:1).
- Sun colour: sky at the sun direction normalised to unit luminance. Analytic
  E_sun at elev 30 deg, size 0.009512 rad: RGB [186.8, 153.2, 109.0] (v1 units).
- **Limb darkening and colour variation** (Cycles shader sky, Apache-2.0):
  `y = (dir_elev - sun_elev)/diam + 0.5`; `limb = 1 - 0.6(1 - sqrt(1 - (d/half)^2))`;
  radiance `= lerp(bottom, top, y) * sun_intensity * limb`. At 4 deg elevation
  the lower limb is 2-3x dimmer and redder; a uniform disc read 1.16/1.28/1.58
  (r/g/b) on the lower disc. With the profile in both camera evaluation and NEE
  it read 0.9999/0.9990/0.9963; uniform NEE alone biased ~1.3% low under MIS.
- **Camera-visible**: v1 skipped lights for camera rays so the disc vanished;
  make the sky sun (only) visible.
- **Azimuth**: Blender world sun azimuth = 90 deg - sun_rotation, a reflection,
  not the +90 rotation a reading of the shader predicted. Direction toward sun
  `(cosE sinR, cosE cosR, sinE)`. Verified: pole-shadow azimuth within 4 deg at
  rotations 0/90/225; toward-sun luminance ratio 0.40 -> 0.993 (rot 0), 1.19 ->
  0.993 (rot 60), anti-sun 0.84 -> 0.998. A mirrored-shadow comparison had
  produced a spurious "1.48x over-bright".

### Environment (HDRI) sampling

- **Estimator**: standard infinite-area-light NEE (PBRT-4e sec 12.5; Cycles'
  background code is the same): piecewise-constant 2D distribution over lat-long
  texels weighted by luminance x sin(theta); sample jittered inside the texel;
  radiance is the bilinear lookup at the sampled direction so it matches a
  missed ray. Solid-angle pdf `pdf_uv / (2 pi^2 sin theta)`. No extra NdotL when
  the BSDF evaluation includes the cosine. Power-heuristic MIS (beta 2) on both
  strategies; the miss leg unweighted after a camera or specular bounce.
  No gamma on HDRI load.
- **Missing environment NEE was the whole "HDRI too dark" gap.** Background ROI
  0.047 vs 0.121 was first blamed on Cycles' importance sampling inflating its
  background (withdrawn). With env NEE: ROI 0.112 (Cycles 0.1207), sky strip
  0.1826 vs 0.1829. Constant 0.5 world gave 0.4995; strength sweeps linear.
- **Azimuth bug class**: azimuth computed in pixel units without dividing by
  width puts all samples near phi = 0. Test `pdf(sample.dir) == sample.pdf` to 1e-4.
- **Cycles lookup conventions**: `u = (pi - atan2(dy,dx))/2pi`, `v = 1 - acos(dz)/pi`;
  texel centres at `u*W - 0.5`, bilinear, no mip; the CDF is for sampling only.
  Mapping rotation is XYZ extrinsic (`R = Rz Ry Rx`); Blender-to-engine swap
  `(x,y,z) -> (x,z,-y)`; Background colour tint multiplies the lookup.
- **Chrome-sphere HDRI reflection is a good integrator check.** A "23% darker"
  report was ruled out rung by rung (lookup, sRGB-on-load, mip blur, spectral
  upsample, F82 Fresnel, ground illumination, CPU vs GPU). Residual: green-biased
  ~+11% G from spectral vs RGB pipelines.

### Light tree

- **Reference**: Conty Estevez and Kulla 2018 (SAOH). Importance
  `~ energy/d^2 * cos(min outgoing angle)` with cone `(theta_o, theta_e)`;
  prune when `theta_i - theta_u > theta_o + theta_e + pi/2`. SAOH cost =
  M(left)+M(right), 12 buckets per axis, force leaf if min cost >= parent cost.
- **Per-type cones**: area theta_o 0, theta_e half the spread (cap pi/2);
  point/distant full sphere; spot from cone; single-sided mesh theta_o 0,
  theta_e pi/2. Energy = on-axis intensity (area P/pi, point/spot P/4pi, spot
  P/solid angle).
- **Seam fixes** (each removed a tree vs non-tree discontinuity): MIS pdf for
  the BSDF-hit strategy uses the previous vertex's normal; importance distance
  clamp `max(0.5|centroid - bbox.max|, d)`; triangle pdf uses geometric normal
  `t^2/(|dir.Ng|*A)`; leaf selection with min/max importance bounds; distant
  lights in one leaf; always flag transmission so back-side light is not pruned.
- **Sun starved by emitters**: with a dedicated sun (power 6.6e-5 of total) and
  mesh emitters the GPU tree never picked the sun (CPU tree 0.195, CPU power
  0.006, GPU 0.005 mean). The tree must carry every light type.
- **Power-based selection was insertion-order dependent** (3-4x dark at 256 spp
  when a dedicated light preceded a mesh).
- Gates used: >=2x variance reduction at 64 area lights / 256 spp; <=0.5 dB PSNR
  on a Cornell box.

### Light types and energy calibration (Blender semantics)

- **Area**: radiance `P/(pi*A)`; solid-angle pdf `(1/area)*d^2/cos`. Mixing an
  area-measure pdf into the MIS sum gave 0.13x (large) to 1.40x (small, far).
- **Point**: intensity `P/(4pi)` (`1/pi` was 3.59x too bright). **Spot**: same;
  radius-0 point/spot is a delta light (pdf 1, not `1/coneSolidAngle`).
- **Radius > 0 point/spot**: emission must be consistent with the sphere's area
  measure; `1/(4 pi r^2)` pdf with point-intensity emission scaled brightness by
  `4 pi r^2` (0.125x at r=0.1). After the fix all legs totalled 1.0000.
- **Distant**: `emission = eval*I/Omega`, pdf `1/Omega`.
- **Blackbody**: normalise to unit photopic luminance (un-normalised was 14.4x
  too bright; Y_planck(6500 K) ~ 3.6).
- **Area spread**: Blender `spread` is the full angle, Cycles uses half. Cycles
  soft-box attenuation `f(a) = max(tan h - tan a, 0)*N`, `N = 1/(tan h - h)`,
  power-preserving (`pi L A N (tan h - h) = pi L A`, checked 1.0000); on-axis
  boost 4.66 at 45 deg, 19.3 at 22.5 deg. As h -> 0 treat as collimated:
  uniform area sampling hits the axis with measure zero so the lamp otherwise
  renders black. A hard-clipped cone gave 0.17x. (`area_light_spread_clamp_light`
  is variance-only; not ported.)
- **Delta lights and MIS**: lights BSDF rays cannot hit take NEE weight 1 and
  BSDF-strategy pdf 0. Weighting them as hittable cost ~9% (one light) and up to
  60% (N=1/2/4 equal lights: 0.910/0.722/0.397).
- **Lamp visibility to BSDF rays** (Cycles): area lamps hittable by BSDF rays,
  not camera rays; one-sided reject `dot(D, Ng) >= 0`; rectangle/ellipse bounds.
  Without it NEE pays the MIS complement and lamp reflections go near-black.
  BSDF-hit pdf must be solid-angle `t^2/(area*cos)`. Point/spot not hittable;
  their mirror reflections are a documented limitation.
- **Lamp pass-through**: the ray continues past a lamp after MIS-weighted
  emission (the lamp is not a path vertex); cap 4 lamps per segment. In media
  the lamp term is deterministic `throughput*Tr(t_lamp)*Le*w_B` with free flight
  over `[0, t_surface)`, an unbiased split. Two collinear area lamps NEE on/off
  agreed to 0.4% (was 8%).
- **Emission-hit clamping**: Cycles clamps emission hits with index `bounce - 1`;
  using `bounce` clamped first-hit direct light as indirect, non-linearly (900 W
  lamp 0.73, 100 W 0.93). Fixed: volume scene 0.75-0.81 -> 0.989/0.989/0.986.
- **Emission with alpha**: `weight *= alpha`, emission scaled by alpha, path
  continues through the BSDF after emission. One alpha factor per emission site;
  alpha-scaled light-sampling power is a deliberate unbiased deviation (only the
  selection pdf differs; Cycles' estimate is not alpha-scaled).
- **Lights need**: no emissive-sphere substitute for point lamps, orientation
  cones, per-light shadow/MIS/caustic/max-bounce switches, spectral emission.

### IES

- **Honour absolute candela** (owner decision; peak-normalisation reversed).
  Cycles: `factor = multiplier*ballast*4pi/177.83` (0.0706650768394), no peak
  normalisation, node Strength multiplies. A flat 1-cd IES = no-IES x `4pi/177.83`.
- Cycles interpolates Catmull-Rom, vertical then horizontal, on its packed
  table; linear was up to 27% off near troughs.
- Frame from the lamp's world-matrix columns; wrong frame gave azimuth sectors
  0.29-3.0 (spot) and 0.42-9.4 (point).
- Float32 quirk: `2pi - 1e-7f == 2pi`, so Cycles' horizontal wrap test is false
  for 0-360 tables; ignoring it put a reference 1.8% off at 330-360 deg.
- Spot cone: Cycles blends in cosine space; angle-space blending moved falloff
  start (25.5 vs 27.6 deg). Raster `(res-1)` vs `res` divisor: ~0.5 px shift.
- LM-63: version line, optional `[KEYWORD]` lines, `TILT=NONE`, 10-field
  photometric line (lumens_per_lamp -1 = absolute), ballast line, vertical
  angles, horizontal angles, candela matrix (row per horizontal plane). A
  synthetic test profile: 19 vertical (0-180, 10 deg), 8 horizontal (0-315,
  45 deg), Gaussian vertical lobe peak ~65 deg, two-term cosine azimuth series;
  embedded as an INTERNAL text datablock so the .blend is self-contained.

## Sources and reference implementations

| Source | Used for |
|---|---|
| Preetham, Shirley, Smits 1999, SIGGRAPH (sec A.3, A.6) | Perez sky coefficients, zenith values |
| Perez, Seals, Michalsky 1993, Solar Energy | Sky luminance distribution |
| CIE 018:2019 | K_m = 683 lm/W |
| Bird and Riordan 1986 (SPCTRAL2); IES Lighting Handbook | Solar spectrum; solar-disc luminance |
| Hestroffer and Magnan 1998 | Limb darkening (noted, not used) |
| Hosek and Wilkie sky model (BSD-3) | Evaluated, deferred |
| appleseed `preethamenvironmentedf.cpp` (MIT) | Preetham reference |
| Cycles `intern/sky/source/sky_single_scattering.cpp` (Apache-2.0), `sky_multiple_scattering.cpp` (MIT), `intern/cycles/kernel/svm/sky.h` (Apache-2.0; `sky_radiance_nishita`), `util/math_float3.h`; GPL only for sky math helper and Nishita header | Nishita sky, disc profile, azimuth |
| Cycles `kernel/light/distant.h`; PBRT-v4 `DistantLight` (Apache-2.0) | Distant light (distant.h returned 404 at fetch) |
| Cycles `intern/cycles/blender/shader.cpp`, `scene/light.cpp` (`device_update_background`), `kernel/light/background.h` (Apache-2.0) | World node conversion, background CDF |
| Cycles `scene/light.h`, `kernel/light/*.h`, `scene/light_tree.*` (Apache-2.0) | Light interface and tree |
| Cycles `src/scene/light.cpp` (`light_normalize_factor`), `src/kernel/light/area.h`, `point.h`, `spot.h`, `kernel/svm/svm_blackbody.h` (Apache-2.0) | Wattage normalisation |
| Cycles `kernel/light/area.h` `area_light_spread_attenuation`; Blender D10594 | Area spread |
| Cycles `kernel/light/light.h` `lights_intersect`, `kernel/light/area.h` `area_light_intersect` | Lamp BSDF-ray visibility |
| Cycles `kernel/integrator/shade_light.h` (`integrator_shade_light`) | Lamp pass-through |
| Cycles `kernel/film/light_passes.h` (`film_clamp_light`) | Emission-hit clamp index |
| Cycles v5.2.0 `kernel/svm/closure.h`, `shade_surface.h`, `scene/shader.cpp` (`output_estimate_emission`) | Emission alpha |
| Cycles (Blender 5.2, Apache-2.0) `util/ies.cpp` (`IESFile::parse`, `process_type_c`, `process`), `kernel/util/ies.h` (`kernel_ies_interp`, `interpolate_ies_vertical`), `util/math_base.h` (`cubic_interp`, `smoothstepf`, `inverse_lerp`), `kernel/svm/ies.h`, `scene/shader_nodes.cpp`, `scene/shader_graph.cpp`, `kernel/svm/vector_transform.h`, `kernel/light/spot.h` (`spot_light_attenuation`), `scene/light.cpp` (`copy_to_kernel`), `blender/light.cpp`, `kernel/light/point.h` (`point_light_sample`), `kernel/sample/mapping.h`, `kernel/light/sample.h` | IES, spot cone, point sampling |
| Cycles `kernel/svm/svm_image.h` | Image texel convention |
| Cycles `kernel/light/tree.h` (`light_tree_importance`, `light_tree_cos_min_incoming_angle`, `light_tree_cluster_select_emitter`), `scene/light_tree.cpp` (`recursive_build`, `LightTreeMeasure::calculate`), commit e52e5eb06f6b24055f0e7508bc7d7278e139ba0f; Blender PRs #105862, #106683 | Light tree |
| Conty Estevez and Kulla, "Importance Sampling of Many Lights with Adaptive Tree Splitting", ACM CGIT 1(2):25 (2018), DOI 10.1145/3233305 (HPG 2018; sec 4.4) | Light tree, SAOH |
| PBRT-v4 `src/pbrt/lightsamplers.cpp` (`BVHLightSampler`), `lights.h`, `src/pbrt/lights.cpp`, `util/sampling.h` (`ImageInfiniteLight`, `PiecewiseConstant2D`) (Apache-2.0); PBR 4th ed. sec 12.5, 12.6 | Tree cross-check, light interface, env NEE |
| Veach and Guibas 1995; Veach 1997 sec 9.2 | MIS, power heuristic |
| ANSI/IES LM-63-2019; ANSI/IES TM-30-20; stb_image `stbi_loadf` | IES format; colour fidelity; RGBE loading |
| https://www.rastro.ai/resources/glossary/ies-file-format-every-line-explained-lm-63-2002 | LM-63-2002 line by line |
| https://blog.ansi.org/ansi/standard-file-photometric-data-ies-lm-63-19/ | LM-63 overview |
| https://docs.agi32.com/PhotometricToolbox/Content/Open_Tool/iesna_lm-63_format.htm | AGi32 LM-63 format |
| https://docs.blender.org/api/current/bpy.types.ShaderNodeTexIES.html | Blender IES node |

Caveats recorded by v1: the light-tree importance formula was inferred from
Cycles source because the paper PDF was blocked, and the `cos_min` expression is
an unverified paraphrase. A patent search note (nothing encumbering SAOH,
2026-05-24) is the note's own claim, not independently checked. Apache-2.0
reference code into an MIT project is compatible; cite at each call site.

## Measured numbers worth keeping

- Sky band residual after the Nishita fit: 15-26% at low sun, within +-5% at
  high sun. Sunlit/shadow ratio 1.87 (v1) vs 2.06 (Cycles); sunlit radiance
  0.96x. Direct:diffuse 1.8:1 (v1, physics-first) vs 6.4:1 (Cycles).
- Disc centre radiance in Cycles 5.2 (MS sky, elev 4 deg): (2.19e5, 1.01e5,
  1.70e4); uniform-disc v1 (1.77e5, 8.1e4, 1.4e4).
- IES, Cycles 5.2 vs numpy reference: total 1.0000, annuli 0.9999-1.0002.
  Spot annuli 26-29 deg before the cosine-space fix: 0.78/0.53/0.34/0.25.
  Radius-point totals before fix at r = 0, 0.05, 0.1, 0.25, 1.0: 0.930, 0.0315,
  0.126, 0.760, 1.347.
- Lamp-shader fold, ratio before -> after: spot floor pool 0.019/0.020/0.024 ->
  1.097/1.087/1.041; spot backdrop 0.587/0.608/0.670 -> 0.890/0.882/0.862;
  point backdrop 0.672/0.671/0.760 -> 0.961/0.927/0.956; sun 1.016/0.994/0.980
  -> 0.980/0.974/0.977.
- Env lookup: CPU bilinear vs numpy < 1e-3; GPU vs CPU RGB worst relative
  2.25e-7, spectral upsample 4.94e-6; NEE on/off at roughness 0: 1.0000, 0.05:
  0.9999-1.0001, 0.2: 0.974-0.978 (MIS effect). Chrome sphere per-pixel median
  vs Cycles R 0.964, G 1.111, B 1.018 (p10-p90 0.89-1.08).
- Light tree: NEE-on/off 118x on a 5e-5 m^2 emitter triangle before seam fixes;
  tree/power ratio on a medium 0.19 -> 1.0, translucent plane 0.016 -> 1.0.
  One scene's floor lit by key 65%, world 18%, fill 15%. Cycles' own tree
  on/off: wall 0.48, floor 0.12.
- Emission clamp: volume scene with clamp off in both engines 0.991/0.990/0.994;
  default clamp at 4 volume bounces 0.92/0.92/0.91.
- A leaked fallback sky gradient in a scene meant to be black added 0.057-0.11
  per channel, type-independent, stable across 32 vs 512 spp (deterministic).

## Dead ends and negative results

- "Cycles inflates its own background via importance sampling": wrong; the cause
  was missing env NEE. Disabling Cycles' sampling method halving the gap was an
  artefact.
- Rotating the sky bake to fix the sun azimuth: moved the glow to the wrong
  side; the loader's coordinate swap already mapped azimuth. Fix the direction.
- Scaling sun irradiance by disc solid angle; one fitted exposure constant for
  all sky models/elevations.
- Fixed-probability (50%) environment-vs-lamp selection: rejected for additive
  MIS strategies (splitting raises variance with lamp plus HDRI).
- Incidence-based pruning in the light tree: lost delta lights on the
  transmission side.
- Peak-normalised IES: reversed; the "energy ~3x off" premise was stale.
- Chrome HDRI gap hypotheses all ruled out: lookup, sRGB-on-load, mip blur,
  spectral upsample, F82 Fresnel, ground illumination.
- Mean ratios over specular highlights: a hot sun reflection or one caustic
  firefly dominates ROI means; use medians and look at the image.
- Retuning emitters for an "equal wattage" 1.07-1.16x offset: it was the leaked
  fallback sky. Don't retune speculatively.
- A lighting-looking darkness that was the BSDF: a ground strip 7-17% dark
  under both lamp and env NEE was ~6-7% diffuse energy loss at roughness 0.85 in
  the layered BSDF, independent of light type (suspected per-channel (1-albedo)
  layering weight vs Cycles' scalar max-channel form; not numerically
  confirmed). Test with a plain diffuse and a sun lamp before blaming lights.
  Self-referential test floors (0.85 at roughness 0.5) may encode the bug.
- Harness: Blender's pixel buffer is bottom-up; a "top" ROI measured the ground
  (sky ground truth 0.0338 vs correct 0.184).

## Design ideas worth reconsidering in the rewrite

- **Spectral sky in the engine** with physically derived exposure (683 lm/W,
  solar spectrum); candidates Nishita (per-file licences) and Hosek-Wilkie.
- **Dedicated sun light** with physical solid angle, camera-visible, one
  limb-darkening profile for NEE and camera/BSDF evaluation.
- **First-class light interface** (sample, pdf, power, orientation cone, bounds;
  per-light shadow/MIS/caustic/bounce switches; spectral emission via blackbody,
  tabulated SPD, RGB). Gates v1 proposed: blackbody 6500 K vs D65 within 1%, IES
  within 5%, spot cos^2/r^2, radius-0 point shadows, ReSTIR target-weight
  consistency within 1% over 1000 samples.
- **Environment as a light inside the light sampler** (one selection scheme for
  lamps, mesh emitters, HDRI) instead of two additive strategies; weigh variance
  in lamp-plus-HDRI scenes.
- **One light tree for CPU and GPU** including distant, delta and environment
  lights; v1 hand-mirrored and had a GPU reverse-pdf proxy mismatch, treated
  mesh emitters as full-sphere, and used max-only share for inner nodes where
  Cycles averages. Adaptive SAOH rather than median split.
- **One IES evaluator** on host and device (Cycles packed layout).
- Alpha-weighted emission once per site, allowed on any leaf material type.
- **Test oracles**: constant-colour world, furnace, `pdf(dir) == sample.pdf`,
  NEE on/off equality, analytic spot/point annuli, chrome-sphere medians.

## What no longer applies

- Matching Cycles' direct:diffuse ratio of the Nishita sky (units choice).
- The 1/1766 and 1/1333 sky bridge constants and fits to them.
- Cycles hiding the below-horizon part of the sun disc (GPL-only source).
- Reproducing Cycles' float32 IES wrap quirk bit-for-bit (keep only if a Blender
  parity requirement is chosen).
- Re-deriving the Principled roughness-0.85 floor against Cycles (material
  research, not lights).
- RGBIlluminant vs RGBUnbounded emission residual (~10%): Cycles-RGB artefact.
- Matching Cycles' tree on/off ratios as a gate; use variance and unbiasedness
  against analytic references.

## Open questions

- Additive environment-plus-lamp MIS, or single energy-weighted selection?
- Half-texel offset in the environment lookup (v1 floored `u*W`, Cycles
  `u*W - 0.5`; under 1e-3 on smooth HDRIs): choose explicitly.
- Cause of the ~+11% green residual on chrome under an HDRI (spectral
  upsampling vs Cycles RGB): unresolved.
- Untested residual candidates for the HDRI gap: firefly clamp on sun-texel NEE;
  point pdf vs bilinear radiance at a sun texel; indirect from hair. Does an
  environment-resolution sweep (256/1024/4096) change the HDRI
  background gap? Not run.
- Blackbody default: normalise to unit luminance whenever set, or RGB always?
  (owner question left unresolved).
- Goniometric/projection lights; triangle-light unification; IES photometric
  types A/B (v1 read them as C).
- Limb-darkening accuracy: the Nishita 0.6 formula is a model, not a
  measurement; Hestroffer and Magnan 1998 unused.
- Transparent-bounce counting: v1 counted transparent hits as bounces where
  Cycles does not, so a lamp behind glass could appear one bounce early.
- Clamping emission under spectral transport (hero-wavelength spikes in
  multi-scatter media; XYZ-Y vs Cycles RGB-sum metric).
- ReSTIR-style direct lighting with dedicated lights (v1's first-bounce path
  passed none, so the sun disc was invisible there).
- Turbidity/aerosol mapping for sky models lacking direct aerosol/air-density
  inputs: v1's is a fitted heuristic.
