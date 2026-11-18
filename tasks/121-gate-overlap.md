# 121 Gate time: run the CPU-device and GPU-device halves side by side
Goal: `check gate` runs 150-185 s against B-09's 180 s (099's landing gate measured 185 s), with every device-split step run serially: analytic cpu then cuda (~20 s each), integrator cpu then cuda (~8 s and ~28 s), cpu gate tests then gpu gate tests (~24 s and ~11 s) -> independent CPU-device and GPU-device steps run concurrently under the one machine lock, no check is removed or weakened, and the gate time is measured before and after.
Read first: 0009 d8 (machine lock, waits excluded), d11 and d13 (gate tiers, B-09, kernel part), d18; tools/check.ps1 (step table: Name, Tag, Lock, Echo, Run), tools/lib/summary.ps1, tools/lib/lock.ps1; docs/STATUS.md "Budget headroom".
Scope: tools/check.ps1, tools/lib/summary.ps1, tools/lib/lock.ps1 (only if a step-level tree lock blocks the overlap), docs/decisions/0009-build-test-process.md (one sentence in d11 or d13 saying device halves overlap, if the record describes steps as serial).
Out of scope: removing, skipping or moving checks between tiers; lowering sample counts, families or thresholds; kernel or scene changes; the tripwire's statistic (076/089).
Reference: none (process change; 0009's own rules).
Do:
1. Group steps that touch different devices and share no outputs (for example analytic cpu with analytic cuda, integrator cpu with integrator cuda, cpu gate tests with gpu gate tests) and run each group's members as concurrent jobs; keep the step order, the per-step lines, the Echo lines and the summary format; a failing member fails the group with its own step name and log tail.
2. Timing tests stay valid: the CUDA tripwire (integrator cuda, B-16) and any wall-time check must not share the GPU with another GPU job, and its CPU side must not be starved — run timing-sensitive checks alone (say which) and show the tripwire's numbers before and after are within its noise.
3. B-09's measured time counts only work (0009 d8 excludes waits): confirm in-run tree-lock waits are excluded from the tier clock or make them so.
4. Measure: three `check gate` runs before and after on main, quiet machine, report each total and the kernel part.
Acceptance: `pwsh tools/check.ps1 gate` passes three times in a row with every step listed; the PR body holds the before/after table.
Look at: none.
Canary: none (no test added); `tools/check.ps1 gate` with a deliberately failing CPU analytic scene must still fail naming that step (show the output in the PR body).
Budget: target B-09 headroom of at least 40 s.
Depends on: none.
Size: S (about 150 lines); stop and report at 2x.
Progress:
- started (task/121-gate-overlap)
- check.ps1: Group tag on gate steps, concurrent jobs per group (g1 analytic cpu|cuda, g2 integrator cpu|gpu abi tests, g3 cpu|gpu gate tests); tripwire, material compile, aov, smoke stay alone.
- summary.ps1: Add-ArExcluded/Get-ArSeconds, in-run tree-lock and build waits leave the tier clock (were counted before).
- Before (main 0257c5f): 171.8 / 145.2 / 143.8 s, kernel part 13.8 / 13.3 / 13.3 s. After: 120.8 / 114.2 / 114.4 s, kernel part 13.9 / 13.5 / 13.4 s.
- Tripwire after 6.21-6.23e-5 / 2.06e-5 ms per sample vs before 6.2e-5 / 2.05e-5 (baseline 6.34e-5 / 2.05e-5): same. Not run: check.ps1 ci (needs the bpy wheel download).
