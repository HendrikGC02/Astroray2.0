# 035 Mesh ingestion: staged buffers, validation and the host mesh record
Goal: meshes cannot enter the engine -> `ar_edit_mesh_begin` and `ar_edit_positions_begin` hand out staged arrays, a worker validates and triangulates each committed mesh into a host mesh record, and invalid input drops the generation with a report entry naming the mesh.
Read first: 0007 d9 (O(n) validation on workers, outcomes), d10 (geometry record, corner-indexed lookups), d12 (edit map rows for deformation and other geometry changes), d22 (positions-only path), docs/decisions/0007-appendix-abi.md (`ar_mesh_desc`, `ar_positions_buffers`), 0007 V15 (invalid indices, NaN positions).
Scope: src/host/scene/mesh.{h,cpp}, src/capi/scene_api.cpp, src/capi/unimplemented.cpp, tests/cpu/test_mesh_ingest.cpp, tests/abi/test_mesh_api.py.
Out of scope: device upload and acceleration structures (037, 040); tangents; deformation blur (S > 1 is stored, only the shutter-centre step is used until M8); curves and points (M7).
Reference: none.
Do:
1. `ar_edit_mesh_begin`: validates the descriptor's counts (V, C, F, U, S, attribute list) and returns staged pointers for positions S x V x 3, corner_vert C, face_start F, corner_normals C x 3, uv U x C x 2, material_index F (only with HAS_MATERIAL_INDEX) and attributes. `ar_edit_positions_begin`: for an existing key, staged positions and corner normals of its current V and C.
2. Worker job at item commit: O(n) checks (every corner_vert in [0, V), face_start starts at 0 and is strictly increasing with every face >= 3 corners and the last start < C, all positions and normals finite, material_index >= 0) with a failure detail naming the first bad element; triangulate with task 034; build the host mesh record: triangles as corner triples, per-corner normals and UV sets, per-triangle material slot, bounds, triangle count; record host bytes in the ledger's geometry category.
3. Positions-only commits keep topology and update positions and normals; a count mismatch fails validation.
4. A failure rejects the whole generation through 032's mechanism with `GENERATION_REJECTED` (subject = mesh key, detail = failed check).
5. Tests: test_mesh_ingest.cpp (fast): a quad mesh triangulates into the expected corner triples; an out-of-range corner_vert, a NaN position and a non-monotonic face_start each reject their generation and keep the previous mesh; positions-only update keeps triangles identical; test_mesh_api.py drives the same cases through `_abi.py` with numpy views on the staged arrays.
Acceptance: `pwsh tools/check.ps1 gate` passes; `<python> -m unittest discover -s tests/abi -p "test_mesh_api.py"` passes and prints the rejected generation's report detail for the NaN case.
Look at: none.
Canary: tasks/035.canary.patch must turn red: astroray_tests_cpu "mesh/nan position rejected", "mesh/corner_vert out of range rejected".
Budget: none.
Depends on: 032, 034.
Size: M (about 350 lines); stop and report at 2x.
Progress:
