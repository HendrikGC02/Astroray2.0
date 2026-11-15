# 062 EON diffuse lobe
Goal: there is no diffuse lobe -> the energy-preserving Oren-Nayar lobe of OpenPBR is a pure kernel function with sample, eval and pdf taking `TransportMode`, verified by the energy test, chi-squared and the eval/pdf consistency checks.
Read first: 0005 item 4 (lobe stack, EON), item 7 (per-wavelength evaluation), item 8 (TransportMode, below-surface rejection), item 6 (EON albedo is analytic); 0009 d14-d15 (pure functions, energy and chi-squared tests); 0004 D1 (f excludes the cosine).
Scope: src/kernel/material/lobes/lobe.h (shared result types), src/kernel/material/lobes/eon.h, tests/cpu/test_lobe_eon.cpp (+ f64).
Out of scope: layering with the specular lobe (064); the shade stage (071, 073).
Reference: Portsmouth, Kutz and Hill 2025, "EON: A practical energy-preserving rough diffuse BRDF", JCGT (equations for f, the analytic albedo and the sampling scheme); Adobe `openpbr-bsdf` `impl/` EON lobe file (Apache-2.0) as the cross-check implementation.
Do:
1. lobe.h: `LobeSample {Real3 wi; Spec f; Real pdf; uint32_t flags /*DELTA, REFLECTION, TRANSMISSION*/}`, `LobeEval {Spec f; Real pdf;}`, `enum class TransportMode {Radiance, Importance}`; the shading frame is core/frame.h (019); every function `AR_FN`, structs over 32 B by const reference.
2. eon.h: `eon_eval(params, wo, wi, mode)`, `eon_pdf(...)`, `eon_sample(params, wo, u2, mode)` for base_color (per lane) and base_diffuse_roughness, from the paper; reflected samples below the geometric surface are rejected by the caller (documented); f excludes cos; roughness 0 equals Lambertian rho/pi exactly.
3. Tests: fast (deterministic, fixed quadrature): integral of pdf over the hemisphere <= 1 (1e-3); `eval.f == sample.f` and `eval.pdf == sample.pdf` at sampled directions (1e-5 relative); roughness 0 matches Lambertian to 1e-6; gate (harness): energy test of f cos/pdf against the paper's analytic albedo for roughness 0, 0.5, 1 at cos theta_o in {0.05, 0.25, 0.5, 0.75, 1} and four wavelengths; chi-squared of samples against the pdf integrated per cell (ires 8, retry at 16 by the harness).
Acceptance: `pwsh tools/check.ps1 fast` passes; `out/bin/astroray_tests_cpu.exe --tier=gate -tc="lobe_eon/*"` passes and prints the albedo error per configuration.
Look at: none.
Canary: tasks/062.canary.patch must turn red: astroray_tests_cpu "lobe_eon/energy vs analytic albedo", "lobe_eon/chi2", "lobe_eon/boundary uniforms".
Budget: counts toward the gate statistical family (report checks added).
Depends on: 020, 019.
Size: M (about 300 lines); stop and report at 2x.
Progress:
- Implemented from the paper's Listings 1-4 (JCGT 14(1)); sampling is CLTC + uniform one-sample MIS, not cosine sampling.
- Deviation: 1 - E numerators floored at 0 (paper: 1e-7) so roughness 0 is exactly rho/pi; denominator keeps 1e-7.
- Literals in the test come from an independent NumPy transcription, which also checked albedo = integral of f cos and pdf integral = 1.
- Gate checks added per test copy: 40 energy z-tests + 5 chi-squared (r = 0 energy rows are zero-variance, compared at 1e-6 instead); 90 across the f32 and f64 copies.
- Canary: tasks/062.canary.patch (5 one-line breaks), all 18 lobe_eon cases red with it applied.
