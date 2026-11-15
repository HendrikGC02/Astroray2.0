# 102 KernelGlobals assembly and CUDA upload of scene and session tables
Goal: no task builds `KernelGlobals` or puts the spectral, material, image, light, camera and settings data on the GPU -> one builder assembles KernelGlobals per session on both devices from the active generation and session state, and the CUDA device uploads every scene table per generation and the KernelGlobals block per epoch, all through the ledger.
Read first: 0002 D13 (scene data layout, ledger), D5 (pools), 0007 d11-d14 (generations, ledger categories, reserve rule), d25 (latch); src/kernel/kernel_globals.h; src/device/cuda/scene_upload.h (040).
Scope: src/host/session/kg_builder.{h,cpp}, src/host/scene/scene_tables.{h,cpp}, src/device/cuda/scene_upload.{h,cpp}, src/kernel/kernel_globals.h, src/host/session/session.cpp, tests/cpu/test_kg_builder.cpp, tests/gpu/test_scene_upload.cpp, tests/gpu/kernels/kg_checksum.cu.
Out of scope: partial uploads below table granularity (later); texture compression and mips (M3); light trees (M1b).
Reference: none.
Do:
1. scene_tables: the per-generation set of host arrays that earlier tasks produce: wavelength table (024), rgb2spec table (023), observer and response channels (046), filter table (048), E table (064), material headers, programs and constant blocks (059), image descriptors and texels (060), light records and alias table (067, 068), meshes, instances and material-table pool (035, 036, 040), camera view (053). Each table has a dirty flag set by the job that rebuilt it.
2. CUDA: scene workers upload dirty tables before the generation becomes ready (a failed upload rejects the generation through 032's mechanism); unchanged tables keep their device copy; replaced copies are freed when their generation retires; every allocation carries its ledger category (tables, textures, geometry).
3. kg_builder: at each latch (052) builds the host `KernelGlobals` with pointers to the active generation's tables, the session's pools, film, step table and active list, and `SessionUniforms` (settings, epoch, sampler mode, test hooks of 101); on CUDA copies it to one device block with one async copy before the pass's first launch; CPU uses host pointers. Every pointer field is set or marked unused by the feature set.
Sessions on a CUDA context: until this task, `ar_session_create` refuses them with AR_ERR_UNSUPPORTED (052); hand the scene's CUDA device to the session and remove that refusal (src/capi/session_api.cpp and src/capi/scene_api.cpp join Scope for this).
4. Tests: test_kg_builder.cpp (fast): for a scene with one object of each kind every non-optional KernelGlobals pointer is non-null and every count matches the scene; test_scene_upload.cpp (gate): kg_checksum.cu reads the count, first and last element of every table through the device KernelGlobals and returns a checksum equal to the host's; changing one material re-uploads only the material tables (upload byte counter); ledger totals equal the sum of table sizes.
Acceptance: `pwsh tools/check.ps1 gate` passes with `kg_builder/*` and `scene_upload/*`; the GPU test prints each table's bytes and checksum.
Look at: none.
Canary: tasks/102.canary.patch must turn red: astroray_tests_gpu "scene_upload/checksums match host", astroray_tests_cpu "kg_builder/no null tables".
Budget: host code; upload bytes per edit reported.
Depends on: 101, 040, 059, 060, 064, 068.
Size: M (about 400 lines); stop and report at 2x.
Progress:
