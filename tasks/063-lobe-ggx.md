# 063 GGX dielectric specular reflection lobe and lobe_is_delta
Goal: there is no glossy lobe -> GGX with height-correlated Smith masking and visible-normal sampling gives OpenPBR's dielectric specular reflection (specular_weight modulation of F0, specular_color, specular_ior), switches to a delta sample below the effective-smoothness threshold through the single predicate `lobe_is_delta`, and passes the statistical lobe tests.
Read first: 0005 item 4 (GGX, height-correlated Smith, VNDF, specular_weight F0 modulation, delta threshold), item 7, item 8 (TransportMode), item 13 (Rs and Rp averaged); 0004 D19 (`lobe_is_delta` is the one predicate); 0009 d15.
Scope: src/kernel/material/lobes/ggx.h, src/kernel/material/lobes/fresnel.h, src/kernel/material/lobes/delta.h, tests/cpu/test_lobe_ggx.cpp (+ f64).
Out of scope: transmission, metal, coat, thin film, anisotropy, energy compensation (M1b and M2); layering (064).
Reference: Heitz 2014, JCGT 3(2) (height-correlated Smith); Heitz 2018, JCGT 7(4) (VNDF sampling); Walter et al. 2007 (microfacet dielectric); OpenPBR 1.1.2 specification, dielectric specular section; pbrt-v4 `src/pbrt/util/scattering.h` `TrowbridgeReitzDistribution` (incl. `EffectivelySmooth`) and `FrDielectric` (Apache-2.0); Adobe `openpbr-bsdf` `impl/openpbr_vndf_microfacet_distribution.h` (Apache-2.0) as cross-check.
Do:
1. fresnel.h: unpolarised dielectric Fresnel (mean of Rs and Rp) per lane.
2. ggx.h: alpha = specular_roughness^2 (OpenPBR's mapping), isotropic in M1a; `ggx_eval/pdf/sample(params, wo, wi|u2, mode)`; F0 from specular_ior per lane modulated by specular_weight and tinted by specular_color exactly as the spec writes it; VNDF sampling; f excludes cos.
3. delta.h: `lobe_is_delta(alpha_x, alpha_y)` = both below 1e-3 (pbrt `EffectivelySmooth`); when true, ggx_sample returns the mirror direction with flag DELTA and weight F, and eval/pdf return 0.
4. Tests: fast: pdf integrates to <= 1 (1e-3, quadrature) for roughness 0.1, 0.5, 1; eval/sample consistency (1e-5); peak of sampled directions at the mirror direction for roughness 0.05 (v1 VNDF lesson: check the peak position); delta path for roughness 0 and 1e-4; gate: chi-squared per roughness and cos theta_o; energy test of f cos/pdf against deterministic quadrature of f cos (consistency only, 0009 d15(a)).
Acceptance: `pwsh tools/check.ps1 fast` passes; `out/bin/astroray_tests_cpu.exe --tier=gate -tc="lobe_ggx/*"` passes.
Look at: none.
Canary: tasks/063.canary.patch must turn red: astroray_tests_cpu "lobe_ggx/chi2 roughness 0.5", "lobe_ggx/delta below threshold".
Budget: gate statistical family checks reported.
Depends on: 062.
Size: M (about 350 lines); stop and report at 2x.
Progress:
- Added ggx.h, fresnel.h, delta.h and tests/cpu/test_lobe_ggx.cpp; VNDF basis follows the JCGT paper's lensq>0 special case (a first 0.9999 branch was non-orthogonal and was caught by the pinned samples).
- Delta convention: pdf 1, f = F / cos, so f cos / pdf = F. specular_weight clamps xi F0 at 0.9999 (numerical guard).
- f32 eval-vs-sample tolerance is 2e-4 (f64 1e-5): the half vector recovered from wo + wi is rounded in float at roughness 0.1, grazing wo.
