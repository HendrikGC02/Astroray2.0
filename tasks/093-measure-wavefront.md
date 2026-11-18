# 093 Measurement: wavefront overhead (B-19, B-17), state bandwidth and kernel time vs Cycles (B-20)
Goal: the wavefront design's scheduling overhead is estimated at 20-40% against a 10% budget -> measure GPU idle between launches, queue_build and sort share, launch durations, state traffic and per-sample kernel time on B-1M and B-5M at 1080p 1 spp, compare with Cycles, and apply the roadmap's fallback path on a negative result.
Read first: 0002 D7 (overhead estimate, remedies a-c), Risks 2-3; 0009 B-17, B-19, B-20; docs/ROADMAP.md Measurements rows "Wavefront overhead" and spike C; docs/spikes/B-blender-end-to-end/RESULTS.md section 3 (Cycles per-sample times); `astroray_cli bench`.
Scope: tools/bench.ps1 (machine lock, runs `astroray_cli bench` on bench/scenes.toml and appends history lines), tools/measure_wavefront.ps1 (one-off; deleted when this task lands), bench/history/ (lines committed by the lead), tools/budgets.toml (B-19, B-20 resets if measured with headroom).
Out of scope: implementing remedies (follow-up tasks written by the lead); the alpha-card scene (alpha pass-through lands in M1b; its B-19 run moves to M1b's alpha task).
Reference: NVIDIA Nsight Systems (timeline: GPU idle between kernels) and Nsight Compute (DRAM bytes per kernel) user guides. Precondition: owner question 0002 q1 (Nsight counter enable) answered; without counters, state bandwidth is reported as "not measured" and the rest proceeds.
Do:
1. tools/bench.ps1: runs `astroray_cli bench` for each bench scene (burn-in, then N passes), appends 0009 d22 history lines locally (out/bench/), prints a summary; the lead commits history lines from `full` runs only.
2. On b1m and b5m at 1080p, 1 spp per pass, 64 passes after burn-in: Nsight Systems timeline -> GPU idle between launches as % of pass time, queue_build + sort GPU time as % (B-19, each <= 10%); longest single launch (B-17 target 20 ms, fails above 100 ms); steps per pass.
3. Nsight Compute on one pass: DRAM bytes per kernel; bytes per path per step against the 157-225 B path state (0002 Risk 3).
4. Kernel time per 1080p sample on S1 and S2 vs Cycles' measured times through the .blend files (B-20: <= 1.25x of 7.6 ms / 10 ms).
5. Result (PR body `Result:` with commands, numbers and the timeline screenshot). On each outcome: B-19 and B-17 pass -> record and close; B-19 fails -> the lead writes tasks for 0002 D7 remedy (a) fixed grids plus CUDA graph capture, then (b) main-pool compaction, then (c) fused partition, re-measuring after each, all before M1a closes; if B-14 on B-1M (096) still fails after them, spike C (megakernel viewport prototype) runs and a superseding 0002 record is settled before M1b. B-20 fails -> reported with the per-kernel breakdown; no design change without the lead.
Acceptance: the PR body holds every number with its command; out/bench/ holds the history lines (format passes lint); `pwsh tools/check.ps1 gate` passes on the final diff.
Look at: out/evidence/093/timeline_b1m.png (Nsight Systems screenshot of one pass).
Canary: none: measurement only (bench.ps1 is a tool exercised by its acceptance).
Budget: resets B-19/B-20 only if measured with headroom; otherwise follow-up tasks.
Depends on: 089, 076.
Size: M (bench tool about 150 lines plus result); stop and report at 2x.
Progress:
- started (branch task/093-measure-wavefront)
