# 059 Compiler outputs, magenta policy and material ABI wiring
Goal: compiled programs carry no flags and failures are silent -> the compiler emits MAY_BE_DELTA, NON_OPAQUE, EXTENSION, READS_RAY_TYPE, constant flags and the emission estimate, turns every unsupported node or failed compile into a magenta material with one report entry, and `ar_edit_set_graph`/`ar_edit_set_material` build device material arrays on the scene workers.
Read first: 0005 item 40 (unsupported-node policy), item 43 (compiler outputs), item 4 (`lobe_is_delta` threshold 1e-3), 0001 D2.11 (`NODE_UNSUPPORTED`, `NODE_APPROXIMATED`, `PROGRAM_REJECTED`), 0007 d8-d9 (graph transport, outcomes), 0009 B-24 (compile row).
Scope: src/host/material/compiler.{h,cpp}, src/host/material/material_store.{h,cpp}, src/capi/scene_api.cpp, src/capi/unimplemented.cpp, src/kernel/scene/scene_view.h (material header and program arrays), tools/check.ps1 (B-24 compile row in gate), tests/cpu/test_compiler_outputs.cpp, tests/abi/test_material_api.py.
Out of scope: Blender-side lowering (083); textures' image binding (060); TABLE/BLACKBODY/CAUCHY_ABBE sources (M1b: rejected with PROGRAM_REJECTED "SpectrumRef kind <k> lands in M1b").
Reference: none.
Do:
1. Flags: `MAY_BE_DELTA` unless every specular and coat roughness folds to a constant >= 1e-3 (any per-point roughness sets it); `NON_OPAQUE` clear in M1a (alpha lands in M1b; geometry_opacity is fixed at 1); `EXTENSION` clear; `READS_RAY_TYPE` when a Light Path read is reachable (none in M1a); `CONSTANT_SURFACE`, `CONSTANT_EMISSION`.
2. Emission estimate (0005 item 43): exact band-summable spectrum for constant emission; a texture-driven input replaced by the image mean (task 060 stores it per image key and wires it in; until then the declared upper bound 1 is used); any other per-point input by 1; zero only when emission folds to 0.
3. Magenta policy: an `UNSUPPORTED` op anywhere (detail = node type from its string), an unknown op, a SpectrumRef kind not resolvable in M1a, or a compile error replaces the material with diffuse magenta (base_color (1,0,1) REFLECTANCE, nothing else) and raises exactly one entry per (material, node type): `NODE_UNSUPPORTED` for unsupported nodes, `PROGRAM_REJECTED` for compile errors; a clamped REFLECTANCE constant raises `NODE_APPROXIMATED` ("RGB clamped").
4. `ar_edit_set_graph(SURFACE)` and `ar_edit_set_material` store their descriptors; the material job on a worker compiles and writes header, program words and constant block into scene arrays (host arrays in the ledger tables category; the CUDA upload is task 102's); recompiling a material is an upload, never a kernel compile.
5. check.ps1 `gate` times compiling a fixed 100-node fixture graph and fails above B-24's native-compile limit (2 ms per 100 nodes).
6. Tests: test_compiler_outputs.cpp (fast): MAY_BE_DELTA for roughness 0, 1e-4 and textured; emission estimate for constant and textured emission; two instances of one unsupported node type plus one other in one material give two NODE_UNSUPPORTED entries (0001 V15 part); test_material_api.py submits graphs through `_abi.py` and reads the entries through a session report.
Acceptance: `pwsh tools/check.ps1 gate` passes and prints the B-24 compile time; `<python> -m unittest discover -s tests/abi -p "test_material_api.py"` passes.
Look at: none.
Canary: tasks/059.canary.patch must turn red: astroray_tests_cpu "compiler_outputs/two node types two entries", "compiler_outputs/may be delta textured roughness".
Budget: B-24 compile row now gated.
Depends on: 058, 016, 104.
Size: M (about 400 lines); stop and report at 2x.
Progress:
- Started: compiler outputs and magenta policy in compiler.cpp; asked lead for scene.h (store member) and report activation order.
