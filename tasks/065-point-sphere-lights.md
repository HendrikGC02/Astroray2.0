# 065 Light interface, point and sphere lamps
Goal: there are no lights -> the one light interface of 0004 D7 (with the segment context and `sample_le`) exists as pure kernel functions, with point lamps of radius 0 (delta) and above (sphere, visible-cone sampling) in Blender's energy convention under the unit anchor.
Read first: 0004 D6 (types and energy conventions), D7 (interface, no sentinels), D1 (delta lights: NEE weight 1, BSDF pdf 0); 0003 d11 (unit anchor, Blender watt factor 1), d16 (illuminant convention); 0004 Verification rows D6 and D7/D11.
Scope: src/kernel/light/light.h, src/kernel/light/light_record.h, src/kernel/light/point.h, src/kernel/light/sphere.h, tests/cpu/test_lights_point.cpp (+ f64).
Out of scope: area lamps (066), emissive triangles, world and the alias table (067), host ingestion and lamp primitives (068), spot lamps (M4).
Reference: pbrt-v4 `src/pbrt/lights.h`/`lights.cpp` `PointLight::SampleLi`, `SampleLe`, and `src/pbrt/shapes.h` `Sphere::Sample(ctx, u)` (visible-cone sampling) (Apache-2.0).
Do:
1. light.h: `LightSampleContext {Real3 p, n_geom, n_shade; kind POINT | SEGMENT (o, d, t_max)}`; `LightLiSample {Real3 wi; Real dist; bool infinite; Real pdf; bool delta; Real3 p_light; Spec Le;}` (no 1e30 sentinel); `LightLeSample {ray, Spec Le, Real pdf_pos, Real pdf_dir}`; `LightBounds {bbox, theta_o, theta_e, two_sided}`; dispatch by a closed `LightType` enum in a switch (no virtuals or function pointers).
2. light_record.h: {type, xf, radius or sizes, emission SpectrumRef (ILLUMINANT), strength scale, flags, light_group u8, visibility bits}; spectral emission s(lambda) = illuminant upsample of colour times power (0003 d11, factor 1), times the radiance scale 1/s applied later in the resolve.
3. point.h (r = 0): I = P/(4 pi) s(lambda); `sample_li` delta with pdf 1; `pdf_li` 0; `sample_le` uniform sphere; `power` = P (weighted later by 067). sphere.h (r > 0): L = P/(4 pi^2 r^2) s(lambda), visible-cone sampling, `pdf_li` consistent with it, `sample_le` per pbrt.
4. Tests (fast unless noted): `pdf_li(sample.wi) == sample.pdf` within 1e-4 relative for spheres at distances 1.1 r to 1000 r; irradiance at a point from 2^16 sphere samples matches the analytic sphere irradiance within 0.5% and the point lamp's I cos/d^2 exactly; the sphere's radiance from P and r reproduces power P by quadrature (1e-4); gate: chi-squared of sphere sample directions against the pdf.
Acceptance: `pwsh tools/check.ps1 fast` passes with `lights_point/*`; `out/bin/astroray_tests_cpu.exe --tier=gate -tc="lights_point/*"` passes.
Look at: none.
Canary: tasks/065.canary.patch must turn red: astroray_tests_cpu "lights_point/sphere pdf consistency", "lights_point/sphere irradiance".
Budget: none.
Depends on: 023, 019, 020, 056.
Size: M (about 350 lines); stop and report at 2x.
Progress:
