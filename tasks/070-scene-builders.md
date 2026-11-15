# 070 Scene builders, meta.toml and recording generation
Goal: test and benchmark scenes have no source -> Python builders call the ABI through `_abi.py` with recording on, each scene has a `meta.toml`, `tools/scenes.py` regenerates every recording into out/scenes/, and the first analytic scenes exist as builders.
Read first: 0009 d19 (scene format, producers, recordings never committed), d20 (canonical set, meta.toml fields), d16 (analytic scenes prefer shape-independent setups), d21 (asset rules); 0003 d9 (metres).
Scope: scenes/lib/ar.py, scenes/lib/meshes.py, scenes/lib/meta.py, scenes/a5_emitter/{build.py,meta.toml}, scenes/a1_furnace/{build.py,meta.toml}, tools/scenes.py, tools/check.ps1 (gate step regenerating recordings), tools/README.md.
Out of scope: truths and tests (072 onward); benchmark and showcase scenes (089, 090); `.blend` output (084 adds it for the Blender smoke).
Reference: none.
Do:
1. scenes/lib/ar.py (stdlib + NumPy, Apache-2.0): wraps out/build/gen/_abi.py: open a recording context (`record_path`), create scene and session descriptors, edit helpers that fill staged arrays from NumPy (`mesh(key, positions, corner_vert, face_start, normals, uv)`), instances, a graph builder for M1a ops (any OpenPBR input as a constant by its spec name, e.g. `specular_weight = 0`; base colour from an image texture; emission colour and strength), lamps, world colour, camera from position, target, focal length and sensor, settings (spp, seed, max bounces, filter, size).
2. scenes/lib/meshes.py: UV sphere with smooth normals, plane, box, displaced grid; all in metres, deterministic.
3. scenes/lib/meta.py validates meta.toml: `class` (A, D, O, B, G), `truth` (formula name and parameters), `tolerance`, `eps_sys` (default 0), `spectra`, `units = "m"`, `assets`, `single_witness`.
4. Builders: a5_emitter (a black-based emissive plane of radiance L facing the pinhole camera and filling its view, world black; truth `pixel = L`); a1_furnace (a sphere with base colour rho, `specular_weight = 0` and diffuse roughness 0, i.e. Lambertian, in a constant world of radiance Lw, camera outside; truth `pixel = rho * Lw` on the sphere and `Lw` elsewhere, independent of tessellation because the sphere is convex). Each takes `--out <path.arrec>`.
5. tools/scenes.py builds every scenes/*/build.py into out/scenes/<name>.arrec (skipping up-to-date outputs by source hash); check.ps1 `gate` runs it before tests that need recordings.
Acceptance: `<python> tools/scenes.py` writes out/scenes/a5_emitter.arrec and a1_furnace.arrec; decoding each recording with arrec.py lists the expected calls with the builder's values (a5: 2 triangles and emission strength L in the graph's constant pool; a1: the sphere's triangle count, base colour rho, specular_weight 0, world Lw) and the camera descriptor (pixels are first checked in 072 and 073, since surfaces shade from 071); meta.toml files validate.
Look at: none.
Canary: none: no test added (the builders are exercised by 072 and 073, whose canaries cover them).
Budget: recording generation time inside the gate; report it.
Depends on: 069.
Size: M (about 400 lines Python); stop and report at 2x.
Progress:
- Builders a5_emitter and a1_furnace, scenes/lib/{ar,meshes,meta}.py, tools/scenes.py and the gate `scenes` step (tag cpu) written; recordings decode with the builders' values.
- Renders of both recordings end in "no entry point registered for stage 5" (shade_surface, task 071), as the task expects; the engine accepts both generations.
