# 090 Showcase and textured scenes: Cornell box and S3 (B-TX)
Goal: the owner's M1a showcase has no scenes -> Bitterli's CC0 Cornell box is converted into a builder lit by an area lamp and a point lamp, and the textured scene S3 (B-TX), which 0007 names but never specifies, is defined here and built as a builder and a .blend.
Read first: docs/ROADMAP.md M1a "Owner sees" and rule 4 (scene-builder versions), 0007 d35 (S3 in the first-frame gate), 0009 d20 (B-TX), d21 (assets); docs/research/16-test-scenes-and-assets.md (Bitterli Cornell Box entry); assets/manifest.toml.
Scope: scenes/cornell/{build.py,meta.toml}, scenes/cornell/convert_bitterli.py, scenes/s3_textured/{build.py,meta.toml}, scenes/s3_textured/make_textures.py, scenes/lib/blend_writer.py (Image Texture nodes), bench/scenes.toml.
Out of scope: Classroom (M3), HDRI worlds (M1b), metals (M1b).
Reference: Bitterli rendering resources, Cornell Box (CC0), the Mitsuba or pbrt scene file in the fetched archive.
Do:
1. convert_bitterli.py reads the fetched Cornell box archive (assets/_cache/cornell_box) and extracts meshes, Lambertian albedos and the light's rectangle and emission; build.py emits it with the M1a material set (constant base colours, `specular_weight = 0`) in two variants: `cornell_area` (the original rectangular lamp as an area lamp) and `cornell_point` (a point lamp of equal power at the lamp's centre); meta.toml class G with the asset reference.
2. S3 definition (designed here, recorded in meta.toml and in the PR body for the lead to confirm): 1,000 instances of 50 unique UV-mapped meshes (about 1 M triangles in total) using 32 distinct 2048x2048 RGBA8 sRGB textures generated deterministically by make_textures.py into out/scenes/s3_textures/ (procedural patterns, no external asset), each texture driving base colour through an Image Texture node, roughness 0.5, one area lamp, constant world. Rationale: textures dominate its sync and first frame, which is what 0007 d35 wants S3 to measure.
3. Both scenes as .blend files through 084's blend_writer.py, extended here with Image Texture nodes; S3 registered in bench/scenes.toml as `b_tx` with set `M1a`; cornell scenes as showcase entries for the gallery (100).
Acceptance: `<python> tools/scenes.py` writes the three recordings; `out/bin/astroray_cli.exe render out/scenes/cornell_area.arrec -o out/tmp/cornell.fits --spp 256 --size 512x512 --device cuda` exits 0 with no report entries; the .blend files open in Blender and render with the addon.
Look at: out/evidence/090/cornell_area.png, out/evidence/090/cornell_point.png, out/evidence/090/s3.png.
Canary: none: no test added (scenes are inputs to 095, 096 and 100).
Budget: none on tiers (recordings are generated only when a gate step needs them; these are not in gate).
Depends on: 088, 070, 083, 089, 110.
Size: M (about 350 lines Python); stop and report at 2x.
Progress:
- started
