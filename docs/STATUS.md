# Status

Overwritten by the lead only. Live facts (branches, PRs, last gate) come from `tools/status.ps1`, never from here.

## Current milestone
M1a "First pixels" (`docs/ROADMAP.md`) is built: textured, lamp-lit meshes render progressively in the Blender 5.3
viewport and F12 on CUDA and CPU through the final C ABI, every gate live; gallery and report from task 100
(`tools/gallery.py --milestone M1a`, `tools/report.py`; CPU and CUDA means agree to 3 decimals on every showcase
scene). The close waits on the owner decisions below (ROADMAP: a milestone that breaks a gate does not close).
Next: the Fable planning session for M1b on `docs/retro/M1a-retrospective.md`, then 136.

## Owner decisions for the M1a close
- B-19 (0002 D7): partition row met on B-1M after 134 (9.4 %, ruling 85); GPU idle 11.0 % against 10 % is the
  step-table round trip plus launch gaps, no regressing commit (run-to-run 0.3 ms); B-5M not measured (136).
- B-20: 2.00x / 1.63x Cycles per 1080p sample (limit 1.25x). B-31: instance loop 39.8 / 88.5 ms per 10k warm /
  cold (limit 30); V7a met regardless (worst S1 1.17x). M-1 probe spills 130/376 B over 128/192 (D10 ladder, M1b).
- Process changes in the retrospective marked [O] (hooks vs 0009 d31, budgets, agent caps in lint).
- Earlier: deleting superseded records (0009 q4); exact JPEG parity (ruling 78).

## Red items
- B-5M bench hangs before its first pass on most runs since 1a7e364 (task 136; generation 1 activation 110-124 s
  when it does run). The CLI bench spins on PAUSED instead of failing loudly (136).
Unowned follow-ups (a task that touches the area takes them):
- ABI/host: no CUDA probe reason or driver version (017); CUDA init creates the context on the init thread; OptiX's
  first TLAS build keeps ~230 MiB (126); 052 starts an epoch per activated commit (0007 d12); 051's display ring
  downloads synchronously (0007 d29); PASS_UNSUPPORTED entries persist for a session's life (ruling 74); the build
  record and gallery sidecars print `cuda none` on CUDA runs (106, 100).
- Kernel: JH round trip 1.1-1.6e-3 off-grid near white (023); vm/store_slot.h copies kSlotSemantic; 0006's interval
  rules have no opcode-generator owner; eta_clamped has no counter (073); light splats must reject negative radiance (047).
- Addon: translate.py copies C header ids behind a drift test (083); astroray_extension not in ALL (084); an
  orthographic camera renders black in the Rendered viewport (097); viewport sync resends more than it must (111);
  film_exposure has no viewport effect.
- Tests: check 14's constant-hero exception (078); flaky once: `cpu_device/parallel_for rethrows`, `wavelengths/
  stratified lane positions`, `mesh/remove erases the record`, Blender smoke `test_cancel_returns_partial_result`;
  fault tests leave temp dirs; minidumps land in the cwd (132); check full's `waited` omits Blender waits (131);
  `check fast` exceeds B-08 after a rebuild (retrospective R1).
- Report: build times, pass_ms and ray-count sections have no history lines yet; the bounce heat map is low contrast.
- To settle (076, 107): roulette start vs min bounces (0004 D2); AUTO emission threshold (067); NaN inputs render
  black with no INVALID_SAMPLE.
- M1b inputs: Mix/Add Shader (058), colour-to-float for textured Roughness/Specular, 083 Do 7, a spectral blackbody
  lamp; ruling 80's locality-preserving sort key (122); Material/World.use_nodes deprecated in 5.3 (087).

## Budget headroom
Landing gates ~130-165 s of B-09's 180 s; `check full` ~10-15 min. `check fast` 5-7 s warm; over B-08 after a
rebuild. History: bench/history/2026.jsonl. Spikes: docs/spikes/*/RESULTS.md.
