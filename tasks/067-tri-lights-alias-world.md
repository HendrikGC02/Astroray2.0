# 067 Emissive triangles, the constant world light and the alias-table light sampler
Goal: emissive meshes and the world are not lights and no light can be chosen -> every emissive triangle and the constant world implement the light interface, `ar_edit_set_world` compiles the world graph to a constant radiance, and an alias table over all lights implements `LightSampler` with `sample` and `pmf`.
Read first: 0004 D9 (emissive geometry, zero-estimate rule), D10 (LightSampler, importance energy, alias table as oracle), D6 (world row), D7; 0005 item 43 (emission estimate, sidedness), item 32 (world entry); docs/decisions/0007-appendix-abi.md (`ar_world_desc`).
Scope: src/kernel/light/triangle.h, src/kernel/light/world_const.h, src/kernel/light/light_sampler.h, src/host/light/light_builder.{h,cpp}, src/host/scene/world.{h,cpp}, src/capi/scene_api.cpp, tests/cpu/test_light_sampler.cpp.
Out of scope: the light tree, radiometric floor and excitation weighting (M4: the alias table uses display-weighted importance only); environment maps (M1b); NEE in the shade stage (074).
Reference: Arvo 1995, "Stratified Sampling of Spherical Triangles"; pbrt-v4 `src/pbrt/shapes.h` `Triangle::Sample` (solid-angle range constants), `src/pbrt/util/sampling.h` `SampleSphericalTriangle`, `AliasTable`, `src/pbrt/lightsamplers.h` `PowerLightSampler`, `src/pbrt/lights.h` `UniformInfiniteLight` (Apache-2.0).
Do:
1. triangle.h: one light per emissive triangle; radiance from the material's emission program evaluated through the `emission` entry at the sampled point (NEE path, task 074) or its constant SpectrumRef; spherical-triangle sampling when the subtended solid angle is inside pbrt's range, else uniform area; one- or two-sided per the material's emission-sampling setting (theta_e pi/2 or pi).
2. world_const.h: constant radiance from the compiled world program (`CONSTANT_EMISSION` path), uniform sphere sampling (pbrt UniformInfiniteLight), `infinite = true`.
3. world.{h,cpp}: `ar_edit_set_world` stores the WORLD graph key; the world job compiles it with 058/059 (Background colour x strength lowered to EMIT_LE); a world graph that does not fold to a constant in M1a raises `NODE_UNSUPPORTED` (detail = first non-constant node) and renders as constant magenta radiance of strength 1, the world equivalent of 0005 item 40's magenta material.
4. light_builder (host, double): per light the importance energy = integral of s(lambda) times the normalised display response (CMF sum) over the support, times power or area x pi x emission estimate (0004 D9); alias table over every light (lamps, emissive triangles, world); a triangle with estimate 0 gets pmf 0. Host arrays in the ledger tables category (CUDA upload: task 102); rebuilt only when lights, emissive materials or the world change.
5. light_sampler.h: `sample(ctx, u) -> {light index, pmf}`, `pmf(ctx, light)`.
6. Tests (fast): `pmf(ctx, sampled) == pmf` (1e-6) for a mixed set of 1,000 lights; selection frequencies over 2^20 draws match pmfs (harness, gate); a zero-estimate triangle is never chosen; the world light's pdf integrates to 1; a world with a non-constant node raises one entry.
Acceptance: `pwsh tools/check.ps1 gate` passes with `light_sampler/*` cases.
Look at: none.
Canary: tasks/067.canary.patch must turn red: astroray_tests_cpu "light_sampler/pmf matches sample", "light_sampler/zero estimate never chosen".
Budget: B-23 is measured later (094).
Depends on: 066, 059.
Size: M (about 450 lines); stop and report at 2x.
Progress:
