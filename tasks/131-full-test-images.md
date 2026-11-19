# 131 check full red on main: blender test_images rejects a mesh with non-finite positions
Goal: 091 found `pwsh tools/check.ps1 full` failing on main at its `blender test_images` step (4 errors, including `GENERATION_REJECTED: mesh 'sphere 0' positions[0] is not finite`); the gate does not run that step, so a landed change broke it unseen (suspects by date: 128 and 129, the direct mesh reads and the sync caches) -> find the commit, fix the cause (not the test), and get `check full` green; then measure B-10, which 091 could not.
Read first: 091's report (in its PR body once it lands); tests/blender/test_images.py; blender/astroray/sync/mesh.py, instances.py, keys.py (128, 129); 128's and 129's PR bodies (`gh pr view 98`, `gh pr view 101`); 0009 B-10.
Scope: blender/astroray/sync/ and blender/astroray/render_final.py / viewport.py (the fix), tests/blender/headless/ (a regression test that reproduces the defect without Blender's GUI where possible), tools/budgets.toml and docs/decisions/0009-build-test-process.md (B-10 only, from measurement with >= 30 % headroom).
Out of scope: changing tests/blender/test_images.py's expectations (only if it is itself wrong, and then stop and ask first).
Reference: none beyond the records named.
Do:
1. Reproduce: run test_images through Invoke-ArBlender on main and on 81553dd^ (before 128) and 81553dd (128) and ff16141 (129), each in a worktree built with its own tools/build.ps1; name the first bad commit.
2. Find the cause (for example a direct read of an evaluated mesh whose pointer outlived its data, a staged-array offset, or a cache that kept an old layout) and fix it in the sync code; add a headless regression test that fails before the fix.
3. Run `pwsh tools/check.ps1 full` to the end; report every failing step if more than test_images fails (fix only this defect; report the rest).
4. B-10: three full runs' durations (excluding lock waits); reset the row only with >= 30 % headroom, else report.
Acceptance: `pwsh tools/check.ps1 full` passes; `pwsh tools/check.ps1 gate` passes; the headless suite passes on the bpy wheel.
Look at: none.
Canary: tasks/131.canary.patch must turn red: the regression test (name it in land format).
Budget: none added.
Depends on: 129.
Size: S (fix plus test); stop and report at 2x.
Progress:
- started
