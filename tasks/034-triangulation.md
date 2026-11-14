# 034 Native triangulation with Blender's quad rule and ear clipping
Goal: polygons cannot be turned into triangles the way Blender does -> golden data captured from Blender's `loop_triangles` fixes the quad rule before implementation, and a host triangulator copies triangles, splits quads identically to Blender and ear-clips n-gons on the Newell-projected polygon.
Read first: 0007 d10 (triangulation rule, golden test first), V2; 0001 D7.2 (Blender source is study-only for the core); tools/lib/blender.ps1.
Scope: tests/blender/capture_triangulation.py, tests/data/triangulation/golden.json, src/host/geometry/triangulate.{h,cpp}, tests/cpu/test_triangulate.cpp.
Out of scope: mesh ingestion and validation (035); tangents (MikkTSpace, later).
Reference: D. Eberly, "Triangulation by Ear Clipping", Geometric Tools 2002; Newell's polygon normal (Sutherland, Sproull, Schumacker 1974); Blender's tessellation behaviour is taken only from the captured data (Blender code is GPL and may be read for facts, never copied).
Do:
1. capture_triangulation.py (run with `Invoke-ArBlender`, writes JSON): builds fixture meshes in headless Blender 5.3: a triangle; planar convex quads in several shapes; non-planar quads; concave quads with the reflex vertex at each of the four positions; convex and concave n-gons with 5 to 12 vertices; a star; a polygon with collinear vertices; for each polygon records vertex positions, corner order and `loop_triangles` corner indices. The JSON is committed (small).
2. From the quad data, determine the rule Blender uses to pick the diagonal (state it in a comment with the evidence, e.g. which diagonal for which shape class) and implement it; if no simple rule reproduces every captured quad, stop and report the counterexample rather than guessing.
3. `triangulate(positions, corner_vert, face_start) -> tri corner indices`: triangles copied; quads by the rule; n-gons with 5+ corners by ear clipping on the polygon projected along its Newell normal (Eberly 2002), deterministic tie-breaking, no degenerate output triangles.
4. test_triangulate.cpp (fast), reading golden.json: triangles and quads index-identical to Blender; each n-gon's triangles cover the same area within 1e-6 relative and none is degenerate (0007 V2).
Acceptance: `pwsh -c ". tools/lib/blender.ps1; Invoke-ArBlender -Script tests/blender/capture_triangulation.py -Args tests/data/triangulation/golden.json"` reproduces the committed JSON; `pwsh tools/check.ps1 fast` passes with `triangulate/*` cases.
Look at: none.
Canary: tasks/034.canary.patch must turn red: astroray_tests_cpu "triangulate/quads match blender", "triangulate/ngon area".
Budget: none.
Depends on: 004, 006.
Size: M (about 350 lines); stop and report at 2x.
Progress:
