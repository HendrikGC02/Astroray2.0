# 058 Material compiler core: graph to bytecode
Goal: host-neutral op graphs cannot become programs -> the core compiler infers types, folds constants without crossing texture or context reads, removes dead code per entry, allocates the 32 registers by linear scan, lays out the entry table and the emission segment plus one BSDF block, and packs the constant block and SpectrumRefs.
Read first: 0005 item 29 (pipeline), items 30-32 (ops, encoding, entries), item 35 (folding), item 38 (closures and segments), item 10 (slot semantics), 0007 d8 (graph transport, `ar_graph_desc`); docs/decisions/0007-appendix-abi.md (`ar_graph_desc`).
Scope: src/host/material/graph.{h,cpp}, src/host/material/compiler.{h,cpp}, src/host/material/regalloc.{h,cpp}, tests/cpu/test_compiler.cpp.
Out of scope: material flags, emission estimate, magenta and report entries, and ABI wiring (059); selection between children (Mix and Add Shader: M1b, M2); volume, opacity and light entries (later); texture binding to image keys is recorded as a constant image index resolved by 060.
Reference: Cycles `src/scene/svm.cpp` and `src/scene/shader_graph.cpp` (Apache-2.0) for graph compile and folding structure; Poletto and Sarkar 1999, "Linear Scan Register Allocation", ACM TOPLAS 21(5).
Do:
1. graph: parse `ar_graph_desc` (op nodes with param ranges, f32/i32/string pools, links) into typed nodes; type inference over f32, f32x3 and spec; a type error or a cycle is a compile error (returned, not thrown across the ABI).
2. Folding: a node folds only if every input is constant and it reads no context and no texture (0005 item 35), using the CPU build of the op function from 057 (so folding cannot disagree with evaluation). A surface program that folds completely has length 0 and marks `CONSTANT_SURFACE`; its stores become constant-block values.
3. Dead-code elimination per entry; the `surface` entry is laid out as emission segment (everything feeding Le) then one BSDF block (record stores) ending in END; the `emission` entry is the emission segment alone; opacity and volume entries are 0 in M1a.
4. Linear-scan register allocation over 32 f32 registers (f32x3 takes three consecutive); a program needing more fails to compile ("needs <n> registers, 32 available").
5. Constant block packing with each slot's semantic from 056's table; RGB constants become RGB SpectrumRefs with host-precomputed coefficients (023), scalar constants above 1 in REFLECTANCE slots clamped and flagged for 059's report.
6. Tests (fast): a constant Principled-equivalent graph compiles to length 0 with the expected constant block; a texture-driven base colour keeps TEX_IMAGE, RGB_LIFT and STORE_SLOT and folds the rest; folding never crosses LOAD_CTX_UV; a 40-value live graph fails with the register message; compiled programs run through the interpreter (057) give the same record as hand-evaluated expectations on 3 graphs.
Acceptance: `pwsh tools/check.ps1 fast` passes with `compiler/*` cases; the test prints program lengths for the three graphs; B-24's compile row is not yet gated (059 adds it).
Look at: none.
Canary: tasks/058.canary.patch must turn red: astroray_tests_cpu "compiler/no folding across texture read", "compiler/register overflow rejected".
Budget: none.
Depends on: 057, 031.
Size: M (about 500 lines); stop and report at 2x.
Progress:
