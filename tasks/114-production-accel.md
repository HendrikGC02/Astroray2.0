# 114 Production acceleration structures per scene generation
Goal: only tests build an EmbreeScene or OptiX GAS/TLAS -> each scene generation gets its traversal structures (meshes, instances and the lamp primitives of 105) on its device, pinned with the generation's device tables, and kg_builder binds them as `SceneData::accel`, so sessions trace real geometry on CPU and CUDA.
Read first: 0002 D11 (traversal, masks, SBT per primitive type), D13 (scene data, ledger); 0007 d9, d24 (pipelined sync, retirement), RULINGS.md 70 (uploads at the latch, pinned snapshots); src/device/cpu/embree_scene.h, src/device/cuda/optix_accel.h, src/host/session/kg_builder.h.
Scope: src/host/scene/scene_tables.{h,cpp}, src/host/session/kg_builder.cpp, src/device/cpu/embree_scene.cpp (construction from a snapshot only), src/device/cuda/optix_accel.cpp (construction from a snapshot only), tests/cpu/test_scene_accel.cpp, tests/gpu/test_scene_accel_gpu.cpp; added by lead ruling: src/device/cuda/optix_accel.{h,cpp} and src/device/cpu/embree_scene.{h,cpp} (per-snapshot shared structures), src/host/scene/scene.{h,cpp} (the scene owns its builder), src/capi/scene_api.cpp (OptiX builder made on the init thread), src/kernel/scene/scene_view.h (accel convention comment), src/device/cpu/intersect_cpu.cpp (throwing accel_of), src/host/session/kg_builder.h (comment); new src/host/scene/accel.h (host/device seam: lint forbids host code including device headers); call-site updates in tests/gpu/test_scene_upload.cpp (rig gets a builder, ledger check adds the structures) and tests/cpu/test_kg_builder.cpp (accel is now set, no longer asserted null).
Out of scope: refit and incremental TLAS updates (later); motion (M8); curves (M7).
Reference: Embree 4 API (rtcNewScene, rtcCommitScene, instancing); OptiX 9.1 Programming Guide (optixAccelBuild, compaction); both through the wrappers 037, 040 and 105 already provide.
Do:
1. At each latch that takes a new scene snapshot (102), build or reuse the traversal structures for that snapshot: unchanged mesh GAS are reused by identity, the TLAS is rebuilt when instances or lamps changed; the structures live in the snapshot and are freed when its last holder drops it (Ruling 70). A build failure follows Ruling 70 (previous snapshot kept, failed_generation set, one entry).
2. Lamp primitives come from the snapshot's LightTable through 105's explicit lamp parameter; mesh and instance tables from the snapshot.
3. `accel_of` (src/device/cpu/intersect_cpu.cpp, joins Scope for this) asserts only in debug builds; make a missing accel throw in every build so a release camera pass never dereferences null (rule 5). kg_builder sets `SceneData::accel` (Embree scene on CPU, TLAS handle on CUDA) for every session of the scene; nothing marks it unused once a snapshot exists.
4. Tests: CPU (gate): a session on a scene with one mesh, one rectangle lamp and one sphere lamp traces camera rays that hit each at analytic t; editing one instance transform rebuilds the TLAS and not the mesh GAS (build counters). GPU (gate): the same scene on CUDA gives the same hits as CPU (instance exact, t within 1e-5 relative); ledger totals include the structures.
Acceptance: `pwsh tools/check.ps1 gate` passes with `scene_accel/*` and `scene_accel_gpu/*`.
Look at: none.
Canary: tasks/114.canary.patch must turn red: astroray_tests_cpu "scene_accel/hits mesh and lamps", astroray_tests_gpu "scene_accel_gpu/matches cpu".
Budget: host code; build time per generation printed.
Depends on: 102, 105.
Size: M (about 300 lines); stop and report at 2x.
Progress:
- Seam: lint forbids host code including device headers, so host/scene/accel.h defines AccelBuilder/SnapshotAccel; the Scene owns the builder (Embree lazily on CPU, OptiX made in CudaInit).
- Embree scenes and OptiX GAS, and lamp unit shapes, are shared between snapshots by MeshRecord identity (shared_ptr, last holder frees); OptixAccel stays as a thin version-tracking view of the same builders.
- Unchanged instances, lamps and meshes reuse the previous snapshot's whole structure; an empty scene builds a valid empty structure.
- CUDA SceneData::accel holds the traversable handle value (scene_view.h); a scene with a device but no accel_of throws (no silent Embree fallback).
- Embree GAS are built serially: Scene::pool() is a job-queue WorkerPool, not a ThreadPool; Embree parallelizes each commit itself.
- Canary: CPU path drops lamp instances; CPU case red, GPU case red by disagreement with the CPU.
