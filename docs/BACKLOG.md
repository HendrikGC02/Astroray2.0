# Backlog

Items with no task file. The lead edits this page; a task that touches the area takes its rows (and deletes
them in the same change), or the slice review turns them into tasks. Source ids are the M1a tasks that found
them. Milestone is where the item must be settled at the latest.

| Area | Item | Source | Milestone | Taken by |
|---|---|---|---|---|
| ABI, host | No CUDA probe reason or driver version in `ar_build_info` / CLI info | 017 | M1b | |
| ABI, host | CUDA init creates the context on the init thread | 094 | M2 | |
| ABI, host | OptiX's first TLAS build keeps about 230 MiB | 126 | M2 | |
| ABI, host | An epoch starts per activated commit (0007 d12) | 052 | M2 | |
| ABI, host | The display ring downloads synchronously (0007 d29) | 051 | M2 | |
| ABI, host | PASS_UNSUPPORTED entries persist for a session's life | ruling 74 | M8 | |
| ABI, host | Build record and gallery sidecars print `cuda none` on CUDA runs | 106, 100 | M1b | slice D gallery |
| Kernel | Jakob-Hanika round trip 1.1-1.6e-3 off-grid near white | 023 | M4 | |
| Kernel | `vm/store_slot.h` copies kSlotSemantic | 056 | M1b | 151 |
| Kernel | 0006's interval rules have no opcode-generator owner | 055 | M1b | slice C |
| Kernel | `eta_clamped` has no counter | 073 | M2 | |
| Kernel | Light splats must reject negative radiance | 047 | M5 | |
| Kernel | Locality-preserving sort key (ruling 80), only with a measured gain | 122 | M3 | |
| Addon | `translate.py` copies C header ids behind a drift test | 083 | M1b | 151 |
| Addon | `astroray_extension` target is not in ALL | 084 | M1b | |
| Addon | Orthographic camera renders black in the Rendered viewport (must warn until M4) | 097 | M1b | slice B |
| Addon | Viewport sync resends more than it must on material, image and light edits | 111 | M2 | |
| Addon | `film_exposure` has no viewport effect | 111 | M1b | slice B |
| Addon | `Material.use_nodes` and `World.use_nodes` are deprecated in 5.3 | 087 | M1b | slice A lowerings |
| Addon | Mix Shader, Add Shader lowering; colour-to-float for textured Roughness and Specular; 083 Do 7 | 058, 083 | M1b | slice A lowerings (Add: M2) |
| Addon | B-31 instance loop 39.8 / 88.5 ms per 10k against 30 | 129 | M1b | 161 |
| Lights | Spectral blackbody lamp (lamps render BLACKBODY as RGB today) | 068 | M1b | slice B |
| Tests | Check 14's constant-hero exception | 078 | M2 | |
| Tests | Flaky once: `cpu_device/parallel_for rethrows`, `wavelengths/stratified lane positions`, `mesh/remove erases the record`, Blender smoke `test_cancel_returns_partial_result` | several | M1b | the task that sees one again files it |
| Tests | Fault tests leave temp dirs; minidumps land in the cwd | 132 | M1b | |
| Tests | Windowed canaries start one Blender per case | 137 draft | M1b | after 137 |
| Tools | `check full`'s `waited` may omit Blender lock waits (verify) | 131 | M1b | 141 |
| Tools | `astroray_tests_cpu --tier=full` is run by no tier (verify); 29 of 37 budget rows are checked by nothing | retrospective | M1b | slice review after 140 |
| Tools | Idle and partition share analysis for `check perf` (134's script was not committed) | 134, 140 draft | M1b | after 140 |
| Tools | Landing several green branches in one gate run | R7 | M2 | |
| Report | Build times, pass_ms and ray-count sections have no history lines; bounce heat map is low contrast | 100 | M1b | 140, slice D |
| To settle | Roulette start against min bounces (0004 D2) | 076 | M2 | |
| To settle | AUTO emission threshold | 067 | M4 | |
| To settle | NaN material inputs render black with no INVALID_SAMPLE | 107 | M1b | slice A flags task |
| Media | VM volume entry with `medium_eval`; constant-block layout for a medium (write into 0006) | 152 draft | M1b | slice C |
| Process | Fold rulings 64-85 into the records; delete superseded records | R12 a, 0009 q4 | owner | |
| Process | Stale directories `.claude/worktrees/agent-a934dd254fe6d8f2d`, `agent-a3a823bde1b1e2a0c`, `agent-a61c36ff96e523f48`, `agent-a631435169fa2c847`; 40 merged `worktree-agent-*` branches | handover | M1b | lead, after a look |
