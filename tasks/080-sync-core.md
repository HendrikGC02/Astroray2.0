# 080 Sync framework: keys, update classification and the headless suite
Goal: the addon has no sync structure -> sync is plain functions over a depsgraph, an edit wrapper and a cache; keys follow Cycles' real-object-data rule; every depsgraph update is classified by 0007's full table; and a headless suite runs against the `bpy` 5.3 wheel in hosted CI.
Read first: 0007 d16 (plain functions, `pip install bpy==5.3.0`), d18 (keys), d20 (classification table and order), d6 (zero-copy staging), 0007 V4 (classification cases), V5 (keys); 0009 d12 (`ci` runs the headless sync suite).
Scope: blender/astroray/sync/__init__.py, sync/keys.py, sync/classify.py, sync/edit.py, sync/cache.py, tests/blender/headless/test_keys.py, tests/blender/headless/test_classify.py, .github/workflows/ci.yml (bpy wheel step), tools/check.ps1 (`ci` step), third_party/deps.toml (bpy wheel hash, tool-python).
Out of scope: reading meshes, lamps, materials (081-083); rendering (084).
Reference: Cycles `intern/cycles/blender/sync.cpp` `BlenderSync::sync_recalc` and `BObjectInfo::is_real_object_data()` in `intern/cycles/blender/` (Apache-2.0); read both before implementing, as 0007 d18 and d20 require.
Do:
1. keys.py: geometry identity = original data `session_uid` when the evaluated data is that datablock's shared evaluated copy, else (original object `session_uid`, instance `persistent_id` tuple); object identity = original object `session_uid` (+ persistent_id for instances); a per-scene counter and dict map identity tuples to `ar_key` (no hashing).
2. classify.py: deduplicate `depsgraph.updates` by (ID type, original session_uid) into one bitmask per datablock and return actions in the fixed order settings, world, images, materials, lights, geometry, instances, camera; one explicit entry per ID type of 0007 d20's table; an unlisted type returns a full-resync action and a host diagnostic counted for the Diagnostics output.
3. edit.py: a context manager over `_abi.py`'s edit functions; `staged(array_ptr, dtype, shape)` returns `np.ctypeslib.as_array` views deleted at item commit.
4. Headless tests (in CI with the pinned `bpy` wheel installed into CI's Python by `check ci`; locally the same test files run inside Blender 5.3 through Invoke-ArBlender, since R32 allows no separate local environment): linked duplicates share one geometry key; a modifier makes per-object geometry keys; nested collection instances get distinct keys (0007 V5); one case per classification row that the wheel can produce (0007 V4 headless part; the GUI part runs in 085).
Acceptance: `pwsh tools/check.ps1 ci` with `$env:CI='true'` installs the pinned wheel and passes the headless tests; `pwsh -c ". tools/lib/blender.ps1; Invoke-ArBlender -Script tests/blender/headless/run_all.py"` passes locally.
Look at: none.
Canary: tasks/080.canary.patch must turn red: tests/blender/headless/test_keys.py "TestKeys.test_modifier_splits_geometry", tests/blender/headless/test_classify.py "TestClassify.test_unlisted_type_full_resync".
Budget: B-11 grows by the wheel install; report the CI duration.
Depends on: 079, 031, 009.
Size: M (about 400 lines Python); stop and report at 2x.
Progress:
