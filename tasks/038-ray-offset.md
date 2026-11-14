# 038 Ray-origin offset and self-intersection for triangles and lamps (designed when built)
Goal: no record fixes how spawned rays avoid hitting their own surface -> this task decides, from a published method, the ray-origin offset, tmin, shadow-ray end handling and shadow-terminator policy for triangles and lamps, implements it once in kernel code and writes the decision into 0002.
Read first: R63 (designed when built: decide from a published source, write back in the same change), 0002 Risk 13 (the open question and candidates), 0002 D11 (lamps never block shadow rays; lamp pass-through re-queue), 0004 D8 (lamp crossing re-queues from just past the lamp), 0005 item 8 (front face, below-surface rejection).
Scope: src/kernel/geom/ray_offset.h, docs/decisions/0002-kernel-architecture.md (decision text replacing Risk 13 for triangles and lamps), tests/cpu/test_ray_offset.cpp.
Out of scope: curves, spheres as geometry and the random walk's local rays (M7, per the roadmap's designed-when-built table); bump-related terminator fixes (M3).
Reference: Waechter and Binder 2019, "A Fast and Robust Method for Avoiding Self-Intersection", Ray Tracing Gems ch. 6 (open access; implement from the paper's description, do not copy its listing: the book is CC BY-NC-ND); pbrt-v4 `OffsetRayOrigin` in `src/pbrt/interaction.h` and `ray.h` (Apache-2.0) as the considered alternative (per-hit error bounds).
Do:
1. Decide and implement `offset_ray_origin(p, ng, wo_side)`: the Waechter-Binder integer-ulp offset along the geometric normal, pushed to the side of the outgoing direction; tmin = 0 for every ray; the same function for continuation rays, NEE shadow rays (origin end) and lamp pass-through re-queues (offset along the ray direction past the lamp surface).
2. Shadow-ray far end: offset the sampled light point toward the shading point with the same function and set tmax to the distance between the two offset points; for lamps no offset is needed beyond that because lamp instances lack the SHADOW mask bit.
3. Shadow terminator: decide whether to apply a published correction in M1a (candidates: Hanika 2021, "Hacking the Shadow Terminator", Ray Tracing Gems II ch. 4; Chiang et al. 2019 bump terminator, used by Cycles). Record the decision and its reason (expected: none until bump and normal maps exist in M3; smooth-normal meshes may show the classic terminator artefact, which the reviewer is told to look for).
4. Write the decision into 0002 as a new decision `D16. Self-intersection (triangles and lamps)` and reduce Risk 13 to curves, spheres and the random walk; cite the paper and name this task.
5. test_ray_offset.cpp (fast and gate): a tessellated sphere of 10^5 triangles centred 10^4 m from the origin (float positions): 10^6 random rays spawned from surface points with the offset and traced with 037's `trace_closest` hit their own triangle zero times and never start below the surface; a second surface 1e-4 m away along the normal is still hit by rays toward it.
Acceptance: `pwsh tools/check.ps1 fast` passes; `out/bin/astroray_tests_cpu.exe --tier=gate -tc="ray_offset/*"` prints `self_hits=0` and `near_surface_found=<n>/<n>`; 0002's diff shows D16 and the shortened Risk 13.
Look at: none.
Canary: tasks/038.canary.patch must turn red: astroray_tests_cpu "ray_offset/no self hits at 1e4 m".
Budget: none.
Depends on: 018, 019, 037.
Size: S (about 200 lines plus record text); stop and report at 2x.
Progress:
