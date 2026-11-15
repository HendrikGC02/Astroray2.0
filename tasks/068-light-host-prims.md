# 068 Lamp ingestion, lamp instances and the radiance scale
Goal: Blender lamps cannot enter the engine -> `ar_edit_set_light` builds point, sphere, rectangle and disk lamp records with reported approximations, each lamp gets an instance record carrying its light index and visibility but never the SHADOW bit, and the per-render radiance scale s = 2^k is computed.
Read first: 0004 D6, D8 (lamps as primitives, never block shadow rays), 0002 D11 (mask), 0003 d10 (radiance scale), docs/decisions/0007-appendix-abi.md (`ar_light_desc`, `ar_spectrum_ref` as decided in task 031), 0001 D2.11 (`NODE_APPROXIMATED`).
Scope: src/host/light/lamps.{h,cpp}, src/host/scene/instances.{h,cpp} (lamp instances), src/capi/scene_api.cpp, src/capi/unimplemented.cpp, tests/cpu/test_lamps.cpp.
Out of scope: lamp primitives in Embree and OptiX (105); shading a lamp hit (075); spot, sun and IES (M4); light trees (M1b).
Reference: Blender's lamp conventions as documented in the Blender 5.3 manual (Light properties: Power, Radius, Size, Shape, Normalize); Cycles `src/scene/light.cpp` (Apache-2.0) for the `normalize` semantics of area lamps.
Do:
1. lamps: POINT with size_x = radius (0 = delta point, else sphere); AREA_RECT with size_x by size_y; AREA_ELLIPSE with equal sizes as a disk; colour from `ar_spectrum_ref` RGB packed to an ILLUMINANT SpectrumRef; power passed through unscaled (0003 d11); `normalize` handled as Cycles does (read and cite the line). Unequal ellipse axes, spread below 180 degrees or BLACKBODY/TABLE colours (M1b, M4) raise `NODE_APPROXIMATED` with the setting named and render with the nearest supported form (equal-axis disk, spread 180, the colour's RGB if given, else white).
2. Lamp instances: each lamp appends an instance record (036) with `light_index` set, its transform, and the light's camera, diffuse, glossy and transmission visibility bits, never SHADOW; its geometry is the lamp shape that task 105 builds.
3. Radiance scale: per render the host takes the largest emitter spectral radiance over the table support (lamps, emissive estimates, world) and picks s = 2^k so it lies in [1, 2); s multiplies every emitter's strength on the host; the resolve removes it (047).
4. Tests (fast): records for each lamp type from descriptors; an unequal ellipse raises one entry; lamp instances carry their light_index and never the SHADOW bit; s for a set of emitters is a power of two placing the largest radiance in [1, 2).
5. From 067: the scene sphere that sizes the world light's importance (4π²R²) must include lamp bounds as well as instance bounds, so a scene with lamps and no meshes still gives the world a positive pmf; add the lamps to 067's light_builder input from the scene (until now they came from tests only).
Acceptance: `pwsh tools/check.ps1 fast` passes with `lamps/*` cases.
Look at: none.
Canary: tasks/068.canary.patch must turn red: astroray_tests_cpu "lamps/radiance scale power of two", "lamps/no shadow bit".
Budget: none.
Depends on: 067, 036.
Size: S (about 250 lines); stop and report at 2x.
Progress:
