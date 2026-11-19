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
Canary: tasks/131.canary.patch must turn red: tests/blender/headless/test_render_final_dirs.py "TestUserDirs.test_cache_dir_follows_the_replaced_log_path"
Budget: none added.
Depends on: 129.
Size: S (fix plus test); stop and report at 2x.
Progress:
- started
- bisect (test_images via Invoke-ArBlender, own builds): 6c2276c OK; c50168f (124), 924e170, 81553dd (128), ff16141 (129), main FAILED errors=4, all `ValueError: The "package" does not name an extension` from render_final.cache_dir (124); GENERATION_REJECTED not reproduced.
- fix: cache_dir() is _log_path()'s directory (same path in production), so test_images' _log_path stand-in covers it; headless test_render_final_dirs; canary red alone.
- full: run 2 PASS 705.2 s, run 3 PASS 1891.7 s (about 198 s and 1395 s of Blender machine-lock waits that check.ps1 does not count, so about 505 s and 497 s of work); run 1 stopped by a hung astroray_tests_gpu (gate); runs 4 and 5 stopped by test_cuda_session_trace "no cache dir" failing on a stale OptiX cache WAL. B-10 not reset: only two clean runs.
- the GENERATION_REJECTED 'sphere 0' line is smoke_a1's test_error_entry_fails_render output (an intentional NaN vertex), not a failure.
- after 132 (rebased on ef31f7b): full run 6 PASS 912.2 s, waited 41.4 s, about 357 s Blender lock waits, about 514 s net; B-10 reset to 12 min (1.3 x 514 s = 668 s, rounded up to a whole minute).
