# 224 B-09's tier clock excludes the compile-timing step (ruling 100)
Goal: landing gates of kernel-changing tasks read 179.8 s (163, 182) against B-09's 180 s on a quiet machine because `compile timing` (kernelgate --time) really compiles on a rebuilt tree (23.2 s in 182's gate, 0.8 s cached) and counts on the tier clock -> the gate's B-09 clock excludes the compile-timing step's seconds as ruling 94 already excludes build steps, prints them with the build seconds, and the kernel part's 120 s keeps counting it. [lane tools; Sonnet; skip ok for Opus review: one-flag change, lead reads the diff]
Read first: docs/decisions/RULINGS.md 94 and 100; 0009 d13 row B-09 (as written by 204); tools/check.ps1 lines 45-52 (`$kernelSteps`, `Add-GateBuild`), 139 (the compile timing step) and 225-235 (`$s.Build`); tests/tools/fast_lock.ps1 (the B-09 accounting cases 204 added).
Scope: tools/check.ps1 (the compile timing step's seconds go through `Add-GateBuild` or an equivalent that keeps it in `$kernelSteps`; the summary line names it, e.g. "build steps 26.3 s not counted (compile timing 22.6 s)"), tests/tools/fast_lock.ps1 (one case: a slow compile-timing stand-in does not count against B-09 and still counts in the kernel part), docs/decisions/0009-build-test-process.md (row B-09 text: write-back of ruling 100, ruling 87), tasks/224.canary.patch.
Out of scope: B-05's limits, the 180 s limit and the 170 s hold line, Blender smoke's cold start (BACKLOG if measured), kernelgate itself.
Reference: ruling 100.
Do:
1. The exclusion in check.ps1; the kernel part unchanged.
2. The fast_lock case.
3. 0009 B-09 text.
Acceptance: `pwsh tools/check.ps1 fast` and `pwsh tools/check.ps1 gate` pass, the gate's last line showing the compile-timing seconds among those not counted and the kernel part still including them; `pwsh -NoProfile -File tests/tools/fast_lock.ps1` passes; paste the last lines.
Look at: none.
Canary: tasks/224.canary.patch must turn red: `pwsh -NoProfile -File tests/tools/fast_lock.ps1` (the patch counts compile timing on the tier clock again).
Budget: gate +0.3 s at most (fixture stand-in); no limit moves.
Depends on: none. Collisions: tools/check.ps1 with 210 (row c) and 212; this lands first.
Size: S (about 30 lines); stop and report at 2x.
Progress:
