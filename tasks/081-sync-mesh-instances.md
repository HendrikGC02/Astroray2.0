# 081 Mesh and instance sync
Goal: Blender geometry does not reach the engine -> meshes are read with the measured fast attribute path straight into staged arrays, the topology cache sends positions only where topology provably cannot have changed, and one pass over `depsgraph.object_instances` fills the instance descriptor with transforms, visibility, light group and material tables.
Read first: 0007 d17 (mesh read, `.corner_vert` fallback), d19 (per-instance loop), d22 (topology cache: provable cases only), d12 (edit map), docs/decisions/0007-appendix-abi.md (`ar_mesh_desc`, `ar_instances_desc` as decided in 031); docs/spikes/B-blender-end-to-end/RESULTS.md section 2 (fast paths).
Scope: blender/astroray/sync/mesh.py, sync/instances.py, sync/topology_cache.py, tests/blender/headless/test_mesh_sync.py, tests/blender/headless/test_instances_sync.py.
Out of scope: curves, points and volumes (later milestones); motion blur (S = 1 always in M1a); lamps, materials and camera (082-083).
Reference: spike B `syncmod.py` and `sync_bench.py` (read with `git show spikes-final:spikes/B-blender-end-to-end/syncmod.py`) for the measured read calls.
Do:
1. mesh.py: `attributes['position']` float32, `attributes['.corner_vert']` int32 (fallback `loops.foreach_get('vertex_index')` with a console warning and a host diagnostic when the internal name is missing), `polygons.foreach_get('loop_start')`, `corner_normals` float32, UV layers float32, `material_index` only if the attribute exists; all into the staged arrays of `ar_edit_mesh_begin`; never `loop_triangles`, per-element RNA or float64 destinations. Curves, text, surfaces and metaballs through `to_mesh()`/`to_mesh_clear()`.
2. topology_cache.py: per geometry key (V, C, F); positions and corner normals only (`ar_edit_positions_begin`) in exactly 0007 d22's two cases (empty or deform-only modifier stack from the fixed list with no geometry update on the original Mesh; or only an allow-listed modal operator running per `context.window.modal_operators`); otherwise a full read; a count mismatch on the fast path does a full read and adds a host diagnostic.
3. instances.py: one pass over `depsgraph.object_instances` filling preallocated NumPy arrays (`np.asarray(inst.matrix_world, np.float32)` as 3x4 rows, scene-origin translation folded in per 0003 d26), visibility bits from `Object.visible_camera/diffuse/glossy/transmission/shadow/volume_scatter`, `lightgroup` mapped to a u8 index, pass_index, random_id, material slots to a material-key table; per-object properties read once per original object and cached; a map from original object session_uid to instance indices makes a plain move one `ar_edit_update_transforms` call.
4. Headless tests: a cube, a quad-and-ngon mesh and a mesh with two UV maps sync with expected counts and corner arrays; a deform-only modifier with an armature pose change takes the fast path, a Subdivision modifier does not (decision-function unit tests; 0007 V6 is M3); instance arrays for linked duplicates and a collection instance; a plain move issues only update_transforms.
Acceptance: the headless suite passes in `check ci` and through Invoke-ArBlender; a printed timing of S1-sized mesh read (1M triangles) is within 2x of spike B's 65 ms (full measurement in 094).
Look at: none.
Canary: tasks/081.canary.patch must turn red: tests/blender/headless/test_mesh_sync.py "TestMesh.test_subdivision_takes_full_read", tests/blender/headless/test_instances_sync.py "TestInstances.test_move_only_updates_transforms".
Budget: none on tiers beyond CI time.
Depends on: 080, 035, 036.
Size: M (about 400 lines Python); stop and report at 2x.
Progress:
- mesh.py, topology_cache.py, instances.py and test_mesh_sync.py, test_instances_sync.py added; headless suite 37 ok on Blender 5.3; 1M-triangle read 66.5 ms (bound 130).
- Engine glue (sync_mesh, read_geometry, set_instances) is untested headless: no DLL in CI; the first engine run is 082/094.
- Material keys: default is KeyMap(('material', uid)); an empty slot is key 0. 082 may pass its own material_key.
