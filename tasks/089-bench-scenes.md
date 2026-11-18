# 089 Benchmark scenes B-1M and B-5M (S1, S2 with 300 materials)
Goal: the standing performance gates have no scenes -> scene builders reproduce spike B's procedural S1 (1.0 M triangles) and S2 (5.04 M unique triangles in 10,001 instances, 300 materials) as ABI recordings and as .blend files, registered in bench/scenes.toml, and the timing tripwire moves to B-1M.
Read first: 0009 d20 (benchmark scenes B-1M, B-5M), d19 (builders also emit .blend), d18 (timing tripwire on B-1M), d22 (bench list); docs/spikes/B-blender-end-to-end/RESULTS.md sections 2-3 (scene definitions and measured numbers); 0007 d35 (S1, S2 for the first-frame gate).
Scope: scenes/b1m/{build.py,meta.toml}, scenes/b5m/{build.py,meta.toml}, scenes/lib/blend_writer.py (extends 084's), scenes/lib/ar.py (instances in the JSON description), bench/scenes.toml, tools/check.ps1 (tripwire scene), tests/budgets/counters.toml.
Out of scope: B-TX and the Cornell box (090); running the measurements (093-096).
Reference: spike B `make_scenes.py` (read with `git show spikes-final:spikes/B-blender-end-to-end/make_scenes.py`; it is Apache-2.0 project code), which defines S1 and S2 exactly (seed 12345, mesh shapes, instance kinds).
Do:
1. b1m: port S1 (displaced grid with a UV map, about 1.0 M triangles, one material, camera and sun from make_scenes.py; the sun is replaced by an area lamp of equal irradiance in M1a, since sun lamps land in M4, and the meta.toml says so) as an ABI builder with the same seed and geometry.
2. b5m: port S2 (2,000 unique meshes of about 2,520 triangles, 300 materials with distinct constant base colours and roughness, 5,000 linked duplicates, 2,000 collection instances, 1,000 geometry-nodes instances; 10,001 instances) as an ABI builder producing the same instance transforms.
3. Extend scenes/lib/blend_writer.py (084) so it writes the same scenes as .blend files into out/scenes/, reproducing make_scenes.py's Blender-side construction (linked duplicates, collection instances and the geometry-nodes instancer for b5m) so the addon sync sees the real instance kinds; S2's sun is replaced by the same area lamp as in the builder. Cycles comparisons in 093 and 095 use these .blend files.
4. bench/scenes.toml: entries for b1m and b5m with sets `M1a`.
5. The B-16 timing tripwire (076) switches to B-1M kernel time per 1080p sample at 1 and 64 spp; counters.toml gains B-1M rows on CPU and GPU.
Acceptance: `<python> tools/scenes.py` writes b1m.arrec and b5m.arrec and prints triangle and instance counts (1,0xx,xxx; 5,04x,xxx unique in 10,001 instances); `out/bin/astroray_cli.exe render out/scenes/b1m.arrec -o out/tmp/b1m.fits --spp 4 --size 1920x1080 --device cuda` exits 0; the .blend files open in Blender with the same counts.
Look at: out/evidence/089/b1m.png and out/evidence/089/b5m.png (1080p renders, 16 spp).
Canary: none: no test added (the tripwire and counter rows changed are checked by `check gate` itself; the reviewer verifies the counts printed).
Budget: tripwire runtime inside B-09; report it.
Depends on: 070, 084, 076.
Size: M (about 350 lines Python); stop and report at 2x.
Progress:
- started
- builders b1m/b5m, writer kinds (object, collection, points), bench entries done; .blend counts and 1080p renders verified.
- BLOCKED: tripwire/counters need tools/integrator_checks.py (not in Scope) and counts printing needs tools/scenes.py; lead decides.
