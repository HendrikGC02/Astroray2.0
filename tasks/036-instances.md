# 036 Instances, transforms, visibility mask and material tables
Goal: geometry cannot be placed -> `ar_edit_set_instances` and `ar_edit_update_transforms` build validated host instance records with transforms, motion steps, the 8-bit visibility mask of 0002 D11, instance flags, light group and material table.
Read first: 0002 D11 (instance visibility mask table), 0007 d12 (edit map rows for transforms and instances), d15 (motion steps), d18 (keys), docs/decisions/0007-appendix-abi.md (`ar_instances_desc`, as decided in task 031).
Scope: src/host/scene/instances.{h,cpp}, src/capi/scene_api.cpp, src/capi/unimplemented.cpp, tests/cpu/test_instances.cpp.
Out of scope: TLAS building (037, 040); motion blur (M8: S > 1 is validated and stored, the shutter-centre step is used); media boundary bit 6 (never set until media exist).
Reference: none.
Do:
1. Host instance record: geometry key, 3x4 object-to-world per motion step, object key, random_id, pass_index, crypto ids (stored), colour, visibility bits, light_group (u8), material table slice into the pool of material keys, `light_index` (u32; ~0u for geometry, the light's index on the lamp instances task 068 appends; route_hit reads it, 039).
2. Mask from visibility bits: camera -> bit 0, diffuse -> 1, glossy -> 2, transmission -> 3, shadow -> 4, volume -> 5; bit 6 (MEDIUM_BOUNDARY) unset in M1a; bit 7 never. Instance flags holdout, shadow catcher and caster opt-out are stored for shading and the light pass, never as mask bits.
3. `ar_edit_set_instances` replaces all instances (validation on the worker: geometry keys exist in the generation, material table slices in range, transforms finite); `ar_edit_update_transforms` patches the listed indices only (indices in range, finite).
4. Each change marks the TLAS dirty for the device tasks; GAS are untouched.
5. test_instances.cpp (fast): mask mapping for every visibility bit combination of one bit; update_transforms patches only the listed instances; an unknown geometry key and a NaN transform each reject the generation with GENERATION_REJECTED naming the instance index in `detail`.
Acceptance: `pwsh tools/check.ps1 fast` passes with `instances/*` cases.
Look at: none.
Canary: tasks/036.canary.patch must turn red: astroray_tests_cpu "instances/mask from visibility", "instances/update patches listed only".
Budget: none.
Depends on: 032.
Size: S (about 250 lines); stop and report at 2x.
Progress:
- 036: instances.{h,cpp}, capi/instances_api.cpp, Scene::instances()/tlas_epoch(); canary tasks/036.canary.patch.
