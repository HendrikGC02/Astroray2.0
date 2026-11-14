# Status

Overwritten by the lead only. Live facts (branches, PRs, last gate) come from
`tools/status.ps1` once it exists, never from this page.

## Phase
Planning is complete. The build phase has not started: no engine code exists,
`tools/` and `src/` do not exist yet, and the records in `docs/decisions/` are
awaiting the owner's sign-off.

## Current milestone
Before M1a, then M1a "First pixels" (`docs/ROADMAP.md`). Goal: textured,
lamp-lit meshes render progressively in the Blender 5.3 viewport and F12 on
CUDA and CPU through the final C ABI, with every gate live from the first task.
The open task files and their order are in `tasks/README.md`.

## Open owner questions
The questions needed before M1a are answered by lead defaults at the end of
`docs/decisions/0000-owner-decisions.md`; the owner may overrule any of them.
During M1a: Nsight counter enable (0002 q1); an open-weight worker and deleting
superseded records (0009 q3, q4). Later ones: ROADMAP "Open items".

## Red items
None.

## Last measured numbers (spikes; throwaway code, lower bounds, not product)
Spike A, `spikes/A-kernel-budget/RESULTS.md` (RTX 5070 Ti, CUDA 12.8, MSVC
19.44):
- Fused shade + NEE, N=4: 148 registers, 0 spills, 1.27 ms per 2^20 paths
  (sec. 8 verdict); with `__launch_bounds__(256,2)`: 128 registers, 68 B spill,
  0.82 ms.
- Compile: 1.3-2.5 s per device TU serial; clean 34-TU build 11.3 s at -j8
  (sec. 4).
- CPU/GPU agreement: max eval error 1.1e-6 with FMA off, 1.1e-2 with FMA on
  (sec. 6); CPU 0.35 us per BSDF evaluation (sec. 7).

Spike B, `spikes/B-blender-end-to-end/RESULTS.md` (Blender 5.3.0):
- First frame, mesh and instance sync only, vs Cycles F12 pre-first-sample:
  S1 (1.0 M triangles) 98 ms vs 190 ms; S2 (5.04 M triangles, 10,001
  instances) 1,420 ms vs 1,280 ms (sec. 3). No shaders, textures or lights yet.
- Present: every 1080p path holds 24, 30 and 60 updates/s; 4K holds 60 only at
  or below 32 MiB per update (sec. 4).
- Unmeasured: shader, texture and light sync; product-scope kernels.

Budget headroom from the last gate: none yet (no gate has run).
