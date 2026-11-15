# 060 Image ingestion: 8-bit decode, colour spaces and the image store
Goal: images cannot enter the engine -> `ar_edit_set_image` (FILE and MEMORY) decodes PNG, JPEG, TGA and BMP 8-bit images with stb_image on the scene workers, keeps them at 8-bit precision in their stored encoding with their colour space, computes the image mean, and an unreadable image or unsupported colour space renders magenta with a report entry.
Read first: 0007 d13 (textures at source precision, decoders), d21 (image sources, interop ids, cache key), d9 (decode failure outcome), 0005 item 11 (decode by interop id, `data` skips lifting), item 34 (filtering in stored encoding), 0001 D9.1 and R20 (stb_image only), 0007 V19.
Scope: third_party/stb/ (stb_image.h only, with LICENSE and VERSION), third_party/deps.toml, THIRD_PARTY.md, src/host/image/image_store.{h,cpp}, src/host/image/decode.{h,cpp}, src/capi/scene_api.cpp, src/capi/unimplemented.cpp, src/kernel/scene/texture_view.h, src/host/material/compiler.{h,cpp} (emission estimate from the image mean), tests/cpu/test_compiler_outputs.cpp (one case), tests/cpu/test_image_ingest.cpp, tests/data/images/ (small generated test images).
Out of scope: half and float images, HDR and EXR (tinyexr) (M1b); PIXELS source (float data: M1b; returns AR_ERR_UNSUPPORTED with a message until then); mip chains (M3); UDIM (M3); filtering (061).
Reference: stb_image (public domain or MIT, github.com/nothings/stb, pinned version recorded in deps.toml).
Do:
1. Vendor stb_image.h with its licence; deps.toml entry (kind vendored) and THIRD_PARTY.md regenerated; compile it in exactly one host TU with only the 8-bit loaders enabled.
2. FILE source reads the path, MEMORY source decodes the copied-in bytes; decode runs on the scene workers at item commit; channels 1, 3 or 4 kept as stored (no expansion), rows bottom-up (flip at decode to Blender's order).
3. Colour space by interop id: `srgb_rec709_scene` keeps sRGB-encoded bytes with an `SRGB` encoding tag; `lin_rec709_scene` and the Non-Color/data id keep `LINEAR`/`DATA`; any other id makes the image magenta with `COLORSPACE_UNSUPPORTED` (detail = interop id). Check the exact 5.3 interop id strings by reading `bpy.types.ColorManagedInputColorspaceSettings` in a headless Blender script and record them in Progress.
4. A decode failure makes that image a 1x1 magenta texture and raises `IMAGE_UNREADABLE` (detail = decoder error); the generation still activates (0007 d9).
5. Image mean (linear RGB, after decoding the transfer function) stored per image and passed to 059's emission estimate in place of its upper bound (extend 059's test with one textured-emission case). Cache key per 0007 d21 (path or packed hash, interop id, alpha mode, size, mtime, reload counter).
6. texture_view.h (kernel): views of texel bytes plus per-image width, height, channels, encoding; host arrays in the ledger textures category (CUDA upload: task 102).
7. Tests (fast): decoded bytes equal reference bytes for a generated 4x3 PNG, JPEG (quality 100, compared with stb's own decode of the same file) and TGA; rows are bottom-up; an unsupported interop id and a truncated PNG each give magenta and exactly one entry; the mean of a known image equals its analytic mean within 1e-6.
Acceptance: `pwsh tools/check.ps1 fast` passes with `image_ingest/*` cases; tests/data/images totals under 50 KB.
Look at: none.
Canary: tasks/060.canary.patch must turn red: astroray_tests_cpu "image_ingest/rows bottom up", "image_ingest/truncated png magenta".
Budget: none.
Depends on: 032, 008, 016, 059.
Size: M (about 400 lines); stop and report at 2x.
Progress:
- Blender 5.3.0 `colorspace_settings.interop_id` read headless: sRGB = `srgb_rec709_display` (Rec.709 has no `srgb_rec709_scene` in 5.3; the task's string is accepted too), Linear Rec.709 and scene_linear = `lin_rec709_scene`, Non-Color = `data`; also `lin_rec2020_scene`, `lin_ap1_scene` (ACEScg), `lin_ap0_scene` (ACES2065-1), `srgb_p3d65_display` (Display P3), `lin_p3d65_scene`.
- ar_image_desc.color_interop is a uint32 with no numbering in astroray.h: `ColorInterop` in src/host/image/image_store.h is the numbering until the header defines it (the header is outside this Scope).
- TEX_IMAGE's immediate is the image's ar_key (low 32 bits). The d21 reload counter has no ABI field: it is 0 in the cache key.
- Images are owned by MaterialStore (`materials->images()`), so scene.h is untouched; material_store.{h,cpp} changed (not in Scope) so image activation, removal and dependent recompiles ride the material pass.
- BLOCKED: tools/lint.py [deps] needs the vendored directory name to equal a deps entry name, but its allow-list has `stb_image` and the Scope names `third_party/stb/`; lint fails until the lead fixes one side.
