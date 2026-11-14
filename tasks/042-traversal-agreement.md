# 042 Traversal agreement: Embree vs OptiX
Goal: CPU and GPU traversal are not compared at scale -> 10^6 fixed rays per test scene give at most 1e-4 hit/miss mismatches, all at triangle edges, and the mask fixture holds on both backends.
Read first: 0002 D11 verification row (traversal agreement, mask fixture), D12 (determinism); 0009 d17 (ladder); tests/gpu/agree.h.
Scope: tests/gpu/test_traversal_agree.cpp, tests/common/procedural_meshes.{h,cpp}.
Out of scope: transparent-shadow tests (M2); any shading.
Reference: Woop, Benthin, Wald 2013, "Watertight Ray/Triangle Intersection", JCGT (why edge mismatches are expected).
Do:
1. procedural_meshes: a displaced 1,000 x 500 grid (about 1.0 M triangles, the shape of spike B's S1, built in C++ with a fixed seed) and a 10-instance scene of spheres and boxes with mixed transforms.
2. For each scene, 10^6 rays from a fixed host-generated set (origins on a box around the scene, directions toward random interior points, plus 10% shadow-style rays with finite tmax): trace with Embree (robust) and OptiX; count hit/miss mismatches; for each mismatch compute the minimum barycentric of the hit side; mismatches <= 1e-4 of rays and every one within 1e-4 of an edge; for rays both hit, same instance and primitive except at edges, t within 1e-5 relative.
3. Mask fixture on both backends: one instance per mask bit 0-5; rays of each type hit exactly the instances sharing a bit.
Acceptance: `out/bin/astroray_tests_gpu.exe --tier=gate -tc="traversal_agree/*"` passes and prints mismatches per scene and the largest edge distance; `pwsh tools/check.ps1 gate` passes within B-09.
Look at: none.
Canary: tasks/042.canary.patch must turn red: astroray_tests_gpu "traversal_agree/grid mismatches at edges only".
Budget: about 2 s of gate GPU time; report it.
Depends on: 041.
Size: S (about 250 lines); stop and report at 2x.
Progress:
