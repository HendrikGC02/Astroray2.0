# 019 Spectral packet types, vector maths, path flags and packet operations
Goal: there is no spectral type -> `Spec`, `Lambda` and `kLanes = 4` exist once in `core/spectrum.h`, `Real2`/`Real3` vector maths and the shading `Frame` once in `core/vec.h` and `core/frame.h`, the path flag bits exist once in `state/flags.h`, and the five packet operations of 0003 decision 5 are pure kernel functions with unit tests.
Read first: 0003 d1 (lane count), d4-d7 (transport wavelengths, packet operations, hero lane, MIS lane rules), d21 (flag bits), 0003 Verification check 18; R6, R24.
Scope: src/kernel/core/spectrum.h, src/kernel/core/vec.h, src/kernel/core/frame.h, src/kernel/core/packet.h, src/kernel/state/flags.h, tests/cpu/test_packet.cpp and tests/cpu/test_vec.cpp (both marked `// ar-f64`, task 018).
Out of scope: the full path-state layout (025); wavelength sampling (024); callers of the operations.
Reference: pbrt-v4 `src/pbrt/util/spectrum.h` `SampledSpectrum`, `SampledWavelengths::TerminateSecondary` (Apache-2.0); Rybicki and Lightman 1979 section 1.5 (I_lambda lambda^5 invariant) for `apply_frequency_shift`; Duff et al. 2017, "Building an Orthonormal Basis, Revisited", JCGT 6(1), for the frame from a normal (users: 025, 053, 062, 065).
Do:
1. core/spectrum.h: `constexpr int kLanes = 4`; `struct Spec { Real v[kLanes]; }` and `struct Lambda { Real nm[kLanes]; }` with element-wise + - * /, scalar products, `avg`, `max_lane`, `is_finite`; all `AR_FN`, passed by value (16 or 32 B).
2. core/vec.h: `Real2`, `Real3` (by value, 12 or 24 B) with + - * /, scalar products, `dot`, `abs_dot`, `cross`, `length`, `normalize`, `face_forward`; core/frame.h: `Frame {Real3 s, t, n}` from a normal (Duff et al. 2017) or from a normal and tangent (Gram-Schmidt), `to_local`, `to_world`; all `AR_FN`.
3. state/flags.h: every bit of 0003 d21 as a named `constexpr uint32_t` (bits 0-21) plus helpers `hero_lane(flags)` (bits 7-8), `set_hero_lane`, `medium_depth` (bits 12-14); no other bit defined.
4. core/packet.h, exactly 0003 d5: `terminate_secondaries(Lambda&, Spec& beta, uint32_t& flags)` copies lane 0 of both into lanes 1-3 and sets `SECONDARIES_TERMINATED` (r_u, r_l untouched); `rewrite_wavelength(Lambda&, Spec& beta, Real lambda_i, Real ratio, uint32_t& flags)`; `apply_frequency_shift(Lambda&, Spec& beta, Real g, uint32_t& flags)` (lambda *= g on every lane, beta *= g^5, sets `WAVELENGTHS_REWRITTEN`, never terminates lanes); `film_weights(uint32_t flags, const Spec& pf) -> Spec` ((1/pf0, 0, 0, 0) when terminated, else 1/(4 pf_i)); `hero(flags)`.
5. Tests (suite fast), float and double: terminate copies lane 0 and leaves r_u/r_l; rewrite sets every lane and scales beta by ratio; frequency shift gives lambda*g and beta*g^5 to 1 ulp on all lanes with no lane terminated (0003 check 18, unit part); both film_weights branches; hero bits round-trip for 0..3; frames from 10^4 normals including (0, 0, -1) are orthonormal and `to_world(to_local(v))` returns v, both to 1e-6.
Acceptance: `pwsh tools/check.ps1 fast` passes; `out/bin/astroray_tests_cpu.exe --tier=fast -tc="packet/*"` passes in the float and the f64 test objects (both names appear).
Look at: none.
Canary: tasks/019.canary.patch must turn red: astroray_tests_cpu "packet/frequency shift g^5", "packet/terminate keeps lane 0".
Budget: none.
Depends on: 018.
Size: M (about 350 lines); stop and report at 2x.
Progress:
