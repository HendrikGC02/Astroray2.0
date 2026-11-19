# 129 B-31 remainder on S2: instance loop, plain move, layout-check cost, mesh staging
Goal: after 128 the S2 first frame is within Cycles' (V7b 0.96x / 0.87x) but three B-31 rows still miss: the instance loop at 83 ms per 10k instances (limit 30; per-object murmur3 name hashes in Python, material slots, the real-data check, per-instance bookkeeping for 7,042 unique objects), the plain move at 0.17 ms (limit 0.1), and positions-only at 6.4 ms per 1M (limit 1.1), where 128's per-read layout check costs about 27 ms on S2; mesh staging overhead (`mesh.staged()` / `np.ctypeslib.as_array`, desc/begin/commit) is about 134 ms of S2's remaining 286-462 ms of mesh time -> bring the three rows within B-31 and cut the staging overhead, without weakening 128's layout guarantees.
Read first: 0007 d17, d22, d35 (128's note), 0009 B-31; 128's PR body (B-31 and V7b tables, what it left); blender/astroray/sync/ (instances.py, keys.py, mesh.py, topology_cache.py), blender/astroray/viewport.py and render_final.py (where a per-sync or per-session cache would live).
Scope: blender/astroray/sync/, blender/astroray/viewport.py and blender/astroray/render_final.py (a caller-owned set or cache for per-key work), tests/blender/headless/ (tests for every cache: invalidation on rename, relink, new data; first-read layout check still runs for a new or changed mesh key).
Out of scope: native-side changes; the layout check's content (only when it runs).
Reference: 0007 d17 (topology cache keys), d22; murmur3 as the keys module already cites it.
Do:
1. Instance loop: cache per-object identity work (name hashes, material slots, the real-data check) across builds keyed by session_uid and invalidated by the depsgraph's ID updates, or vectorise it; measure per 10k on S2 cold and warm.
2. Layout check once per mesh key per session (and again when the key's topology changes), not per read; a test proves a changed mesh is checked again.
3. Plain move to <= 0.1 ms on S2 (profile what remains of the 0.17 ms).
4. Staging: attribute and cut the per-mesh overhead.
5. Re-measure B-31 (094's method, S1 and S2 cold and warm) and V7b (095's harness, S1 and S2); PR body Result: with commands.
Acceptance: `pwsh tools/check.ps1 gate` passes; the headless suite passes on the bpy wheel; the PR body holds the tables.
Look at: none.
Canary: tasks/129.canary.patch must turn red: tests/blender/headless/test_instances_sync.py "TestMurmur3Many.test_bit_identical_to_murmur3_32" "TestInstanceCache.test_relink_data" "TestInstanceCache.test_data_linked_slot_change" "TestInstanceCache.test_random_ids_follow_their_instances" "TestPlainMoveFromClassify.test_evaluated_map_spares_the_updates_walk"; tests/blender/headless/test_mesh_sync.py "TestLayoutChecks.test_changed_topology_rechecks" "TestLayoutChecks.test_geometry_update_rechecks" "TestLayoutChecks.test_new_key_is_checked" "TestLayoutChecks.test_staged_read_matches_the_numpy_read" "TestLayoutChecks.test_positions_path_skips_only_a_checked_key"
Budget: none added to the gate.
Depends on: 128.
Size: M (about 250 lines); stop and report at 2x.
Progress:
- started on task/129-sync-remainder at 81553dd.
- Rulings (lead): 0007 d35 note and api_surface.txt comment joined Scope for the check wording; tools/budgets.toml and 0009's B-31 cell joined Scope for the positions-only limit only (S2 measured value x 1.3, bandwidth-bound); murmur3_32_many tested bit-identical with a canary hunk.
- Attribution (S2 warm): instance loop 81 ms = iteration/RNA 15, real-data check 17, murmur3 15, slot tables 8, bookkeeping ~25; plain move 0.157 ms = updates walk 63-126 us, Matrix->numpy 39 us, ctypes 27 us; viewport cache.remember after each delta ~25 ms.
- Done: InstanceCache (name-hash memo, vectorised murmur3, slot tables and real-data per uid, forget from classify's walk); LayoutChecks per key; raw staging (no NumPy views), bind once per Edit; sync_transforms takes classify's evaluated objects; viewport skips remember after a transform patch.
- B-31 S2 after (warm/cold): full 10.8/98.1 per 1M; positions 4.63/12.9 per 1M (memmove alone 2.25-2.59); instances 39.8/83.2 per 10k (missed, limit 30); plain move 0.066/0.108 ms; viewport plain-move delta 25.3 -> 0.175 ms.
- V7b after: S2 1441/1259 ms (0.84x/0.72x Cycles), S1 281/268 (1.8x/1.7x); S2 meshes 248/136, instances 82/80 (cold build, unchanged).