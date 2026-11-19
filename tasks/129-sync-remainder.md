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
Canary: tasks/129.canary.patch must turn red: the cache-invalidation and recheck tests (name them in land format).
Budget: none added to the gate.
Depends on: 128.
Size: M (about 250 lines); stop and report at 2x.
Progress:
