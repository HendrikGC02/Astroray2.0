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
| Addon | Viewport sync resends more than it must on material, image and light edits | 111 | M2 | |
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
| Report | Build times, pass_ms and ray-count sections have no history lines | 100 | M1b | 140, slice D |
| To settle | Roulette start against min bounces (0004 D2) | 076 | M2 | |
| To settle | AUTO emission threshold | 067 | M4 | |
| To settle | NaN material inputs render black with no INVALID_SAMPLE | 107 | M1b | slice A flags task |
| Media | VM volume entry with `medium_eval`; constant-block layout for a medium (write into 0006) | 152 draft | M1b | slice C |
| Process | Fold rulings 64-85 into the records; delete superseded records | R12 a, 0009 q4 | owner | |
| Process | Stale directories `.claude/worktrees/agent-a934dd254fe6d8f2d`, `agent-a3a823bde1b1e2a0c`, `agent-a61c36ff96e523f48`, `agent-a631435169fa2c847`; 40 merged `worktree-agent-*` branches | handover | M1b | lead, after a look |
| ABI, host | `Ledger::remove` throws on a non-live id (double free) and two destructors swallow it on every replace/remove (embree_scene.cpp ~Charge, mesh.cpp ~MeshTopology/~MeshRecord): report it through the context log | 142 review | M1b | |
| Docs | 0009 d26 omits the reviewer's canary.patch input; 0009 Appendix A cites the old CLAUDE.md numbering; d13 rows B-26, B-27, B-36 cite "review item" text now in docs/history/; 0008 line 241 and ROADMAP line 37 say spike F part A must still run (it ran, owner decision 30); 0001 D7 says "one GPL file" (Cycles 5.3 has three GPL headers, none in src/) | 155, 143 reviews | M1b | |
| Tools | `citations` lint checks only the first decision of a slash list (`0006 D37/D40`); bare `R` reads research items as rulings where they share a number | 155 review | M2 | |
| Tools | 0009 d28 write-back: 137's refuse/warn rules, 8-hunk cap, baseline record, `$`-in-double-quotes lint | 137 | M1b | lead after 137 lands |
| Kernel | Out-of-range HG anisotropy is clamped to 1 - 1e-3; the lowering that produces g reports it (0003 D8) | 152 review | M1b | slice C volume entry |
| Addon | The orthographic Rendered viewport warns with the generic `CAMERA_UNSUPPORTED: invalid camera descriptor` (the projection is rejected before `make_camera_view`'s ORTHO check); the frame is black until M4 | 159 | M1b | slice B |
| Kernel | The light-image splat adds to the display channel without `film_exposure` (resolve.h ~110), though 0008 d7 exposes light-pass channels; dormant until light tracing sets `FilmRequest::light_pass` | 159 review | M5 | |
| Addon | `Light.node_tree` (read by sync/lights.py) is missing from blender/astroray/api_surface.txt | 159 review | M1b | |
| ABI, host | `bind_globals` reads bounces and seed from live settings each pass, not the latched generation's snapshot (one-pass race on a display-mode-only latch) | 159 review | M2 | |
| Tests | Flaky once: `cuda_session_trace/no cache dir disables the optix cache` (gate, 136); one hosted-ci `cpu gate tests` case on 137's head (no C++ change; rerun green) | 136, 137 | M1b | the task that sees one again files it |
| Tools | Hosted ci's log shows only check.ps1's 20-line summary, so a failing doctest case is not named; out/check/ci.log is not uploaded as an artifact | 137 landing | M1b | |
| Tools | build.ps1 reconfigures only when the source list changes, not when -Preset changes (a cpu-preset build leaves the tree GPU-off; the next dev build fails 'unknown target')  | 153 | M1b | |
| Tools | A cold or reconfigured worktree's first `check gate` exceeds B-09 (182 s in 142, 198 s in 153) because the in-gate build counts; landings are warm (prepare builds first) | 142, 153 | M1b | |
| Kernel | build_env_table throws on a uniform grid below ~1e-22 (compensated integral denormal in float): loud, unrealistic radiance; revisit with bake_environment | 153 | M1b | slice B |
| ABI, host | Settings (film_exposure, seed, bounces) are read live at the latch, not from the latched generation's snapshot: a commit activating at the latch can give one transient frame whose exposure does not match its tables | 159 review | M2 | |
| Tests | B-12 (0.41 s per fast test) failed once under load on lobe_ggx/pdf integral f64 and material_layout/rgb white reflectance f64 (151's run after a canary build); rerun green. Watch for repeats | 151 | M1b | the task that sees it again files it |
| Tools | Agents shared a session scratchpad file name (break.py): 151 ran 154's canary script against 154's worktree (154 noticed and reverted). Scratch scripts belong under the agent's own out/tmp/NNN/ (implementer.md says so); restate in briefs | 151, 154 | M1b | lead (briefs) |
| Kernel | Cauchy dispersion runs on the unmodulated n_d and specular_weighted_eta modulates per lane afterwards; OpenPBR (index.html 823) defines n(lambda_d) as the modulated specular_ior | 145 review | M2 | M2 dispersion |
| Kernel | emission_sidedness re-parses observer.bin on every call | 145 review | M2 | |
| Tools | Lock timeout counts from owner.json's `started`; while holders still run the old lock.ps1 (no owner file) the waiter's own wait is the clock, so a long queue of old-code holders can trip 3600 s | 141 | M1b | |
| Tests | abi test_session_lifecycle: ar_session_destroy took 6.7 ms against 5.0 ms once under load (138's gate); rerun green. Watch for repeats | 138 | M1b | the task that sees it again files it |
| Blender | F12 error race: smoke_a1 test_error_entry_fails_render failed once in 151's landing gate: the session rejected generation 0 and render_final read Combined ("no film") before _report raised the GENERATION_REJECTED entry, so the RenderFailed text lacked "astroray: GENERATION_REJECTED:". Check the report before reading passes when the state is FAILED | 151 landing | M1b | |
| Kernel | Wide textures: negative texels reach the integrator unclamped (fixtures hold -0.375, -0.75). Decide the rule (refuse at load, or clamp with a report); huge and non-finite texels are refused at load (167, 0007 d13) | 150 review | M1b | |
| Tools | test.ps1 can print more than 20 lines: lock waiting lines (up to ~14 over 3600 s) and WARN lines come on top of its 19+1 | 141 review | M1b | |
| Tools | test.ps1 -Exe astroray_tests_cpu (-NoMachineLock) captures build.ps1's tree-lock waiting lines and shows them only on failure, so a hung tree holder is silent (check fast has the same pattern) | 141 review | M1b | |
| Tools | %ProgramData%\astroray2 owner.json written by different Windows users or sandboxes may not be overwritable (default ACL): WARN plus "unknown holder", not a failure | 141 review | M1b | |
| Tests | test_batching.cpp timing checks flake under load: line 382 "session the publish cap holds every due frame during interaction" (0.0309 s against 0.0227 s) and line 413 "a cancel during the publish wait parks at once"; green on rerun (151's local check ci) | 151 | M1b | |
| Blender | render_final Job: if the first update() raises after full_sync filled icache, `first` stays True and a kept Job (persistent data) later builds from that cache without classify_updates (since 161 this covers own-row keys, light groups, cryptomatte ids). Clear icache on a failed update | 161 review | M1b | |
| Blender | 161's caches by uid rely on renames being named by the depsgraph; ID.rename(mode='ALWAYS') renaming the other, conflicting object is untested; InstanceCache table_ids/table_list never shrink (small) | 161 review | M1b | |
| Kernel | interface_ior_ratio divides by the stack IOR; specular_ior inputs are not range-checked, so a user IOR of 0 divides by zero once the false-interfaces task calls it: guard or validate there | 180 review | M1b | false interfaces task |
| Tests | `check fast` B-08 (10 s) and B-12 (0.41 s per test) fail when other worktrees build or test at the same time (seen by 139, 141, 154, 161, 180 and the lead, 10.6-28.6 s; the immediate rerun passes). check fast takes no machine lock, so concurrent load lands inside its window: decide between a lock for the timed part, a load check that refuses to time, or per-lane scheduling | lead, M1b slice 0 | M1b | |
| Tools | tools/test.ps1 line 32 accepts only `^tests/(abi|blender)/[^/]+\.py$`, so `-Py tests/blender/headless/*.py` is refused (Invoke-ArTestCase and canary.ps1 accept it): widen the regex with a fast_testrun case | 141 rebase | M1b | |
| CLI | cli/bench.cpp:131 stamps `machine` from COMPUTERNAME (a personal name on this PC) into every bench line; 140 replaces it in perf.ps1, but bench.ps1 output still carries it: stamp a neutral hardware id (GPU name, cores, threads) in the CLI | 140 | M1b | |
| Blender | Since 161, a scene with view-layer light groups reads ob.lightgroup for every object in bpy.data.objects on each instance build (S2 has no light groups, so B-31 did not measure it); reading only the owners rows (np.unique(pick)) would bound it | 161 review | M1b | |
| Tools | canary.ps1 171 once reported "inert: agree/bake environment cpu vs cuda still passes" with the cache-breaking hunk applied; a debug print showed the case failing and later runs were red: possible stale astroray_tests_gpu in the canary build (check which targets a GPU-only item rebuilds) | 171 | M1b | |
| Records | Ruling 82 (cap wins over frame-per-pass below about 8 ms) is in 0007 V12, d50 and 0009 B-34, but 0009 B-32 and 0007 d25 still say "a frame for a newer view after every completed pass": align them in the next write-back that touches either | 185 review | M1b | |
| Denoise | `use_denoising` defaults to False (task 198) until task 201's V11 table says the final denoise is met and the owner has seen its images; then flip it to Cycles' True | batch 3 critique | M1b | |
| Packaging | F12 denoise loads `astroray_oidn.dll` from 176's dev folder only; the extension zip must ship the renamed OIDN modules and load them by absolute path (0001 D9.1, D9.4, V5) | batch 3 critique | first release | |
| ABI | `ar_edit_set_graph` refuses target VOLUME (src/capi/scene_api.cpp ~293-304), so a volume graph compiled by 179 cannot reach the store through the ABI; 181 adds `ar_edit_set_medium` only. Open the target before the addon sends volume graphs (slice C, with 182 or the volume lowering task) | 179 | M1b | |
| Blender | 0006 D21: Volume Scatter phase options map to a PhaseKind or are reported; the VOLUME_COEFFS graph node (179) carries no phase and assumes HG, so the volume lowering must report a non-HG phase as NODE_APPROXIMATED | 179 review | M1b | |
| Tests | `lobe_metal/pdf integral` took 0.438 s on the hosted runner (168 landing, 2026-11-24) against B-12 0.41 s: near the limit on hosted `ci`; make it cheaper or move it to `check full` with its history (decision 38), never raise B-12 | 168 landing | M1b | |
| Perf | B-19 `b1m.idle_share` moves with machine load: the baseline code (7723d1a) read 0.2005 and 0.1283 back to back beside implementers against the 0.109 line; shares need a quiet-machine method or a load guard (task 203 step 3) | 176 landing | M1b | |
| Blender | `scenes/lib/blend_writer.py` writes Cycles `transparent_max_bounces = max_bounces` and does not carry Astroray's `transparent_bounces` (task 156): a scene with a non-default limit (d3_alpha_cards 16) does not round-trip through a .blend | 156 report | M1b | |
| Blender | `transparent_bounces` (task 156) has no panel row; blender/astroray/ui/panels.py (199's Light Paths panel) should show it | 156 report | M1b | |
