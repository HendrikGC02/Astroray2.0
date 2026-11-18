# 110 Image sync from Blender and the node-value bake
Goal: Blender images do not reach the engine and texture lookups are unchecked against Blender -> the addon sends each image as FILE, MEMORY or PIXELS with its interop id, engine decoding matches `image.pixels` exactly for 8-bit PNG, JPEG and TGA, and textured emission matches Blender's own evaluation.
Read first: 0007 d21 (image sources, interop ids, cache key), d13 (source precision), V19; 0005 item 34 (filtering in stored encoding), 0005 Verification 6 (node values against Blender's bake); blender/astroray/materials/translate.py (image keys, 083).
Scope: blender/astroray/sync/images.py, blender/astroray/sync/__init__.py (images before materials), tests/blender/test_images.py, tests/blender/test_node_values.py.
Out of scope: generated, half and float images (M1b); UDIM and mips (M3).
Reference: none (Blender behaviour from its API documentation; v1's emission-plane bake method, docs/v1-lessons).
Do:
1. images.py: FILE when file-backed, not packed and not dirty (`filepath_from_user()`); MEMORY for packed files (`packed_file.data`); PIXELS for generated or dirty images (the engine returns AR_ERR_UNSUPPORTED until M1b; the addon reports "generated images land in M1b"); interop id from `colorspace_settings.interop_id`; keyed by the original session_uid with the reload counter in the cache key (0007 d21).
2. test_images.py (Invoke-ArBlender, CPU device): engine decode equals `image.pixels` (8-bit exact after decode) for PNG, JPEG and TGA in sRGB and Non-Color, read through an emission-plane render with Closest filtering; an unsupported interop id renders magenta with one entry (0007 V19).
3. test_node_values.py: emission-plane bake at 1 spp (v1 method): Principled emission colour from a texture through Closest and Linear matches Blender's own evaluation within 1e-4 absolute (0005 V6 node values for implemented ops).
4. From 060: 0007 d21's cache key includes a reload counter, but `ar_image_desc` has no field for it (060 uses 0). Add the field (include/astroray/astroray.h, src/host/image/image_store.cpp and docs/decisions/0007-appendix-abi.md join Scope for it), fill it from the image's reload count, and test that a reload with an unchanged mtime re-decodes.
5. From 083: translate.py keys an Image Texture's image as `keymap.key(("image", image.original.session_uid))`; images.py uses that same key (keys.py has no image helper yet; adding one moves both call sites).
Acceptance: both test files pass through Invoke-ArBlender (added to `check full`).
Look at: out/evidence/110/node_values.png (bake comparison).
Canary: tasks/110.canary.patch must turn red: tests/blender/test_images.py "TestImages.test_png_srgb_exact", tests/blender/test_node_values.py "TestNodeValues.test_linear_texture".
Budget: none on tiers.
Depends on: 083, 060.
Size: M (about 250 lines Python); stop and report at 2x.
Progress:
- started (implementer)
- images.py, test_images.py (13 cases), test_node_values.py (3), ar_image_desc.reload_count, canary patch (16 red) done on task/110-image-sync.
- Open for lead: (1) V6 1e-4 is below the spectral engine's own round trip (1e-5..2.7e-4), test uses 3e-4; (2) engine JPEG (stb) differs from Blender's by up to 2 codes on ~4% of texels; (3) render_final.full_sync does not call images.sync yet (render_final.py out of Scope); (4) no check.ps1 step runs these tests (check.ps1 out of Scope); (5) Image.reload() raises no depsgraph update for the Image, only its users.
