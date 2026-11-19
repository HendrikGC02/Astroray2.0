# 128 Main-thread mesh and instance sync on S2 (0007 d35 remedy 1, B-31)
Goal: 095's V7b puts Astroray at 2.4x / 1.8x Cycles on S2 (first / second switch) and 3.0x on S1, with main-thread mesh reads 65-70 % of S2's first frame (2.7 s cold, 1.8 s warm); 094's B-31 breaches agree (S2 full read 180 ms cold per 1M triangles against 65, instance loop 140-151 ms per 10k against 30, a plain move 8.2 ms against 0.1 because sync_transforms walks all 10,001 instances) -> apply d35's first remedy, `as_pointer()` topology reads, and fix the instance loop and the plain-move path, then re-measure B-31 and V7b.
Read first: 0007 d35 (remedies in order; `as_pointer()` is version-fragile), d17 and d22 (topology cache, reads, modal fast path), d46 (API-surface check: a DNA read is a fragile point to list), d23 (main thread); 0009 B-31; 094's and 095's PR bodies (B-31 table, V7b phase breakdown); blender/astroray/sync/ (mesh, instances, topology_cache), tools/api_surface.py and blender/astroray/api_surface.txt.
Scope: blender/astroray/sync/ (mesh reads, instance build, sync_transforms), blender/astroray/api_surface.txt (the DNA layout and attribute names the direct read depends on), tests/blender/headless/ (equivalence tests: direct reads byte-identical to the attribute-API reads on the golden meshes), tests/blender/ (a GUI or headless timing case if needed), docs/decisions/0007-blender-integration.md (d35 note with the measured result; d17's '.corner_vert' fallback sentence, lead ruling: the fallback is replaced by the same loud error).
Out of scope: the GAS disk cache (d35 remedy 2) and parallel material inlining (remedy 3), unless V7a after this task still fails (the lead writes them); native-side changes.
Reference: Blender 5.3 source (Apache-2.0 / GPL headers read for layout only; cite the struct and field names); Malt's as_pointer() reads as d35 cites; 0007 d17.
Do:
1. Direct reads via `as_pointer()` of the corner-vertex and loop-triangle arrays (and positions if it pays), guarded by a runtime layout check against the attribute API on the first read of each mesh (mismatch -> fail loudly with the Blender version, never silently fall back); the API surface lists every field it depends on.
2. The instance loop and sync_transforms: a plain move touches only the moved objects (depsgraph updates name them), not all instances; the instance build reads matrices in bulk (foreach_get or equivalent).
3. Tests: byte-identical topology and positions between the direct and the API reads on the golden triangulation meshes and b1m; a plain move sends only the moved instances.
4. Re-measure B-31 (S1, S2 cold and warm, 094's method) and V7b (095's harness): before/after tables; PR body Result: with commands.
Acceptance: `pwsh tools/check.ps1 gate` passes; the headless suite passes under the bpy wheel (check ci); the PR body holds the tables.
Look at: none.
Canary: tasks/128.canary.patch must turn red: tests/blender/headless/test_mesh_direct.py "TestDirectEqualsApi.test_golden_triangulation_faces" "TestDirectEqualsApi.test_b1m" "TestDirectEqualsApi.test_positions_path" "TestLayoutCheck.test_good_layout_copies" "TestLayoutCheck.test_stride" "TestLayoutCheck.test_not_contiguous" "TestLayoutCheck.test_value" "TestLayoutCheck.test_attribute_type" tests/blender/headless/test_mesh_sync.py "TestMesh.test_missing_corner_vert_raises_with_version" tests/blender/headless/test_instances_sync.py "TestInstances.test_bulk_matrices_match_matrix_world" "TestInstances.test_plain_move_reads_only_the_moved_object" "TestInstances.test_instancer_move_walks_its_instances"
Budget: none added to the gate.
Depends on: 094, 095.
Size: M (about 300 lines); stop and report at 2x.
Progress:
- started
- lead ruling: element-pointer reads + memmove, no struct offsets; layout check per read (stride, contiguity, first/last values); d17's '.corner_vert' -> loops fallback replaced by the loud error (d17 joins Scope)
