# 069 astroray_cli render, dump and bench
Goal: recordings cannot be rendered from the command line -> `astroray_cli render` replays a recording through the public ABI, renders each session to a sample count, writes a FITS image or a full film dump, prints the final report and fails on ERROR entries; `bench` emits history lines.
Read first: 0001 D1.4 (CLI as primary host, recordings), D2.10 (replay renders each session to its sample count or the CLI's), D2.11 (CLI prints the report, non-zero exit on ERROR), R38, R59 (dump covers film and science output only); 0009 d19 (recordings in out/scenes), d22 (history line).
Scope: cli/main.cpp, cli/render.cpp, cli/bench.cpp, CMakeLists.txt (generated replayer into astroray_cli), tests/abi/test_cli_render.py.
Out of scope: scene builders (070); any scene-state readback (R59: none exists).
Reference: none.
Do:
1. `render <recording> -o <file.fits> [--spp N] [--device cpu|cuda[:i]] [--seed S] [--size WxH]`: replays with 033's replayer (refusing an ABI mismatch, exit 3), overrides device (context creation is the recording's first call; `--device` replaces its desc), seed and size when given, starts every recorded session, waits until each reaches its spp (settings or `--spp` for viewport recordings), writes Combined's mean and variance with `ar_session_write_fits` (DUMP restricted to Combined and sample count), prints the final report table (level, code, subject, count, detail), exits 1 if any ERROR entry exists.
2. `dump <recording> -o <file.fits> [same options]`: as render but writes every allocated channel (moments, AOVs, statistics, counters): the film dump of 0008 d13.
3. `bench <recording> [--passes N] [--warmup W]`: renders, times passes with the session's per-pass times and counters, and prints one history line per metric (0009 d22 fields; `metric` names `kernel_ms_per_sample`, `pass_ms`, `rays_closest`, `rays_shadow`, `steps_per_pass`).
4. tests/abi/test_cli_render.py: a recording made through `_abi.py` of a tiny CPU scene with no geometry and a constant world renders at 4x4, 16 spp; the FITS file parses with fits_read.py and its Combined mean equals the world's display RGB within 5 standard errors (shade_background, 103), so an empty film fails; a recording with an edited hash exits 3; the scene's injected unsupported world node (067) shows in the printed report.
Acceptance: `<python> -m unittest discover -s tests/abi -p "test_cli_render.py"` passes; `out/bin/astroray_cli.exe render <that recording> -o out/tmp/w.fits --spp 4` prints the report table and exits 0; `pwsh tools/check.ps1 gate` passes.
Look at: none.
Canary: tasks/069.canary.patch must turn red: tests/abi/test_cli_render.py "TestCliRender.test_error_entry_exits_nonzero", "TestCliRender.test_hash_mismatch_exits_3", "TestCliRender.test_world_colour".
Budget: none.
Depends on: 033, 104, 103.
Size: M (about 350 lines); stop and report at 2x.
Progress:
- Blocked before code: replayer has no device/seed/size/spp override (tools/abigen.py) and no ABI/report channel for active test hooks (astroray.h, session.cpp); both outside Scope. Reported to lead.
- Done on branch task/069-cli-render: replayer overrides (abigen.py), cli render/dump/bench, test, canary. Item 5 moved to 076.
- Only the sessions-that-render tests fail: no CPU intersect_closest stage until 114 (session faults 0xC0000005). Rebase on 114 and rerun.
- render and dump both write AR_FITS_DUMP: the ABI has no Combined-only file kind. New cli/cli.h added (shared helpers).
