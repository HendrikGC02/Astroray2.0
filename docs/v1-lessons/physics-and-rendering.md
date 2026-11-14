# v1 lessons: physics and rendering

Rules learned the hard way in Astroray 1.0 that hold for any physically based spectral renderer. Each bullet gives the rule, then the reason or the measured failure. Numbers are v1 measurements, kept as fingerprints for recognising the same bug.

## Design consequences

- v1 kept hand-written CPU and GPU twins of every material, light and integrator. A large share of its bugs were one backend drifting from the other: a glass material routed to a different GPU code path than its name suggested, one backend synthesising fallback UVs while the other did not, NEE enabled from the integrator's name on one side and from a parameter on the other, a CPU photon loop using the oriented normal while the GPU used the geometric one, lamps spectral on GPU while emissive geometry was RGB-upsampled. The rewrite compiles each formula once for every target (CLAUDE.md rule 6), so this class cannot exist. What remains below are the physics mistakes themselves.
- v1 used special sentinel values (seed 0 meaning "random", distance 1e30 meaning "no occluder distance") that later code read as ordinary numbers. Represent "absent" in the type, not with a magic value.

## Estimators and transport

- Pick one convention for whether a BSDF evaluation includes the cosine term, write it in the interface, and never apply the cosine again in callers. Double-counting the cosine was a repeated v1 failure.
- Emission hit by a BSDF-sampled ray must be MIS-weighted against NEE, or (without MIS) counted only on the camera ray and after a delta bounce. Otherwise direct light is counted twice.
- A mean that moves when you switch between two unbiased strategies (NEE+MIS vs BSDF-only, guiding on vs off) is an inconsistency, not variance. Rough glass moved a ratio from 1.05 to 0.60 under such a switch. Gate every new BSDF with a lit furnace (uniform environment plus one small bright emitter), require NEE on/off invariance, and cross-check `eval`/`pdf` against `sample`.
- An unlit or environment-only test scene cannot see directional inconsistency between eval and sample; a small emitter is needed.
- Before accepting that an estimator "fundamentally diverges", derive its analytic per-sample bound and instrument the largest contributors. Both v1 "divergences" were defects: an absolute-value cosine that flipped the visible-normal sampling sign, and a missing escape guard on a downward ray that let a masking term overflow to NaN. Example bound: the refraction half-vector Jacobian denominator is at least |n_t - n_i|.
- Not every BSDF is hemisphere-limited. A global "drop samples with wi.n <= 0" made hair about 9x too dark (it killed the transmitting TT/TRT lobes) and also broke subsurface and isotropic phase functions. Hemisphere tests belong inside the lobe that needs them.
- An occlusion query's "infinite" distance for distant or sphere lights is not a geometric distance. Feeding it into Beer-Lambert or falloff silently zeroed or saturated results, and analytic and parity gates both passed. Enumerate per light type which distance is geometric; add a test that increasing medium density monotonically darkens the result.
- Firefly clamps bias the estimate and can cap legitimate focused energy (a clamp capped a caustic focus). Parity and reference renders run with clamps off.
- Single-precision is fine for ordinary transport; the relativistic geodesic integrator needed double precision. Keep the split explicit in the types.

## Geometry, transforms and normals

- Instance transforms with non-unit scale: if the ray direction is renormalised in object space, the hit distance must be converted back to world units. v1's scaled triangle meshes were invisible because `t` came back in object-space units; spheres happened to be immune because their quadratic uses |d|.
- A transform must carry the front-face flag through unchanged. Recomputing it from the transformed normal clobbered it, and transformed glass rendered at 0.39 of its untransformed brightness.
- Coincident triangles (an object resting exactly on another) are tie-broken differently by different BVH/traversal implementations, and Cycles has no tie rule either. Disagreement inside contact geometry can be legitimate. Check mesh hygiene (signed volume, coplanar faces) and compare against a second traversal before filing a backend bug.
- Unexplained, still open: a neutral triangle floor rendered about 0.6% brighter on GPU than CPU while a sphere matched; single-bounce and brightness-independent, so geometry or normals were suspected, not spectral upsampling.

## Dielectrics and refraction

- Decide entering vs exiting from the geometric front-face flag, never from the sign of wo against an already-oriented normal. With the oriented normal every hit reads as "entering", eta is 1/ior at both surfaces, and energy loss grows with IOR (white furnace 0.51 instead of 1.0).
- Clear glass in a uniform white environment must render 1.0 at every IOR. A deficit that grows with IOR points at eta handling.
- Keep the 1/eta^2 radiance scaling at a refractive crossing; it is correct for radiance (pbrt convention) and the owner ruled to keep it. Importance/photon transport is not symmetric (Veach): photon deposits must strip it, or use a Fresnel-weighted deterministic deposit. References that disagree are regenerated, not pinned.
- Rough dielectric "dead samples" (visible-normal reflections that end below the horizon): for a low-Fresnel dielectric that energy physically re-enters and mostly transmits. Reflection-lobe multi-scatter compensation recovers almost nothing (about +0.009 at roughness 1, because single-scatter Fresnel is about 0.09). v1's fallback that redistributed the energy into transmission was load-bearing; removing it dropped the roughness-1 furnace from 0.997 to 0.788. The principled fix is multiple-scattering compensation (Kulla-Conty or Turquin style), not a sampler tweak.
- Rough glass references: use a multiple-scattering microfacet random walk (Heitz et al. 2016) as the oracle, not Cycles' energy-scale approximation. Cycles evaluates transmission for invalid refraction configurations, which leaks light at roughness >= ~0.85; v1 recorded this as a documented divergence.
- Before calling a sampling statistic a bug, confirm both sides measure the same quantity. An apparent 3x dead-sample rate was raw visible-normal reflection rate against a post-Fresnel-split rate.

## Spectral

- RGB-to-spectrum upsampling (Jakob-Hanika) is nonlinear in magnitude and clamps to [0,1]. Upsample the [0,1] reflectance colour, then apply cos/pi, weights and other scalars linearly. Upsampling albedo*cos/pi gave a chroma-dependent 2.5% per-channel error (below 0.02% after the fix).
- Transport factors above 1 (refraction eta^2 up to about 4) must be applied as a flat spectral scalar with only the normalised tint upsampled. Otherwise enter/exit no longer cancel and a white furnace loses energy with IOR (0.705 at IOR 1.5).
- Diagnose chroma-dependent error with a neutral and a saturated sphere: neutral clean and saturated wrong points at upsampling. Test through the render path, not only a unit probe.
- Emitters: importance-sample (or weight) wavelengths by the emitter SPD. Uniform wavelength sampling of a sodium lamp produced a continuous rainbow where discrete lines belong. Every emitter type, including emissive geometry and textures, needs a spectral path.
- Blackbody: evaluate Planck's law spectrally; an RGB lookup table is only a cross-check.
- IES and photometric lights need absolute units (Cycles uses 4*pi/177.83 per candela with node Strength honoured); peak-normalising the profile was wrong.
- Line-profile tables: lowering a line's peak while conserving area inflates anything else normalised to the peak. Broadening one sodium line made a mercury lamp 4.5-8.6x brighter through a shared normalisation. A/B-render every line lamp after any profile edit.
- The wavelength band is part of the contract and must be enforced in one place; a refactor that bypassed the band clamp changed a UV-band result by 1.5x.
- With only 4 wavelength samples under a white illuminant, per-wavelength thin-film or conductor evaluation round-trips to nearly the same RGB as an upsampled RGB evaluation. Do not promise a visible saturation gain from "going spectral" there; it shows only under coloured illumination or with more samples.
- Hair melanin: eumelanin sigma_a proportional to lambda^-3.33 (Jacques 2013), pheomelanin lambda^-4.75 (Donner and Jensen 2006), anchored at 550 nm to the Cycles green coefficient. Earlier colour characterisations made through a broken spectral hair path are untrustworthy.

## Caustics, photons and manifold sampling

- Shadow caustics need smooth-shaded casters; flat facets each refract separately and break the caustic up. Cycles shows the same behaviour.
- Closed solids need consistently outward geometric normals for photon and light tracing. Generate winding automatically (for example, normals away from the centroid for convex casters); an open "tent" of two quads has no inside and scatters incorrectly.
- A progressive photon pass needs a fresh per-iteration seed. Without one, every pass rebuilt an identical photon map and caustic noise never averaged down.
- Specular manifold sampling: the deterministic seed path must weight with the manifold (MNEE-style) generalised geometry term, not the seed-area pdf. v1 double-counted the receiver cosine (cos^2) on one path. Single-seed Newton underestimates multi-solution foci by about 5%; use a multi-solution (polynomial) solver rather than chasing it.

## Sampling, adaptivity and guiding

- One random draw per random number, in a fixed dimension order, on every target. A library distribution that consumed draws differently broke backend alignment in v1.
- The adaptive stopping metric must be colour-aware. Stopping on scalar luminance (X+Y+Z) left a blue variance floor.
- Adaptive sampling's stop/resume model assumes a progressive low-discrepancy sampler with a prefix property (Sobol, PMJ). On a white-noise counter RNG that premise is unmet; build the progressive sampler first.
- Path guiding (SD-tree, Mueller 2017) gave about 1.3x on diffuse scenes once NEE was present; the 110x prototype figure was measured without NEE. Training samples must be folded into the image: discarding them wasted 3x rays at 24 spp and darkened the result through clamped fireflies.
- Never use a seed value that means "random"; a "deterministic" test rendered with seed 0 was nondeterministic.

## Feature semantics

- Per-closure parameters such as Filter Glossy (a roughness floor) must be applied at exactly one point where closures are built. v1 recomputed roughness at about four sites per material, so the setting was stored and never read.
- Per-ray-type bounce limits and caustic toggles must be honoured everywhere; v1 silently discarded them on both backends while the spec claimed one honoured them.
- Volumes are core, not optional, if Blender scenes are to render as authored.
- Sky: a Preetham sky over flat ground gave a direct:diffuse ratio of 1.8:1 where Nishita gives 6.4:1, with sky diffuse about 2x too bright. No constant fixes that; use a Nishita-class model.

## Correctness policy

- When the physics is right and Cycles differs, keep the physics and record the divergence with oracle evidence. Cycles is a cross-check band where it is approximate, and the target only where it is physically right or an artist look must match (owner: "physically correct always").

## Open questions inherited from v1

- How to close the rough-dielectric furnace properly: which multiple-scattering compensation, and how it interacts with transmission redistribution.
- Cause of the 0.6% triangle-vs-sphere brightness offset between backends.
- Whether register pressure in v1 came intrinsically from the spectral BSDF state plus the NEE block (two ~160-register consumers on a ~95-register base) or from its fused-kernel structure.
- Spectral range and observer: v1 had generations using 360-830 nm and 380-780 nm with an unresolved observer mismatch; their colour equivalence was never measured.
- Real payoff of guiding beyond the ~1.3x seen with NEE; the right many-light strategy (v1's light tree landed but stayed off).
- Residual ~5% manifold-sampling underestimate at multi-solution foci.
