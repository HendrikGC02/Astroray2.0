# 056 Material layouts: header, constant block, SpectrumRef, OpenPBRParams, VmRegs
Goal: no material data structure exists -> the 16 B `MaterialHeader`, the constant block with all 41 OpenPBR inputs, the 24 B `SpectrumRef` with per-slot semantics, the 304 B `OpenPBRParams` record and the 128 B register file exist with static_asserts, and slot resolution evaluates CONST and RGB sources at four wavelengths.
Read first: 0005 items 1-3 (pin, three layouts, stack budget), item 9 (SpectrumRef kinds), item 10 (lift semantic per slot), item 2a (interior fields), item 32 (entry table), item 43 (header flags); 0003 d16 (conventions).
Scope: src/kernel/material/material_header.h, src/kernel/material/spectrum_ref.h, src/kernel/material/openpbr_params.h, src/kernel/material/constant_block.h, src/kernel/vm/vm_regs.h, src/kernel/material/slot_resolve.h, tests/cpu/test_material_layout.cpp (+ f64).
Out of scope: SpectrumRef kinds TABLE, BLACKBODY, CAUCHY_ABBE (M1b), SELLMEIER and GAUSSIAN (M4) beyond their enum values; the compiler (058); lobes (062-064).
Reference: OpenPBR Surface 1.1.2 specification (parameter list and defaults), https://academysoftwarefoundation.github.io/OpenPBR/.
Do:
1. MaterialHeader {program_offset u32, const_offset u32, ext u32 (ext_kind u8 | ext_index u24), flags u32}; flag bits `MAY_BE_DELTA`, `NON_OPAQUE`, `EXTENSION`, `READS_RAY_TYPE`, `CONSTANT_SURFACE`, `CONSTANT_EMISSION`; `static_assert(sizeof == 16)`.
2. SpectrumRef {kind u8, semantic u8, aux u16, index u32, p[4] f32} = 24 B; kinds CONST, RGB, TABLE, BLACKBODY, CAUCHY_ABBE, SELLMEIER, GAUSSIAN (values exist; only CONST and RGB resolve in M1a, the others make the compiler reject the program, 059); semantics REFLECTANCE, UNBOUNDED, ILLUMINANT, IOR.
3. constant_block.h: all 41 OpenPBR 1.1.2 inputs in spec order at their spec defaults, scalars f32 and colour and IOR inputs as SpectrumRef with the semantic of 0005 item 10's table; `interior_ior` and `medium_priority` (item 2a); a table `kSlotSemantic[]` used by the compiler.
4. openpbr_params.h exactly 0005 item 2's table (24 scalars, 9 spectral slots, 4 frame vectors, metal_ior, flags), padded to 304 B, `static_assert(sizeof <= 304)` at Real = float; vm_regs.h: `struct VmRegs { float r[32]; }` with `static_assert(sizeof == 128)`.
5. slot_resolve.h: `resolve(const SpectrumRef&, const Lambda&, tables) -> Spec`: CONST gives the value on every lane; RGB applies 023's convention chosen by the semantic (REFLECTANCE albedo with clamp, UNBOUNDED, ILLUMINANT); other kinds assert in debug and return zero (unreachable: the compiler rejects them).
6. Tests (fast): sizes 16, 24, 304 (or less), 128; constant-block defaults equal the spec table for 6 spot-checked inputs; RGB white REFLECTANCE resolves to 1 +- 1e-4 at four wavelengths; ILLUMINANT white resolves to the working-space illuminant; a constant above 1 in a REFLECTANCE slot is clamped (the compiler reports it, 059).
Acceptance: `pwsh tools/check.ps1 fast` passes with `material_layout/*` cases in float and double.
Look at: none.
Canary: tasks/056.canary.patch must turn red: astroray_tests_cpu "material_layout/record size", "material_layout/rgb white reflectance", "material_layout/slot semantic table", "material_layout/header packing and enums".
Budget: none.
Depends on: 055, 023.
Size: S (about 300 lines); stop and report at 2x.
Progress:
- Done: layouts and slot_resolve in src/kernel/{material,vm}; tests material_layout/* (f32+f64); canary tasks/056.canary.patch (field swap + RGB scale halved) turns 'record size' and 'rgb white reflectance' red.
- Choices to ratify: enum value CONSTANT (windows.h CONST macro); ext_kind in low 8 bits; flag bits 0-5 in listed order; geometry_thin_walled u32; vec3 frame default 0 = geometric frame; subsurface_radius default 0.01 m; no metal n,k pair in block (variable trailer, 058).
