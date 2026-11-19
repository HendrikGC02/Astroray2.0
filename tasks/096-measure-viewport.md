# 096 Measurement: viewport rate on B-1M (B-14), present cost (B-13, V8), latency (V12) and batching on S2 (V23)
Goal: the owner's viewport targets are unmeasured on the product -> a windowed harness measures held updates per second at full 1080p on B-1M and the floor on every M1a bench scene, view_draw cost at 1080p on Vulkan, cancel and view-change latency with zero stale frames, and the batching schedule during and after a scripted orbit on S2, with the roadmap's fallback on failure.
Read first: 0009 B-13, B-14, B-32, B-34, B-17; 0007 d25 (latency rule), d29-d33 (present), d50 (batching), V8, V12, V23; docs/ROADMAP.md rule 2 (standing gates: 30/s on B-1M, 24/s floor on B-1M, B-5M, B-TX at full resolution in M1a) and Measurements row "Wavefront overhead".
Scope: tools/viewport_rate_probe.py (lead ruling: the rate harness is a measurement, not a test; was tests/blender/gui_viewport_rate.py), tests/blender/gui_latency.py, tools/measure_viewport.ps1 (one-off; deleted when this task lands), blender/astroray (frame log with generation, pass count and timestamps behind ASTRORAY_FRAME_LOG), tools/report.py (viewport section), tasks/096.canary.patch.
Out of scope: the resolution controller (M2: rates are measured at full resolution only); 4K and OpenGL (M9); OIDN (M1b).
Reference: spike B `gui_present.py` and `run_gui.ps1` (read with `git show spikes-final:...`) for the windowed timing harness.
Do:
1. gui_viewport_rate.py: 1080p region, Rendered shading, a scripted continuous orbit (a view change every frame for 10 s) and a still period; presented updates per second from the frame log (median, p5) and any window where the rate stays below 24/s for more than 0.5 s; on b1m (target 30/s), b5m and b_tx (floor 24/s).
2. B-13 / V8 at 1080p on Vulkan: view_draw time per present over 300 paced frames at a 60 Hz producer (median and p99 against 2 ms / 3 ms), held rate >= 58/s.
3. gui_latency.py (V12): F12 cancel stops at the next launch boundary (time from test_break to session CANCELLED <= B-17); one camera change to its first frame <= the rest of the pass in flight + one pass + present; during the orbit every completed pass yields a frame for a newer view and zero stale frames are shown after a newer one.
4. V23 on b5m: one pass per displayed update during interaction, growing passes per update afterwards, publish rates within B-34.
5. Result (PR body `Result:` with commands, numbers and plots). Outcomes: all met -> record, history lines for the report; B-14 target or floor missed -> per the roadmap, 093's remedies (a)-(c) are applied first (lead tasks), then if B-1M still misses 30/s at full 1080p, spike C runs and a superseding 0002 record is settled before M1b; B-13 missed -> the striped present is profiled against spike B's numbers and the packed-format fallback of 0007 Alternatives is proposed to the lead; latency or stale frames -> a defect task for the latch logic (052).
Acceptance: the PR body holds every table with commands; tools/report.py shows the viewport section (rate over time with the 30 and 24 lines).
Look at: out/evidence/096/viewport_rate_b1m.png (rate plot) and a viewport screenshot during the orbit.
Canary: tasks/096.canary.patch must turn red: tests/blender/gui_latency.py "test_cancel_stops_at_next_launch_boundary", "test_view_change_latency_within_b32", "test_orbit_frames_are_for_newer_views", "test_orbit_shows_no_stale_frame" (the rate probe is a measurement and has no canary).
Budget: evaluates B-13, B-14, B-32, B-34; resets PROVISIONAL rows only with headroom.
Depends on: 086, 089, 090.
Size: M (harness about 300 lines plus result); stop and report at 2x.
Progress:
- 096 started on task/096-measure-viewport.
- Lead rulings: rate harness is tools/viewport_rate_probe.py (not a test); gui_latency.py is the V12 test with the canary above; orbit screenshot by Blender's screenshot operator, rates from the frame log (GDI cannot see the swapchain).
- Measured (out/viewport, out/evidence/096): B-1M 32.1/s mean, p5 30.4; B-5M 27.8/s; B-TX 33.6/s; B-13 view_draw median 0.92 ms (B-1M 1.85), p99 <= 2.2 ms; held 60.1/s at a 60 Hz offer.
- gui_latency red on every scene: view change to screen 20-41 ms against a 33 ms bound on B-1M, orbit gap p99 36.9 ms. Cause: session.cpp waits the 1/60 s publish gap before each interaction pass (serial with the pass); lead decides the defect task (052 latch logic).
