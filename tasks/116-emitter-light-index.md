# 116 Emissive-hit light index
Goal: a BSDF ray that hits an emissive triangle cannot name its light, so emission MIS (074) cannot weight it -> the light builder publishes, per instance, a base offset into a per-triangle light-index array (kNoLight where the triangle does not emit), both devices upload it with the light table of the pinned snapshot, and a kernel helper returns the light index of any hit.
Read first: 0004 D1 (emitter hits weighted with the light's pmf), D9 (emissive triangles, zero-estimate rule), D10 (alias table); RULINGS.md 70 (pinned snapshots); src/host/light/light_builder.{h,cpp} (gather_scene_lights), src/host/scene/scene_tables.{h,cpp}, src/host/session/kg_builder.cpp, src/kernel/scene/scene_view.h, src/kernel/kernel_globals.h.
Scope: src/host/light/light_builder.{h,cpp}, src/host/scene/scene_tables.{h,cpp}, src/host/session/kg_builder.{h,cpp}, src/kernel/scene/scene_view.h or src/kernel/kernel_globals.h (whichever holds light data), src/kernel/light/light_sampler.h (the lookup helper), tests/cpu/test_light_index.cpp, tests/gpu/test_scene_upload.cpp and tests/gpu/kernels/kg_checksum.cu (the new table's row).
Out of scope: NEE and MIS themselves (074); lamp hits (075; lamp instances already carry light_index); light trees (M1b).
Reference: none (a lookup table derived from 067's light order).
Do:
1. light_builder emits, alongside the light table and in the same build, the per-triangle light index for every instance whose material emits, with a per-instance base (kNoLight base for instances with no emitting triangles); its order matches the light records exactly, so lights[index] is that triangle's light.
2. scene_tables uploads it in place (TABLES ledger category) with the snapshot's light table; kg_builder binds it; the scene-upload checksum test gains its row.
3. `light_of_hit(kg, instance, triangle) -> uint32_t` (kNoLight when the triangle is not a light), one source for CPU and GPU.
4. Tests (fast): for a scene with two emissive meshes (one partly emissive by material slot), a non-emissive mesh, a lamp and the world, every emitting triangle maps to the light record holding its vertices, non-emitters map to kNoLight, and a material edit that turns emission off rebuilds the map; GPU (gate): the uploaded map's checksum equals the host's.
Acceptance: `pwsh tools/check.ps1 gate` passes with `light_index/*` and the new scene_upload row.
Look at: none.
Canary: tasks/116.canary.patch must turn red: astroray_tests_cpu "light_index/emitting triangles map to their light", astroray_tests_gpu "scene_upload/checksums match host".
Budget: host code plus one table; bytes reported by the upload test.
Depends on: 067, 068, 102.
Size: S (about 200 lines); stop and report at 2x.
Progress:
- Started: map derived in build_light_table from the final records (one pass, index = record position).
- One uint32 table LIGHT_INDEX: [N, per-instance slice offset or kNoLight, slices {C, light per triangle}]; KernelGlobals light_index/light_index_words appended last.
- light_of_hit: kNoLight for non-emitters and lamp instances; kLightIndexInvalid (~0u-1, AR_ASSERT in debug) for an instance or triangle outside the scene (checked against SceneData).
- Duplicate (instance, triangle) records throw in build_light_table. Canary: swapped entries + short LIGHT_INDEX upload; CPU red verified via check fast.
