# 100 M1a showcase gallery and milestone report
Goal: M1a has no visible close-out -> a gallery script renders the milestone's showcase scenes through headless Blender with the addon (so colour management matches what users see) with JSON sidecars, the benchmark report shows M1a's measured gates against their lines, and the lead has everything needed to inspect and close M1a.
Read first: 0009 d22 (report contents), d23 (showcase gallery, sidecars, images on the GitHub Release, not in git), d21 (image size caps); docs/ROADMAP.md rules 1-2 and M1a "Owner sees", "Exit criteria", "Look".
Scope: tools/gallery.py, tools/report.py (sections filled from history and evidence: B-19, B-14, V7a, M-1 probe, OptiX cold compile, heat maps, CPU vs GPU, renders), scenes/*/meta.toml (gallery entries), bench/scenes.toml (showcase flags).
Out of scope: publishing the GitHub Release and committing history lines (the lead does both); new scenes (089, 090).
Reference: none.
Do:
1. tools/gallery.py (runs Blender through Invoke-ArBlender): for each showcase entry (cornell_area, cornell_point, s3_textured, b5m with 300 materials, a1_furnace) renders F12 through the addon on CUDA and on CPU at the scene's gallery size and spp, saves PNG (<= 1600 px, <= 2 MB, never blank or near-black: the script fails if the image mean is below 1% of white) into out/gallery/M1a/, writes a JSON sidecar {scene, device, spp, time, settings, build id}; also saves the Sample count and Bounce count heat maps of the Cornell scene from the viewport display modes.
2. Side-by-side CPU and GPU images per scene in the report, with the per-pixel relative difference map (informational; agreement is gated elsewhere).
3. tools/report.py fills its sections from bench/history and the measurement tasks' history lines: B-19 shares with the 10% line, B-14 rate with 30 and 24 lines, V7a ratios with the 1.5x line, M-1 probe spills and stack with 0002 D10's lines, OptiX cold compile against B-18, tier durations, kernel resources per kernel over commits.
4. The report opens offline from out/report/index.html with every plot and the gallery thumbnails embedded.
Acceptance: `<python> tools/gallery.py --milestone M1a` writes every PNG and sidecar and prints their sizes; `<python> tools/report.py` writes the report with no "no data yet" placeholder left in the M1a sections; `pwsh tools/check.ps1 full` passes.
Look at: out/gallery/M1a/*.png, out/report/index.html (the lead inspects the showcase scenes and both heat maps, CPU and GPU, as the roadmap's M1a "Look" requires).
Canary: none: tooling for the milestone close; its outputs are inspected.
Budget: none on tiers.
Depends on: 077, 078, 087, 090, 091, 092, 093, 094, 095, 096, 097, 098, 099, 107, 108, 109, 111, 120, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131 (`check full` runs what they add).
Size: M (about 300 lines Python); stop and report at 2x.
Progress:
