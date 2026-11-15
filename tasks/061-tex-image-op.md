# 061 TEX_IMAGE: Closest and Linear filtering, Repeat and Extend
Goal: materials cannot read textures -> the TEX_IMAGE opcode samples 8-bit images with Closest and Linear interpolation and Repeat and Extend extension, filtering texels in their stored encoding and decoding afterwards as Cycles does, identically on CPU and GPU.
Read first: 0005 item 34 (texture lookups, filtering in stored encoding, software filtering), item 33 (context UV), 0002 D13 (`ar::load_texel`, no hardware filtering), 0005 Verification 6 (filter modes in the node-value check); src/kernel/scene/texture_view.h.
Scope: src/kernel/vm/ops/tex_image.h, src/kernel/portability.h (`ar::load_texel`), tests/cpu/test_tex_image.cpp, tests/gpu/test_tex_image_agree.cpp, tests/gpu/kernels/tex_agree.cu; lead rulings: src/kernel/vm/context.h, src/kernel/vm/ops/common.h, tests/cpu/test_vm.cpp, tests/cpu/test_compiler.cpp, tests/gpu/kernels/vm_agree.cu (image table in SceneTables, ruling 1); src/host/material/compiler.{h,cpp}, src/host/material/material_store.{h,cpp}, src/host/image/image_store.{h,cpp}, docs/decisions/0007-appendix-abi.md (ar_image_desc row) (key-to-slot resolution, ruling 2); src/kernel/vm/ops.def (TEX_IMAGE row), src/host/material/graph.cpp (second i32 parameter, ruling 4); tests/gpu/test_vm_agree.cpp (magenta-era case, ruling 5); src/kernel/color/srgb.h (the one sRGB decode, ruling 6).
Out of scope: Cubic, Smart, Clip, Mirror, projections, UDIM, mips and ray-cone LOD (M3); half and float images (M1b).
Reference: Cycles `kernel/svm/image.h` and `kernel/device/cpu/image.h` (Apache-2.0) for Closest/Linear on 8-bit sRGB images, pixel-centre convention and wrap modes.
Do:
1. `ar::load_texel(view, image, x, y) -> float4` reads raw bytes (no decoding) as 0-1 floats per channel; 1 and 3-channel images expand to RGBA as Cycles does (grey to RGB, alpha 1).
2. TEX_IMAGE operands: image index (constant), UV register, interpolation (CLOSEST or LINEAR), extension (REPEAT or EXTEND); outputs colour (f32x3) and alpha. Coordinates follow Cycles: texel centres at (i + 0.5)/w; Linear is bilinear at level 0; Repeat wraps, Extend clamps; filtering happens on the stored values, then sRGB-encoded images are decoded with the sRGB transfer function, linear and data images are not.
3. A missing image index or a magenta-marked image returns (1, 0, 1).
4. Tests: CPU (fast) on a 4x4 image: exact texel values at centres for both modes; Linear at a midpoint equals the encoded-space average decoded (not the decoded-space average); Repeat and Extend at u = -0.25 and 1.25; GPU (gate): 10^5 random lookups bitwise equal for Closest and within 1e-6 for Linear against CPU (nofma), through the agreement harness.
Acceptance: `pwsh tools/check.ps1 gate` passes with `tex_image/*` and `tex_image_agree/*`.
Look at: none.
Canary: tasks/061.canary.patch must turn red: astroray_tests_cpu "tex_image/filter in encoded space", "tex_image/extend clamps".
Budget: none yet; the shade kernel's register delta from texture code is declared in 071.
Depends on: 060, 057, 028, 029.
Size: S (about 250 lines); stop and report at 2x.
Progress:
- Started on task/061-tex-image from 23a3ee4 with the lead's rulings 1-8.
