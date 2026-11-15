# 083 Material and world translation and node coverage
Goal: Blender materials and the world do not reach the engine -> the addon walks `inline_shader_nodes()`, lowers the M1a node set (and the World's Background) into host-neutral op graphs through one graph emitter, emits an `UNSUPPORTED` op for every node, socket or setting without a lowering (magenta plus one report entry), caches translations by session_uid, and the coverage matrix has a row for every ShaderNode type.
Read first: 0005 item 29 (pipeline, translation cache), item 40 (unsupported-node policy), item 41 (coverage, no-ops), item 42 (mapping rows: Principled base colour, roughness, Specular IOR Level, emission), 0007 d8 (graph transport); 0005 Verification 10; 0007 V20.
Scope: blender/astroray/materials/translate.py (with the graph emitter), materials/lowering_table.py (replaces 055's empty table at the same path), blender/astroray/sync/world.py (moved here from 082), tests/blender/test_materials.py.
Out of scope: image sync and the node-value bake (110); Mix Shader, Emission node, Principled metallic, alpha and other inputs (M1b); procedural and colour nodes (M3); custom nodes (P1, M3).
Reference: Blender's node semantics from the Blender tree (GPL, allowed in the extension per 0001 D7.2; cite files); 0005 item 42's table.
Do:
1. translate.py: `material.inline_shader_nodes()` on the evaluated material, walk from the active Material Output's Surface; lower: Principled BSDF (Base Color constant or linked, Roughness constant or linked, Specular IOR Level L as `specular_weight = 2L` (0005 item 42; A1's .blend uses L = 0, task 084), Emission Color, Emission Strength; every other input at a non-default value or linked emits `UNSUPPORTED` naming "Principled BSDF.<input>"), Image Texture (Linear or Closest, Repeat or Extend, flat projection, its image key from sync/keys.py, the image itself sent by 110; other settings -> UNSUPPORTED), Texture Coordinate UV and UV Map (UV set 0 only), Material Output. Anything else -> `UNSUPPORTED` with the node type in the string pool. Graph emitted with `ar_edit_set_graph(SURFACE)` and `ar_edit_set_material`.
2. Translation cache keyed by the original material's session_uid; re-translated only when classification marks the material or a group in its raw tree changed.
3. lowering_table.py: one row per lowered node type with its disposition, consumed by opsgen's coverage matrix (055).
4. world.py: the World's node tree lowered for the constant case (Background colour x strength) as a WORLD graph through the same emitter; anything else is emitted with an `UNSUPPORTED` op so the engine reports it (067).
5. Tests (Invoke-ArBlender, CPU device): test_materials.py: a material with two Glass BSDF nodes and one Toon BSDF renders magenta with exactly two `NODE_UNSUPPORTED` entries (0007 V20, 0005 V10 policy part); coverage.csv rows = shader_node_types_5_3.txt rows, none blank (0005 V10 matrix); a Principled with Specular IOR Level 0 lowers to specular_weight 0 and renders without magenta; a World with a Noise Texture into Background raises one NODE_UNSUPPORTED.
6. Compiler contract from 058: emit constant colours as F32X3 straight into STORE_SLOT (the slot lifts them) and use RGB_LIFT only on the texture path; RGB_LIFT never folds, so a translator that lifts every colour stops constant Principled graphs reaching length 0. One colour or strength value feeding two EMIT_LE nodes is a compile error in M1a: do not dedupe constants across emitters.
Acceptance: test_materials.py passes through Invoke-ArBlender (added to `check full` and to the gate's Blender step in 084).
Look at: none.
Canary: tasks/083.canary.patch must turn red: tests/blender/test_materials.py "TestMaterials.test_two_node_types_two_entries", "TestMaterials.test_specular_ior_level_lowered".
Budget: translation time per material is measured in 095 (M-8).
Depends on: 082, 059, 061, 071.
Size: M (about 350 lines Python); stop and report at 2x.
Progress:
