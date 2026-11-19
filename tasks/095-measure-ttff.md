# 095 Measurement: time to first frame vs Cycles (V7a gate, V7b phases) and M-8 translation time
Goal: the first-frame gate is unmeasured with materials, textures and lights -> a screen-capture harness measures user-perceived first frame (V7a) for Cycles and Astroray on S1, S2 (300 materials) and S3, first and second switch, a log-based phase breakdown (V7b) explains gaps, and S2 gives the first M-8 translation numbers, with the remedies of 0007 d35 applied in order on failure.
Read first: 0007 d35 (V7a gate <= 1.5x Cycles, goal 1.0x; V7b definition; remedies in order), V7a, V7b; 0005 Risks M-8 and 0009 B-24 (translation rows); docs/ROADMAP.md Measurements row "First frame"; docs/spikes/B-blender-end-to-end/RESULTS.md section 3 and its CopyFromScreen method.
Scope: tests/blender/probe_ttff.py (renamed from gui_ttff.py by lead ruling), tools/ttff_poller.ps1 (screen capture of the viewport region at >= 100 Hz, own PID only), tools/measure_ttff.ps1, blender/astroray (log lines: first `ar_scene_edit_begin`, first frame presented; translation timing per material), tools/report.py (first-frame section fed from history lines).
Out of scope: implementing remedies (follow-up tasks); Classroom (M3).
Reference: spike B `gui_screenshot.py`, `gui_cycles_viewport.py` and `parse_cycview.py` (read with `git show spikes-final:spikes/B-blender-end-to-end/<file>`) for the poller and Cycles log parsing.
Do:
1. gui_ttff.py: opens a .blend (b1m, b5m, s3_textured from 089 and 090), and from a timer callback switches the 3D view to Rendered shading with the chosen engine; the external poller detects the first frame with engine pixels in the region (resolution <= 20 ms); measures the first switch after opening the file and a second switch; 3 fresh-process runs per (scene, engine, switch).
2. V7b: Cycles from its log (first `Rendered N samples` minus (the sync line's timestamp minus T)); Astroray from its own log lines; per-phase breakdown (main-thread reads, materials, workers, device, tail, first sample) as in 0007 d35's table.
3. M-8 on S2: `inline_shader_nodes()` plus the Python walk and serialisation per material (median), total for 300 materials, and native compile, against B-24 (<= 1 ms median, <= 300 ms for S2, <= 2 ms per 100 nodes).
4. Result (PR body `Result:` with commands, the per-run table and V7b breakdowns; history lines for the report). Outcomes: every scene and switch <= 1.5x Cycles -> gate met, recorded; S2 or S3 above -> 0007 d35's remedies in order, each as a lead-written task, re-measured after each: (1) `as_pointer()` topology reads, (2) cross-session GAS disk cache, (3) parallel material inlining moved earlier; M-8 above B-24 -> per-node lowering moves into the native compiler, then a value-only patch path (roadmap row M-8), as follow-up tasks.
Acceptance: the PR body holds the tables with commands; `<python> tools/report.py` shows the first-frame section with the measured bars and the 1.5x line.
Look at: out/evidence/095/ttff_s2.png (the report's first-frame plot).
Canary: none: measurement only.
Budget: B-15 (first frame) evaluated; B-24 translation rows reset only with headroom.
Depends on: 089, 090, 086.
Size: M (harness about 300 lines plus result); stop and report at 2x.
Progress:
- started (lead ruling: harness named tests/blender/probe_ttff.py, not gui_ttff.py; measurement, not a test, so check full does not run it)
