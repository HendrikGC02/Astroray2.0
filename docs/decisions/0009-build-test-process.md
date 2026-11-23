# 0009 — Build system, repository layout, testing, benchmarks and development process

Status: proposed (2026-11-14), revised after review (2026-11-14), reconciled with RULINGS.md (2026-11-14)

## Context

v1 lost most of its late effort to its own machinery: five build trees and a 30-minute translation unit, a 45-minute suite, GPU regressions merged because CI had no GPU, reference images calibrated on a buggy engine, SSIM gates that measured noise, and a process (status corpus, 334 specs, three copies of agent roles, dated directives) that grew without bound ([toolchain](../v1-lessons/toolchain-and-build.md), [testing and process](../v1-lessons/testing-and-process.md), [research 13 §A](../research/13-agent-process.md)). The owner wants short builds, fast tests, simplicity, scientific accuracy, full breadth in the first release, and development by Sonnet-class agents from small specs; the lead merges ([0000](0000-owner-decisions.md) #12); the repository is public (#6); this PC may be the GPU test machine (#13); no Cycles pixel parity (#1); one kernel source (#2).

The spine fixes the frame: CMake + Ninja + MSVC, one build tree; CUDA + OptiX on GPU, MSVC + Embree on CPU; per-kernel register and compile-time gates; reference images only from analytic results or independent oracles; HTML benchmark report and milestone showcases; rules enforced by scripts; few agent roles; one page of status ([SPINE](SPINE.md) §1, §2, §9). Spike A measured what the build and tests can cost: 1.3-2.5 s per device TU serially, 11.3 s for a 34-TU clean build at -j8, 0.35 µs per CPU BSDF evaluation, CPU/GPU agreement of 1.1e-6 on eval with FMA off ([spike A](../spikes/A-kernel-budget/RESULTS.md) §4-7). Spike B measured the Blender side ([spike B](../spikes/B-blender-end-to-end/RESULTS.md)). Per-kernel resource budgets (registers, stack, spills, growth) and the agreement thresholds belong to [0002](0002-kernel-architecture.md) D10/D12, and the first-frame gate to [0007](0007-blender-integration.md) D35 (ruling 21). Every other time budget in the project is stated once, here (ruling G2). This record runs all of them and owns one layout, one set of scripts, one budget table and one process.

## Decision

### Repository and toolchain

1. **Layout.** Every directory exists because a target, a test or the process needs it.
   ```
   CMakeLists.txt  CMakePresets.json  cmake/      presets call vcvars themselves; warnings; kernel module and embedding rules
   include/astroray/astroray.h                    the only public header: the C ABI (0001)
   src/kernel/          freestanding kernel headers, stages/, portability.h, launch_config.h, budget.toml (0002)
   src/kernel_gpu/      one .cu per stage + optix_traverse.cu   src/kernel_cpu/  one .cpp per stage (0002 D4)
   src/device/cpu/  src/device/cuda/   the only vendor-specific host code (0001 D5.2)
   src/denoise/dlss/    the DLSS backend, the only place NGX headers are included (ruling 39)
   src/host/            scene, scheduler, film, denoise glue, module loader, allocator
   src/capi/            C ABI implementation, ABI recorder/replayer (0001) -> astroray_core.dll
   cli/                 astroray_cli: info, render, bench, dump
   data/                generated tables + MANIFEST.toml (0001 D8; 0003 decides contents)
   blender/astroray/    the extension (GPL-3.0-or-later, own LICENSE); no engine code
   scenes/              canonical scene builders (Python) and meta.toml per scene
   tests/cpu/  tests/gpu/  tests/abi/  tests/blender/  tests/budgets/  tests/data/oracle/
   bench/               bench scene list and history/*.jsonl
   tools/               build check land bench status fetch release (.ps1), lib/, lint.py, report.py, budgets.toml
   third_party/         vendored headers + deps.toml; _cache/ (git-ignored) holds fetched binaries and headers
   assets/manifest.toml assets/_cache/ (git-ignored)
   docs/  tasks/  .github/workflows/ci.yml  .claude/
   out/                 git-ignored: build tree, bin/, check and land logs, evidence, reports
   ```
   When the build phase starts, the spike code is tagged `spikes-final` and deleted from `main`; each `RESULTS.md` moves to `docs/spikes/`.
2. **Toolchain** (versions in the `tools/build.ps1` header, checked at configure). MSVC toolset 14.44 from VS 2022 Build Tools 17.14, selected by the presets with `-vcvars_ver=14.44`. Locally the toolset line must be 14.44.x, and a mismatch fails (MEASURED installed: cl 19.44.35208, accepted by nvcc 12.8 for sm_120, spike A §1). Hosted CI requires cl ≥ 19.44 and logs the exact version it got, because hosted images update Visual Studio and research 08 S7 never checked their toolset. The full build number goes into `ar_build_info()`. Other pins: CUDA 12.8 (MEASURED, spikes A and B); OptiX 9.1 headers (MEASURED, spike A §5); CMake ≥ 3.31 (4.2.1 installed); Ninja 1.13.2; pwsh 7 for scripts (ASCII only, no `.bat`); tools run on Blender 5.3's bundled `python.exe` with `-I` (Python 3.13.13, NumPy 2.3.4; research 08 §2), with no separate interpreter or virtual environment (ruling 32), and hosted CI, which has no Blender, uses Python 3.13 with the same NumPy pin; Embree is pinned at 4.4.1, the newest 4.x release with Windows static-library support (task 007), built by `tools/deps/build_embree.ps1`; astropy for 0008's FITS checks is pinned with hashes in `deps.toml` (where it is installed under ruling 32 is open, Risks); Blender 5.3.0 at `$env:ASTRORAY_BLENDER`; `gh` CLI. The engine DLL is built `/MT` behind the C ABI, so Blender's bundled CRT version does not matter (research 08 option B; 0001 D4.4). Warnings are errors (`/W4 /WX`, nvcc `-Werror all-warnings`) in every preset. CPU code is compiled with `/arch:AVX2`: the CPU target requires AVX2 (ruling 15; the remaining production flags are 0002 D12's). One GPU compiler, nvcc ahead of time; there is no NVRTC route (ruling 16). The move to CUDA 13.x follows 0001 D4.3/V8 (no gated number regresses by more than 10%), measured with spike A's scripts checked out from `spikes-final` and with this record's budget gate, agreement suite and bench.
3. **Targets.** `astroray_kernel` (INTERFACE: kernel includes and definitions); `astroray_kernels_cpu` (STATIC, one TU per stage, `Real` = float) and `astroray_kernels_cpu_f64` (same sources, `AR_REAL_DOUBLE`, 0002 D3; linked into the tests; a test reference, never shipped, ruling 41); `astroray_kernels_gpu` (one cubin per stage plus the OptiX PTX module, 0002 D11/D14, **always embedded in `astroray_core.dll` as `RCDATA`**, 0001 D1.1; the `dev` preset also writes the same images to `out/bin/kernels/` for the dev reload of decision 5, the release build writes no loose modules, ruling 13); `astroray_kernels_gpu_nofma` (`-fmad=false`, built by `check gate`) and `astroray_kernels_gpu_probe` (launch bounds compiled out, built only by `check full`, ruling 17), same sources, never embedded, zero run-time cost; `astroray_host` (STATIC, no CUDA types in its interface); `astroray_core` (SHARED `/MT`, the only DLL Python loads); `astroray_cli`; `astroray_tests_cpu` (links no CUDA, so it cannot touch the GPU); `astroray_tests_gpu`; `astroray_extension` (stages `blender/astroray` + `out/bin` and runs `blender --command extension build`).
4. **Presets and trees.** One local preset, `dev`: optimised with embedded debug info `/Z7`, `CMAKE_CUDA_ARCHITECTURES=120-real`, GPU on. Preset `cpu` (GPU off, no CUDA needed) exists for hosted CI. Preset `release` is built from clean by `release.ps1` only and adds the architectures 0001 D3.4 declares; only sm_120 is ever tested. Each checkout has one tree, `out/build`: the lead's checkout plus at most two implementer worktrees, all on `dev`, each tree deleted with its worktree by `land merge`. `check fast` builds only CPU targets, so a worktree compiles device code only when its task needs GPU results (kernel tasks need their spill and stack numbers, 0002 D10 "Declare"). There are no per-configuration trees and binaries are never copied out of `out/bin`.
5. **Build identity and kernel source.** The core exports `ar_build_info()` (0001 D2.7: version, git sha, dirty flag, toolkit versions), extended with the kernel source: `embedded` or the directory in `ASTRORAY_KERNEL_DIR`. A generated build-info source is rewritten only when its content changes, so a new commit relinks the DLL and nothing else. After Ninja exits 0, `build.ps1` runs `ninja -n` on the same targets and fails unless it reports no work. This catches a missing or stale output through Ninja's own dependency graph; up-to-date outputs that are older than the build start are legitimate and are not checked. The addon, the CLI and both test executables print `ar_build_info()` at start; `check` fails if the sha is not `HEAD` (v1 shadow-module failures) or if any test ran on loose kernels. Dev reload: only when `ASTRORAY_KERNEL_DIR` is set does the engine load modules from that directory, and it swaps a changed module at the next step boundary (B-04).
6. **Dependencies.** Which libraries may be used, and at which version, is 0001 D9's closed list (ruling 20: a library not on it, texture decoders included, is not used). `third_party/deps.toml` is its machine copy plus build-only tools: per entry name, version, URL, SHA-256, SPDX id, kind (`vendored` | `fetched-binary` | `fetched-not-redistributable` | `tool-python`); lint fails on a shipped entry missing from 0001 D9.1. Vendored in git: doctest (single header) and the header-only libraries 0001 D9.1 approves. `tools/fetch.ps1 deps` (0001 D9.2's dependency script) fetches into `third_party/_cache` our Embree build (no TBB) and our OIDN build (renamed modules, static CRT, at 0001's one pinned version), the OptiX 9.1 headers from `NVIDIA/optix-dev` at a pinned commit, and the NGX SDK headers at a pinned SHA-256 (0008 decision 19; ruling 34); headers are never committed or shipped (research 11). Our Embree and OIDN archives are produced once by their recorded build scripts and published as release assets of this repository (ruling 12); those scripts and their tools (ISPC) are not part of the normal build or of any `check` tier. CUDA, MSVC and Blender are machine prerequisites. Configure never touches the network: no vcpkg, no FetchContent, no submodules. A checksum mismatch or a missing licence field fails. `THIRD_PARTY.md` is generated from `deps.toml`. Reference implementations (pbrt-v4, Cycles, OpenPBR, Adobe's openpbr-bsdf) are not dependencies: `third_party/refs.toml` pins each at a commit and archive SHA-256, `tools/fetch.ps1 refs` puts read-only snapshots in `<cache>/refs/` (`<cache>` is `ASTRORAY_DEPS_CACHE` or `third_party/_cache`), and they are never compiled, linked or listed in `THIRD_PARTY.md`; lint section `refs` checks the manifest and that no build file names them.
7. **No compiler cache.** Spike A builds 34 TUs from clean in 11.3 s; sccache never caches the outer nvcc call and v1 measured 0% device hits (research 08 §7). B-01 must hold with no cache. Revisit only if a measured clean build exceeds half of B-01. Ninja's nvcc pool depth is 8 (PROVISIONAL: spike A ran -j8 cleanly; v1 hard-reset under combined load, so a measurement task records peak RAM).
8. **Locks.** Every `tools/` script that compiles device code, runs the engine on the GPU, launches Blender or times anything takes one named mutex (`Global\astroray2`). The outermost script holds it and sets `ASTRORAY_LOCK_PID`, so nested calls do not re-lock (v1 deadlock). Builds, GPU tests and benchmarks therefore never overlap. `check fast` takes no machine lock: it compiles only CPU targets, with at most 4 `cl` jobs (PROVISIONAL), and runs no GPU or Blender code, so an implementer's 10 s loop never queues behind a 3-minute gate. Every script that builds also takes a per-tree lock file in `out/build`, so two Ninja runs never share a tree. Summaries report lock wait separately; tier budgets apply only to the time after the locks are held. Processes are killed only by the PID they wrote at launch. The lock is visible (task 141): the holder writes `owner.json` under `%ProgramData%\astroray2` (`ASTRORAY_LOCK_DIR` moves it, for tests) with `pid`, `tool` (the outermost script outside `tools/lib`), `cmd` (last 120 characters of the command line) and `started` (ISO time), written to a temporary file and moved, and removed on release only if it is the holder's own pid; a failed write warns and never blocks the lock. The tree lock keeps the same record in `out/build/.tree.owner.json`. A waiter wakes every `ASTRORAY_LOCK_MESSAGE_S` (default 30 s) and prints `waiting for <tool> (pid N) since <time>` (or `waiting for unknown holder (owner.json missing or its pid not running)`) on the first three ticks, then at most one line per 300 s, so a long legitimate queue does not fill the console. A record counts as live only if its pid runs and that process started no later than the record. `ASTRORAY_LOCK_TIMEOUT_S` (default 3600 s) bounds the current holder's hold, counted from `started` in a live `owner.json`: it detects a hung holder, and a queue of healthy holders is not a failure; with no live record the waiter's own wait is the clock. On expiry the waiter fails with `FAIL lock: waited N s for <tool> (pid N), holding N s since <time>, over the N s limit (ASTRORAY_LOCK_TIMEOUT_S); owner file <path>` (`unknown holder` when there is no live record). A mutex abandoned by a dead holder is taken with a `WARN lock: abandoned by ...`. `ASTRORAY_LOCK_NAME` replaces the mutex name for tests; a non-positive or non-numeric setting fails loudly.

### Scripts and tiers

9. **Three entry points** (plus `bench.ps1`, `status.ps1`, `fetch.ps1` and 0001's `release.ps1`). Each prints at most 20 lines (step, seconds, lock wait, pass/fail, final `PASS gate 141 s (budget 180 s, waited 0 s)`), writes the full log to `out/check/<tier>.log` and a JSON summary to `out/check/<tier>.json`, and exits non-zero on failure.
   - `tools/build.ps1 [-Target t]`: configures if needed, builds `dev`, runs the `ninja -n` check, prints the build id.
   - `tools/check.ps1 fast|gate|full|ci`: tiers below.
   - `tools/land.ps1 prepare|merge NNN`: decision 26.
10. **`fast`** (B-08, which times lint and tests but not the build step, owner decision 36: no GPU, no Blender, no machine lock): `tools/lint.py` (the decision-31 caps; SPDX header on every source file matching its directory and no NVIDIA headers or binaries in the tree, 0001 V4 repository part; kernel banned tokens and include allow-list, 0002 D1; ABI header rules, 0001 V2; vendor seam, 0001 V3; data manifest, 0001 V9 file part; asset rules, decision 21; `skip`/`xfail` names an open task; task-file format and canary patch, decision 28; `budgets.toml` ↔ this record and the declared family sizes, decision 15; ASCII `.ps1`); incremental build of `astroray_tests_cpu`; its deterministic tests only: pure functions, RNG and index bitwise checks, layout `static_assert`s, lobe consistency by fixed quadrature (pdf integrates to ≤ 1, `eval` = sample weight × pdf, `pdf()` = sampled pdf). No p-values, no random seeds.
11. **`gate`** (B-09, this PC): `fast`; build `dev` plus the nofma variant; kernel resource and compile-time checks, work counters and timing tripwire (decision 18); binary and ABI checks: `tests/abi/` layout and version test (0001 V1), `dumpbin /dependents` (V5, first part), the driver-check unit test and the CLI with no CUDA device visible (V6), record/replay bit-equality on the CPU (V11); `astroray_tests_cpu --tier=gate` (statistical suite at gate counts, analytic scenes on CPU at 32x32); `astroray_tests_gpu --tier=gate` in one process, serially, with a latent-CUDA-error check after each test (agreement steps 17(i)-(iv); analytic scenes on GPU at 32x32; film and denoiser tests, 0008); one Blender smoke (decision 17a). Every test selects its device explicitly. Gate steps that use different devices and share no output (analytic cpu with cuda, integrator cpu with the GPU ABI tests, cpu with gpu gate tests) run side by side under the one machine lock; the CUDA tripwire, the B-24 compile time and the steps that use both devices run alone, and lock waits inside the run are excluded from the tier clock like the machine-lock wait (decision 8).
12. **`full`** (B-10): `gate`; statistical suite at full counts with seed derived from the commit sha (seed rotation without a scheduler), fine and weighted chi-squared, the OpenPBR parameter-lattice audit; analytic scenes at high spp plus convergence slope; oracle scenes; CPU/GPU beauty test (17(v)); float-vs-double CPU; full Blender suite including the in-Blender module list (0001 V5, second part) and windowed timing for B-13/B-14/B-15; every device TU timed serially; the probe build's uncapped register counts, printed as warnings at 0002 D10's levels and never a failure (ruling 17); the compile-only resource check of every shipped SASS architecture (0001 V13, 0002 D14); the OptiX module's registers via Nsight Compute and traversal rays/s on a fixed scene through the wall-time A/B of decision 18; the engine time budgets B-17..B-37; `bench.ps1` with perf A/B against the previous milestone tag; a cold clean-build audit. Nothing runs unattended on a timer (ruling G3): every check another record calls "nightly" is part of `full`, which the lead runs on demand, before closing a milestone, before a release, and when a task's `Budget:` line says `full`. `ci` (hosted runners only) = `fast` + `cpu` preset build + the CPU part of `gate` (including V1, V6, V11 on the CPU DLL) + 0007 D46's headless sync suite against the `bpy` 5.3.x wheel (5.4 once published).

### Budgets (`tools/budgets.toml` is the machine copy, with the same ids)

13. Every budget fails at its limit. There is no warning band (the Commands section of CLAUDE.md: a failed budget check is the change's to fix, never to raise). Tier times exclude lock wait. PROVISIONAL limits are reset once from the first measurement on product code, with headroom, and from then on fail like the rest. This table is the only statement of every time budget in the project (ruling G2). The record named in "serves" owns the design the budget constrains and, where given, the measurement method; it cites the id and restates no number. Two rows point elsewhere because a ruling put the number there: kernel resources (0002 D10, ruling 17) and the first-frame gate (0007 D35, ruling 21). B-26 to B-37 collect the rows other records reported as moved here (ruling 29); B-28 to B-30 have no number on record yet and get one from their first measurement, like every PROVISIONAL row. Columns: tier that fails it; basis.

| id | budget | limit | serves | tier | basis |
|---|---|---|---|---|---|
| B-01 | clean build, `dev`, all targets, no cache | ≤ 5 min | here | `full` | owner; MEASURED 11.3 s for 34 device TUs at -j8 (spike A §4); product scale PROVISIONAL; MEASURED 43.5 s clean `dev` all targets at -j8 on main 1585e60 (091) |
| B-02 | incremental build after editing any one file, incl. a kernel header | ≤ 30 s | here | report | owner; MEASURED header touching all 34 TUs 11.2 s, one stage file 3.7 s |
| B-03 | host `.cpp` edit, rebuild + relink | ≤ 5 s | here | report | MEASURED 0.70 s cl + link |
| B-04 | dev kernel edit, one stage file, to running engine (nvcc rebuild + module swap) | ≤ 5 s | decision 5 | `full` | MEASURED 1.3-2.5 s nvcc per TU, module load 1.6 ms, about 1.1 s edit-to-launch per kernel (0005 Alternatives); a measured breach is the only reason to reopen runtime compilation (ruling 16) |
| B-05 | compile time per device TU, serial nvcc | ≤ 3.0 s | 0002 D10 | `gate` (≤ 3 changed TUs), `full` (all) | spine; MEASURED 1.3-2.5 s; method decision 18 |
| B-06 | per-kernel registers, stack, spills, launch-bounds check, growth | 0002 D10 | 0002 | `gate` (capped build), `full` (uncapped warning) | not a time budget; listed because the gate runs it (ruling 17) |
| B-07 | configure from scratch, no network | ≤ 10 s | here | `full` | MEASURED 7.38, 7.19, 7.12, 7.08 s over four clean configures (task 091); 10 s is 1.35 x the maximum |
| B-08 | `check fast` lint and test time; the incremental build step is outside the timed window and is reported on its own line (its rows are B-02 and B-03; owner decision 36) | ≤ 10 s | here | every run | owner; MEASURED CPU TU 0.65 s, 0.35 µs per BSDF eval (spike A §7); task 091: 6.2-8.5 s on a warm tree, and the first run in a fresh worktree (configure plus clean CPU build) took 42.8 s, a known first-run cost |
| B-09 | `check gate` wall time incl. incremental build; of which the kernel part (device builds, agreement suites, compile timing) | ≤ 3 min; ≤ 120 s | here, 0002 D10 | every run | owner; kernel part PROVISIONAL (about 20 s for a one-stage edit; MEASURED 15.5-15.6 s with no edit, 16.7 s after touching one stage, task 091; those are the no-kernel-change case, gates that touch kernels re-time the changed TUs and measured 31-35 s (tasks 119, 120, 122), so 120 s stays) |
| B-10 | `check full` | ≤ 12 min | here | every run | MEASURED 505, 497, 514 s net over three complete runs (task 131; wall time less every machine-lock wait, including Blender's, which the tier's `waited` does not count); 1.3 x 514 s = 668 s, rounded up to a whole minute |
| B-11 | hosted `check ci` | ≤ 7.5 min | here | every run | MEASURED 241-345 s job time over the last five `ci` runs (including the `bpy` wheel headless suite and the API-surface check) on image windows-2025-vs2026 with `cl` 19.51.36260.0, task 091; 1.3 x 345 s (runner: 4 vCPU, 16 GB, research 08 §10) |
| B-12 | one test: `fast` / `gate` | ≤ 0.41 s / ≤ 20 s | here | every run | `fast` part MEASURED: slowest test 0.304-0.315 s over 9 runs, 1.3 x 0.315 s (task 091); `gate` part PROVISIONAL (slowest CPU gate test 4.46 s; GPU per-test times not measured) |
| B-13 | present: `view_draw` at 1080p / 4K; held update rate at a 60 Hz producer, 1080p, 1440p and 4K | ≤ 2 ms / ≤ 8 ms (p99 ≤ 3 ms / ≤ 15 ms); ≥ 58/s | 0007 D29-D33 (method V8) | `full` | MEASURED spike B §1: 0.8 ms 1080p, 7.4 ms striped RGBA16F 4K, paced; synthetic producer 0.74 ms per 1080p update, Python overhead 0.014-0.03 ms per present (0001 D6) |
| B-14 | viewport update rate on bench scenes (ruling 30) | target ≥ 30/s at full 1080p on B-1M; floor: never < 24/s for > 0.5 s; 60/s is a later goal, reported, not gated | 0007 D34, D50 | `full` | owner #9, #21; PROVISIONAL (no product-scope kernel throughput yet) |
| B-15 | first frame vs Cycles viewport | 0007 D35 | 0007 | `full` | ruling 21: the number is stated only in 0007; spike B §3 (0001 D6): 1.42 s against Cycles' 1.28 s F12 and 0.97 s viewport, mesh and instance sync only |
| B-16 | work counters per fixed scene; timing tripwire | +2% / 1.25x of baseline | decision 18 | `gate` | counters deterministic per device (0002 D12) |
| B-17 | duration of any one kernel launch, bench scenes, 1080p | target 20 ms; fails above 100 ms | 0002 D7, 0007 D25 | `full` | ruling 14 (viewport responsiveness; far below the 2 s Windows TDR default) |
| B-18 | CUDA context and modules ready: warm / cold (after a driver update) | ≤ 260 ms / ≤ 490 ms | 0002 D11, D14 | `full` | MEASURED task 094 (`ar_scene_create` to the first pass complete, 10 fresh processes each): warm median 157 ms, max 193; cold (empty OptiX `cache_dir` and `CUDA_CACHE_PATH`) median 364 ms, max 375; the limits are 1.3 x the maxima. Cold OptiX module 86 ms, motion variant 96 ms; spike A §5 had about 1 s per module (PTX), now cubins |
| B-19 | 1080p 1 spp pass: GPU idle between launches; `queue_build` + sort GPU time | each ≤ 10% of pass time | 0002 D6, D7 | `full` | PROVISIONAL; DERIVED from 0002 D7's counts: about 26 steps, partition traffic about 40 MB per step ≈ 50 µs at 896 GB/s, round trip 20-100 µs assumed, so 1.8-3.9 ms against a 9.5 ms pass (1.25 × spike B's 7.6 ms), 20-40%: breached before 0002 D7's remedies |
| B-20 | kernel time per 1080p sample, S1 / S2 | ≤ 1.25x Cycles (7.6 ms / 10 ms) | 0002 D9, 0007 | `full` | PROVISIONAL (spike B Cycles timings) |
| B-21 | any engine call on Blender's main thread, except copying data the host supplied | ≤ 5 ms | 0007 D23 | `full` | PROVISIONAL |
| B-22 | edit to start of the new pass (accumulation reset), excluding sync and rebuilds | ≤ 2 ms | 0004 D25 | `full` | PROVISIONAL (about 20% of the engine's share of a 60/s frame) |
| B-23 | light-structure build, counted inside first frame | ≤ 50 ms per 10k lights | 0004 D25 | `full` | PROVISIONAL; 1M emissive triangles to be measured |
| B-24 | material: native compile; translation per material (median); S2's 300 materials; re-translation of an animated material | ≤ 2 ms per 100 nodes; ≤ 1 ms; ≤ 300 ms; ≤ 1 ms per frame | 0005 (Verification 7, M-8) | `gate` (compile), `full` (rest) | PROVISIONAL |
| B-25 | per viewport frame: render, resolve, denoise or DLSS, and pack | ≤ B(rate) = 1/rate − t_present, rate = B-14's target | 0008 (frame budget) | `full` | t_present to be measured (0008 Risks 9) |
| B-26 | viewport OIDN denoise per presented frame, 1080p and 4K | ≤ B(rate)/2 at B-14's target; above it quality steps down (0008 decision 18) | 0008 decision 18 (method V11) | `full` | 0008 review item 1 (the B(rate)/2 share) |
| B-27 | DLSS-RR pack + evaluate per frame, at least one preset | ≤ B(rate)/2 at B-14's target for 1080p output and at B-14's floor for 4K | 0008 decision 19 (method V12 part A(c)) | on demand (spike F) | 0008 review item 1; ruling 30 |
| B-28 | per-pass resolve and `sample_add` traffic, 1080p and 4K, 1 spp per pass | resolve ≤ 11% of 1/rate at B-14's target (1080p), ≤ 33% of 1/rate at B-14's floor (4K); `sample_add` reported, not gated | 0008 decision 3 (Risks 1) | `full` | MEASURED task 094 (Nsight Systems, B-1M, 40 passes): `k_film_resolve` 2.76 ms at 1080p (8.3% of 33.3 ms), 10.30 ms at 4K (24.7% of 41.7 ms); the limits are 1.3 x those; t_present is not yet measured (B-25), so the share is of 1/rate; `sample_add` is atomics inside the shade kernels, no kernel of its own |
| B-29 | statistics overhead: time per 1080p sample with every statistic on vs off | ≤ 2.4% more time per 1080p sample with every statistic on than off | 0004 D28-D29, 0008 decision 12 | `full` | MEASURED task 126 (B-1M, 1080p, 280 passes each in alternating rounds, `ar_progress.last_pass_ms` at each new sample count): on 13.425 ms, off 13.184 ms median (+1.83%); the limit is 1.3 x that; "every statistic" is the channels with a request path (bounces; the sample count is the resolve's n), since termination, time and rays have none yet |
| B-30 | VM overhead: textured and procedural Principled vs constant Principled, kernel time per sample | set from M-3 | 0005 (M-3) | `full` | PROVISIONAL; unmeasured |
| B-31 | main-thread sync, S1/S2 cold and warm: full mesh read; positions-only update; instance loop; plain move | ≤ 65 ms per 1M triangles; ≤ 6.0 ms per 1M (S2 size: 153 MB per read, memory-bandwidth-bound; S1's 30 MB fits in cache at 0.97); ≤ 30 ms per 10k instances; ≤ 0.1 ms | 0007 D17-D22 (method V3) | `full` | MEASURED spike B §2 (0007 D19, D22); PROVISIONAL |
| B-32 | latency: F12 cancel; one view change to its first frame; continuous orbit | stops at the next launch boundary (≤ B-17); ≤ rest of the pass in flight + one pass + present; a frame for a newer view after every completed pass, zero stale frames | 0007 D25, D27 (method V12) | `full` | 0007 D25 derivation |
| B-33 | resolution-controller response | step down within 100 ms when no frame arrives; reach B-14's floor within 0.5 s; full resolution within 250 ms of interaction stop | 0007 D34, D49 (method V11) | `full` | 0007 V11; PROVISIONAL |
| B-34 | progressive batching (ruling 30): frame publish rate; display interval after interaction | ≤ 60/s during interaction, ≤ 10/s otherwise; the display interval grows on Cycles' viewport schedule (`render_scheduler.cpp`): 0.1 s for the first 1 s of path-trace time since the last view or scene change, 0.25 s to 2 s, 0.5 s to 4 s, 1 s to 8 s or while under 32 samples, then 2 s; passes per update = max(int(interval / average pass time of the epoch), 1), rounded to the nearer power of two, a tie going up (Cycles' `round_num_samples_to_power_of_2`) | 0007 D30, D50 | `full` | owner #22; PROVISIONAL |
| B-35 | curved-region branch: `intersect_closest` with vs without it, flat 1M-triangle scene | median-of-3 runtime ratio ≤ 1.05 | 0003 decision 28 (check 16) | `full` | spike A §1 noise |
| B-36 | light-pass share of pass time (ruling 29) | ≤ 25% | 0004 D20 (method: 0004's window sweep, which resets it) | `full` | PROVISIONAL (0004 review item 11) |
| B-37 | timing constants: completion poll period; F12 wait timeout; F12 preview interval; interaction window; controller step-up hold; viewport OIDN at-rest delay; `specgen` total run time that would move table generation into the build | 50 ms; 100 ms; 1 s; 250 ms; 0.5 s; 0.25 s; ≤ 20 s | 0007 D26, D27, D34, D49; 0008 decision 18; 0003 Risk 9 | `full` (specgen: report) | PROVISIONAL; constants, not limits, kept here under ruling 29 |

### Tests

14. **Kernel unit tests on the CPU target** (doctest, `astroray_tests_cpu`). Every lobe, light, sampler and spectral function is a pure function taking explicit uniforms and returning value, pdf and flags (requirement on 0002-0005). Agreement tests link `astroray_kernels_cpu`; precision-oracle tests link `astroray_kernels_cpu_f64`.
15. **Statistical tests and flake policy.**
    - **Per lobe/light configuration:** (a) energy test: mean of f·cos/pdf with its standard error, against the closed-form albedo where one exists (Lambertian ρ; 1 for lossless configurations; smooth Fresnel), otherwise against deterministic quadrature of eval·cos, which checks only that sampling and evaluation agree (physics truth then comes from O scenes and 0005's reference checks); (b) chi-squared of samples against the pdf integrated per cell, cells pooled below expected count 5, `ires` declared per lobe (Pearson 1900; harness pattern of pbrt-v4 `bsdfs_test.cpp` and Mitsuba `chi2.py`); (c) in `full`, weighted chi-squared against ∫f·cos per cell.
    - **Family size.** Each statistical suite (gate statistical, full statistical, analytic scenes) declares a constant family size m in `budgets.toml` (PROVISIONAL m = 2,000 for the gate suite). Every per-test threshold comes from Šidák at family α = 0.01 with that m (Šidák 1967; per-test α ≈ 5e-6 at m = 2,000). Lint fails when a suite's test count exceeds its m, and raising m is a reviewed budget change. Adding a test therefore never moves another test's threshold, a fixed-seed result is reproducible across commits, and power does not erode as the suite grows.
    - **Seeds.** Gate seeds are hash(test name, repo constant). No seed value is special (ruling 3; the sampler and its seed handling are 0003's). Every failure prints its seed and a one-line reproduce command. A chi-squared failure re-runs at 2x `ires` before it is reported (v1 glass false reds).
    - **Rotated-seed failures.** A failure on a `full` rotated seed is never dismissed. It is re-run on 20 further seeds; more than one failure in 20 is a defect, in the code or in the test's calibration, and keeps `full` red until it is fixed (research 09 §3). Otherwise the seed joins that test's pinned gate seeds and a task investigates both sides, because a low-power real bug looks exactly like this.
    - No quarantine: `skip`/`xfail` only with an open task id (lint).
16. **Analytic and oracle scenes.**
    - **Analytic scenes** compare the film mean with closed form: |mean − truth| ≤ z·SE + eps_sys, with z from the suite's declared m and eps_sys declared per scene in `meta.toml` (default 0). The test reads the film's fp64 first and second moments, resolved from the per-sample buffer as 0008 specifies (ruling 1). Every image test also asserts the image is not constant and not black where truth is not (v1 black-against-black).
    - **Oracle scenes** use Mitsuba 3 `scalar_spectral` and pbrt-v4 (spectral) with explicit spectra, never RGB. The test is a per-pixel z with a variance floor, Šidák across the scene's pixels at 0.01, passing at ≥ 99.75% of pixels (Mitsuba `test_renders.py` pattern). An oracle is adopted for a scene only when the two oracles agree with each other on it. Where one oracle lacks the physics: dispersion (Mitsuba's dielectric has no spectral IOR) is adopted from pbrt-v4 with A10, the analytic prism of the same glass, as second witness; layered coat is informational only, because OpenPBR's coat is the specification's albedo-scaling model rather than physical layering, so its conformance comes from 0005's reference-implementation checks; `meta.toml` records single-witness status. An oracle disagreement is investigated on both sides and, if the physics is ours, recorded as a labelled divergence in `meta.toml`, never loosened.
    - **References.** There are no self-generated golden images (spine). References are written only by `tools/check.ps1 bless <scene>` (lead only), which records tool version, command, seed and spectra hashes in a sidecar.
17. **CPU/GPU agreement**, one ladder:
   (i) bitwise: integer code, sampler values, indices, hashes over 1e6 inputs, zero differences;
   (ii) nofma GPU vs CPU float, and (iii) production (FMA) GPU vs CPU f64, per stage on 1e6 fixed host-generated inputs: 0002 D12's thresholds and outlier rule, the single statement (the shipped binary is tested by (iii));
   (iv) deterministic AOVs (position, normal, material id, UV) on scene D2: ids equal, positions ≤ 1e-5 relative, coincident-face ties excluded;
   (v) `full` only: beauty images in 8x8-pixel tiles, a Welch test (Welch 1947) per tile on 8 independent seeds per backend, Holm correction (Holm 1979) at family α 0.05, 1% relative margin, pass = zero rejected tiles. This is the configuration v1 validated ([v1 testing research](../v1-lessons/research/testing.md) lines 25-33). v1 null-calibrated it only at 4 vs 4 and on three scenes, so 17(v) gates nothing until a null calibration on this engine passes (Verification);
   (vi) `full` only: CPU float vs CPU double render of the same scene agree within noise.
   17a. **Blender end-to-end.**
   - **Launcher** `tools/lib/blender.ps1`: `--background --factory-startup --python-exit-code 1`, `BLENDER_USER_{CONFIG,SCRIPTS,EXTENSIONS,DATAFILES}` under `out/blender_home/<run>`, killed only by its own PID (spike B §0 method).
   - **Gate smoke:** install the staged extension zip, then render A1's `.blend` (written by its builder in headless Blender) at 64x64 through the addon, and check the image analytically. Exporter equivalence: replay the addon's recording and the builder's recording, and compare deterministic AOV dumps of the two renders (ruling 59 removed scene-state read-back). The two producers may differ in call order and in shipping polygons or triangles (0007 D10).
   - **`full`:** node coverage (every `ShaderNode*` type in 5.3, 108 types: implemented, or magenta plus a report entry; none silent); edit round trip (apply an edit sequence through `view_update`; the final state dump equals that of a full re-sync); windowed timing for B-13, B-14 and B-15 using spike B's harness pattern.
   - Blender 5.4 runs are informational until 5.4 ships.
18. **Performance, register and compile-time gates.**
    - **Merge gates (deterministic or gross).**
      - *Kernel resources (ruling 17):* spills and stack of the shipping capped cubins against the allowances recorded in `src/kernel/budget.toml`, plus 0002 D10's launch-bounds check and growth rule, measured its way (`cuobjdump -res-usage` cross-checked with `ptxas -v`). The uncapped register count from the probe build is a `check full` warning, never a merge failure. The gate prints each delta next to the task's declared delta.
      - *Compile time:* one serial nvcc call per TU, cache off, OS file cache warm (0002 D10). The gate times the changed TUs, at most the three with the largest recorded time, because a shared-header edit changes every TU and timing them all serially would cost about a minute at spike scale. A time within 10% of the limit is re-timed and the minimum of 3 kept. `full` times every TU. Times are never read from `.ninja_log`: at -j8 each TU runs about 1.6x slower than serially (spike A §4, 4.9x speedup on 8 jobs).
      - *Work counters (B-16),* on B-1M and the A fixtures at fixed seed, on GPU and CPU: rays traced (closest, shadow), queue steps and launches per pass, path-state and shadow-item bytes, allocator totals per category (0002 D13). The gate fails above +2% of `tests/budgets/counters.toml` unless the same change re-blesses it, so the reviewer sees the change. BVH nodes visited are not observable under OptiX hardware traversal and are not counted.
      - *Timing tripwire (B-16):* B-1M kernel time per 1080p sample at 1 and 64 spp, after burn-in, minimum of 5. It fails above 1.25x of the baseline in the same file. That is far outside the measured noise (0.5-2% spread, spike A §1; about 5% clock drift in v1), so it does not flake and still catches gross regressions per merge.
      - *Tier durations* against B-08..B-12; B-05 for the timed TUs; the material compile row of B-24 on 0005's VM fixtures.
    - **Wall-time A/B** (soft; in `full` and on demand). `bench.ps1 -Compare <ref>` runs fixed workloads timed with CUDA events: 15 burn-in runs, then 15 interleaved base/candidate pairs, analysed with a Wilcoxon signed-rank test on the paired log ratios (Wilcoxon 1945). A regression is a median ratio > 1.05 with p < 0.01 that persists across 3 allocation shifts (a dummy 6, 32 or 64 MB allocation first; [v1 testing and process](../v1-lessons/testing-and-process.md)). Benchmarks run under the machine lock with no implementer active.

### Scenes and assets

19. **Scene format.** A scene is an ABI recording (`.arrec`, 0001 D1.4: every ABI call and its arrays, versioned; replay refuses a mismatch). Producers: `scenes/*.py` builders that call the ABI through the generated binding with recording on (stdlib + NumPy; the same builder also emits Mitsuba XML and pbrt input for oracle scenes, and a `.blend` where Blender tests need one), and the addon with recording on (`ASTRORAY_RECORD=<path>`; one branch per ABI call when off). Recordings are generated into `out/scenes/` by `check`, never committed. Consumers: `astroray_cli`, tests, benchmarks. Bug reports and benchmark scenes become recordings that replay without Blender. Tiny unit-test scenes are built in C++ directly through the ABI. Analytic scenes prefer shape-independent setups (a convex object in a uniform environment) so tessellation does not enter the truth.
20. **Canonical set.** `meta.toml` per scene states class, truth, tolerance, spectra, units (metres), assets and single-witness status.
   - **Analytic (A)** (truth formulas in research 09 §9). A1 furnace: convex object in a uniform environment, **lossless configurations only**: pixel = 1 for every white non-absorbing lobe and stack (0005 V1's energy target), ρ for Lambertian; lossy microfacet, coat and sheen configurations have no closed-form albedo and truth taken from the lobe itself would be circular, so they go to the energy test (15a) and O3. A2 closed Lambertian sphere interior, L = Le/(1−ρ); A3 disk emitter over a plane; A4 sphere, point and spot lights; A5 view-filling emitter through pinhole and thin lens, pixel = L; A6 dielectric slab with Beer-Lambert absorption, R + T and slab transmittance; A7 homogeneous absorbing slab and scattering furnace; A8 blackbody emitter to a band film (Planck band integrals, photon counts); A9 environment map with one bright texel (deterministic quadrature); A10 prism with line emitters, deviation from Snell's law and the glass's Sellmeier fit; A11 grating, diffraction-order angles from the grating equation (Born and Wolf 1999) via the light-tracing pass (owner #17); A12 fluorescent slab under monochromatic light, re-emitted photons = quantum yield × absorbed (definition of quantum yield, Lakowicz 2006); A13 uniform emitting-absorbing slab, I = S(1 − e^−τ) (Rybicki and Lightman 1979); A14 Schwarzschild: critical impact parameter 3√3 M, weak deflection 4M/b, redshift (1 − 2M/r)^−1/2 (research 09 S17; Bardeen 1973 for the Kerr generalisation).
   - **Deterministic (D):** D1 UV/texture chart AOVs (orientation, sRGB decode, Blender bottom-up pixel rows); D2 AOV geometry agreement scene; D3 node-coverage `.blend`; D4 edit-sequence `.blend`.
   - **Oracle (O):** O1 spectral Cornell-style box with explicit spectra; O2 Veach MIS (Bitterli, CC0) with explicit spectra; O3 rough conductor and rough dielectric rows (lossy-lobe truth); O4 heterogeneous grid medium.
   - **Benchmark (B):** B-1M and B-5M (spike B's procedural S1 and S2: 1.0 M triangles; 5.04 M unique triangles in 10,001 instances); B-TX (0007's textured S3); B-CR Blender Classroom (manifest, CC0 reported); B-NB black hole in an emission nebula.
   - **Showcase (G):** chosen per milestone in `docs/ROADMAP.md`; owner-modelled scenes from research 16 "Gaps" (spectral Cornell box, dispersion cabinet, gas-discharge lamp gallery, measured-metals tile, black hole in a nebula).
21. **Asset rules** (research 16 "Asset handling"). `assets/manifest.toml` per asset: name, URL, SPDX id or `none-stated`, size, SHA-256, redistributable, citation, units. `tools/fetch.ps1 assets` downloads named assets into `assets/_cache`, failing on a checksum mismatch or a missing licence; Poly Haven assets are fetched by name, never crawled. Only CC0/CC-BY/MIT/Apache files ≤ 2 MB may be committed; share-alike, non-commercial, no-derivatives and "not stated" never enter the repo. No Git LFS. Total `tests/data` + `scenes` ≤ 50 MB (lint). Evidence and gallery images are ≤ 2 MB and ≤ 1600 px, never blank or near-black. `ASSETS_NOTICE.md` is generated.

### Benchmarks and showcase

22. **Report.** `tools/bench.ps1` runs `astroray_cli bench` on the B scenes plus the Blender first-frame and viewport harness, and appends one JSON line per (sha, machine, driver, CUDA, scene, metric, value, unit, n, spread) to `bench/history/<year>.jsonl`. `tools/report.py` (stdlib + NumPy, inline SVG, base64 PNG, no network needed to view) writes `out/report/index.html` with: convergence (relMSE vs spp and vs time, log-log, against analytic truth or an independent high-spp reference, with the N^−1/2 line); throughput (Mpaths/s, samples/s) and work counters across commits; the first-frame breakdown (sync, transfer, acceleration build, first sample) next to Cycles; viewport updates/s and resolution scale while orbiting; build times per edit class and tier durations with budget lines; registers, stack and spill per kernel with 0002's lines; CPU vs GPU throughput; statistics heat maps (sample count, bounce count, termination reason, time; owner #15) and the renders. History lines are committed by the lead only, from `full` runs.
23. **Showcase gallery.** Each milestone ends with its G scenes rendered through Blender headless with the addon (so colour management matches what users see), each with a JSON sidecar (scene, spp, time, settings, build id). Images and the HTML report are attached to the milestone's GitHub Release. The repository keeps only scene definitions and sidecars, so history does not grow by tens of MB per milestone.

### Continuous integration (public repository)

24. **Hosted CI** (`.github/workflows/ci.yml`, GitHub-hosted Windows runner, `permissions: contents: read`, no secrets). It runs `check ci` on every pull request and every push to `main`. It catches what the dev PC cannot: a clean checkout, missing files, stale-artifact dependence and the actual head commit (v1 lessons).
25. **Local GPU gate.** `land.ps1` runs `check gate` on this PC under the lock and posts the result as commit status `astroray/gate` on the tested sha (`gh api .../statuses/<sha>`, description = the summary line). Branch protection on `main` requires `ci`, `astroray/gate` and an up-to-date branch. **This PC is not registered as a self-hosted runner:** GitHub advises self-hosted runners only for private repositories, because a fork PR can run code on them (research 08 §10); a runner would also take the GPU outside the machine lock and goes offline with the PC. Fork PRs get hosted CI; the GPU gate runs only after the lead has read the diff.

### Development process

26. **Landing (lead only).** **`land.ps1 prepare NNN`** runs in the task's worktree: (1) refuse unless on `task/NNN-*` with a clean tree; (2) fetch and rebase on `origin/main`; (3) canary check: apply `tasks/NNN.canary.patch`, rebuild, run the test cases named in the task's `Canary:` line, require each to fail, revert (a diff that adds or changes a test without a canary patch is refused); (4) write `out/land/NNN/task.md` (the task file as it stands) and `out/land/NNN/diff.patch` for the reviewer, who has no git; (5) delete `tasks/NNN*` in a commit, push, run `check gate`, post the status. The lead spawns the reviewer on that sha with those two paths and, for visual changes, opens the evidence PNGs. **`land.ps1 merge NNN`** requires the PR head to equal the gated sha, `ci` and `astroray/gate` green, a lead comment `REVIEW PASS <sha>`, and an `evidence:` line if the task has `Look at:`; then `gh pr merge --squash`, confirms the squash commit is on `origin/main`, and deletes the branch and the worktree with its `out/`. If `main` moved, re-run `prepare`.
27. **Roles and models** (aliases, never model ids; `effort` pinned in every agent file).

| role | model / effort | tools | job |
|---|---|---|---|
| lead | main session, opus | all | ADRs, tasks, STATUS, landing, looking at renders, root cause of gate failures |
| `implementer` | sonnet / medium (high for kernel or physics tasks, per invocation) | default, `isolation: worktree`, `maxTurns` set | one task file, its canary patch |
| `reviewer` | opus / medium (high for kernel or physics diffs) | Read, Grep, Glob only (harness-enforced read-only; `land` runs the checks and supplies `out/land/NNN/`) | reads `out/land/NNN/task.md`, `diff.patch` and `canary.patch` when present, and main sources; finds only: acceptance unmet, CLAUDE.md rule broken, a test that would pass on broken code (names the breakage), algorithm without `Ref:`, a code defect (race, lifetime, NaN path, silent drop); no style; last line `VERDICT: PASS`, `PASS WITH FIXES` or `FAIL` (owner decision 36) |
| `digest` | haiku / medium, `omitClaudeMd: true` | Read, Grep, Glob | cited digests ≤ 60 lines; prompt < 100K tokens (5x price above, research 13 §B) |

   Implementers are limited per lane (owner decisions 31 and 36, replacing "at most two at once"): any number of CPU-only tasks the lead can rule for, one GPU lane and one Blender lane, since the machine lock serialises those. Dependent work stays on one implementer; fan out only for independent tasks (research 13 §B). One hidden skill, `land` (`disable-model-invocation: true`), scripts the prepare → review → look → merge sequence. The open-weight worker (owner decisions 32 and 36) is `tools/lead/deepseek.sh`; it does only mechanical edits on repository text and returns a diff and transcript.
28. **Task spec** (`tasks/NNN-slug.md`, ≤ 60 lines, written by the lead, deleted by `land`; open questions must be empty before dispatch):
   ```
   # NNN Title
   Goal: before -> after, two sentences.
   Read first: the records, rulings and files the implementer reads before coding.
   Scope: files or directories that may change.
   Out of scope: what a reader might assume is included.
   Reference: paper, section/equation, reference file path, licence (required for physics, sampling, numerics).
   Do: numbered steps.
   Acceptance: `tools/check.ps1 fast` passes; `<exact command>` prints <value or threshold>.
   Look at: out/evidence/NNN/<name>.png, or none.
   Canary: tasks/NNN.canary.patch (implementer writes it) must turn red: <test case names>.
   Budget: expected effect on build/test time and register delta ("none", or "full" to require a full run).
   Depends on: tasks that must land first, or none.
   Size: expected diff; stop and report at 2x.
   Progress: (implementer appends at most 10 lines; WIP commit after each acceptance step)
   ```
   The `Canary:` line is `none` or `... must turn red: <items>` (task 137); `tools/canary.ps1 NNN` in the worktree and `land` run the same code (`tools/lib/canary.ps1`), and `tools/lint.py` checks the line's shape in `fast`. An item is a backticked command (run through `pwsh -Command`, or `python -I` for a `.py` command; a `$` inside double quotes there is a lint finding, use single quotes), or a test executable (`astroray_*`) or `tests/abi`/`tests/blender` `.py` file followed by one or more quoted case names (`"suite/case"`, `"Class.method"`); a quoted name before any executable or file, or an executable or file with no name, is refused. Refusals before any run: the patch touches a file under `tasks/`, holds CR bytes (write it with `git diff --output=`, never `>`), or fails `git apply --check` (stale after a rebase). Every item must run and pass on the tree without the patch (a passing baseline is skipped only on a clean tree with the same tree hash and item list), and every item must turn red under the whole patch: an item that errors, does not exist or still passes is a problem. Named cases of one `tests/abi` or non-GUI `tests/blender` file share one start, and each case's outcome is read from its own line; a crash or timeout errors every case of the file. The matrix then applies each hunk alone and records which cases it turns red; each hunk must apply by itself, every case must be red under at least one single hunk (one breakage per test group; hunks must not need each other), and a hunk that turns nothing red is a WARN. The matrix is skipped, with a WARN, above 8 hunks; `-NoBaseline` and `-NoMatrix` skip their parts on request. If `tools/scenes.py` fails after a patched build, every item of that build counts red (the patch broke the recordings the items read; task 138); on the baseline build the same failure is fatal. The tree is restored by reverse-applying the patch, and a tree left dirty is a failure.
29. **Status and records.** `docs/STATUS.md` ≤ 50 lines, overwritten by the lead only: milestone and goal, open owner questions, red items with task ids, budget headroom from the last gate. Live facts (branches, PRs, last gate) come from `tools/status.ps1`, never stored. History is `git log`; no journal, standup or round log. `docs/ARCHITECTURE.md` is the current synthesis; ADRs are the rationale. A superseding ADR deletes the superseded file in the same commit and names it with `git show <sha>:<path>` (pending owner question 4; the current CLAUDE.md keeps superseded files).
30. **v1 knowledge.** `docs/v1-lessons/` is the only channel. A `Read`/`Edit` deny on the v1 path sits in the git-ignored `.claude/settings.local.json` (the public repo must not carry a personal path). When a lane needs a v1 fact it says so in its report; the lead runs one `digest` call from a separate v1-rooted session and writes the answer back as a lesson with its citation. A lesson made moot by an ADR is deleted, not annotated. No agent that has read v1 code copies it; data items (tables, line lists) are regenerated or copied by script with provenance.
31. **Anti-accretion rules** (all lint-checked in `fast` unless noted): CLAUDE.md ≤ 60 lines; each `.claude/rules/*.md` (path-scoped, created only after a failure recurs) ≤ 40; agent file ≤ 60; skill ≤ 80; STATUS ≤ 50; task ≤ 60; v1-lessons file ≤ 150; `tools/README.md` ≤ 30; a build-phase ADR ≤ 80 (research 13; the planning records 0001-0009 are exempt, being synthesised into `ARCHITECTURE.md` at sign-off and changed later only by short superseding ADRs). At most 3 agent files, 2 skills, 0 Claude hooks, 0 git hooks, no `AGENTS.md`/`.codex`/`.opencode`. No dates or model ids in CLAUDE.md, rules or agent files. Adding a rule, agent, skill, hook or script names the incident in the PR body and, at a cap, deletes something (reviewer). One-off scripts are deleted when their task lands. Process changes (`.claude/`, `tools/`, CLAUDE.md) land as their own commits, never inside a feature diff. At each milestone close the lead runs `/doctor prompt-audit` and deletes prose rules with no incident since the last milestone. Auto memory is off (`autoMemoryEnabled: false`); durable lessons go to reviewed files.

### Challenges to the spine

The challenges on NVRTC and on the register gate were settled by rulings 16 and 17 and the spine was amended; they are no longer open here.

- **Spine §2, agreement "with a test build that disables FMA contraction."** Kept, and extended with ladder step 17(iii) (0002 D12), because the nofma binary is not what ships.
- **Spine §9, "one build tree."** Read as one configuration: v1's failure was five differently configured trees and shadow binaries. This record allows one `dev` tree per checkout (≤ 3 at once), guarded by the build-id check (decision 4).
- **Spine §9, "rules are enforced by scripts, not hooks."** Read as also excluding git hooks. The implementer and `land` run `fast`; no pre-commit hook does, because a blocking hook fights the WIP-commit rule that protects against usage-limit kills (v1). Research 13 R2 proposed a pre-commit hook; this record overrides it.

## Alternatives considered

- **NVRTC dev path vs nvcc only:** the second compiler bought about 1.1-1.4 s per edit (spike A §4-5) against flag and header drift and an identity test; dropped by ruling 16.
- **Loose kernel modules in releases:** a stale-file mismatch class (v1); 0001 embeds them.
- **sccache/ccache:** sccache cannot cache the outer nvcc call or the fatbinary and needs base-dir and `/Z7` care; v1 got 0% device hits and dropped connections on long compiles (research 08 §7, v1 toolchain). The measured clean build makes a cache unnecessary.
- **vcpkg/FetchContent:** both add network access or source builds to configure; three binary dependencies do not justify a second package manager (research 08 option G).
- **CPU kernel hot-reload DLL (research 08 rec. 7):** the CPU TU compiles in 0.65 s and is used by test executables that relink anyway; no Blender workflow needs it.
- **Worktrees on the `cpu` preset only:** this would push every register and GPU result for kernel tasks to `land`. `fast` already builds only CPU targets.
- **Self-hosted runner on this PC:** unsafe on a public repository (GitHub guidance) and fights the machine lock; replaced by the posted `astroray/gate` status.
- **Hosted GPU runners:** need a paid plan (research 08 §10), and are not this GPU.
- **Python/pytest-centred tests (Mitsuba style):** need vectorised bindings of every kernel, and Python start-up inside the 10 s tier; Blender's Python has no pytest. C++ for engine tests; Python only for Blender drivers, scene builders, ABI tests and the report (research 09 option 1).
- **SSIM/FLIP or Cycles-style pixel tolerances as gates:** they measure noise, not bias, and accrete blocklists (research 09 §C, §G; v1 testing research). FLIP stays a human diff map on converged images only.
- **Self-golden regression images:** cheap but can bake in bugs (v1); the spine excludes them.
- **Run-wide Šidák family; Mann-Whitney on paired timings:** rejected in review (decisions 15, 18).
- **Nightly/weekly scheduled tiers (research 09 §8, 0002 D10):** require an unattended scheduler on the owner's PC; excluded by ruling G3. Commit-derived seeds in `full` give seed rotation; per-merge counters and the tripwire catch performance drift early.
- **Tasks as GitHub issues or one TASKS.md:** issues grow without bound and need network; one file becomes STATUS.md (research 13 option 2).
- **Git LFS for references and gallery:** paid bandwidth on a public repo and a second tool; release assets and a 50 MB cap instead.

## Consequences

Easy: an agent needs three commands; every claim has a script output; budgets fail loudly; Blender bugs become replayable recordings; CPU tests cannot touch the GPU by construction; an implementer's `fast` never waits for the GPU; canaries are verified by `land`, not self-reported; the public repo carries no NVIDIA headers, no personal paths, no secrets in CI.

Hard: GPU coverage exists only on this PC, so merges stop when it is off; the lead is a serial bottleneck at `land`; each new worktree pays one configure and, for kernel tasks, one device build; oracle references need Mitsuba and pbrt-v4 on the lead's machine; each new test costs one canary patch and one extra test run in `land`.

Cost when unused: the nofma build costs `gate` build time and the probe build `full` build time only (about one extra device build each; zero run-time cost); the dev kernel-directory override is one environment check at engine start; the ABI recorder costs one branch per ABI call; `ar_build_info()` is one exported function; the work counters come from the per-stage queue counts the scheduler already reads (0002 D7) and the allocator ledger (0002 D13); the windowed viewport and oracle tiers cost nothing outside `full`.

## What lands when

When each item lands is `docs/ROADMAP.md`'s business (ruling G1).

**Must exist from the first milestone, because retrofitting is expensive:**
- Layout, presets, the one tree per checkout, `build`/`check`/`land`/`status`/`fetch` and the locks: every later task is built, tested and landed through them; work landed before them has no gate record and no lock protection (v1 hard resets).
- Build identity with the `ninja -n` check, embedded kernels and the `ASTRORAY_KERNEL_DIR` override: v1's stale-binary failures start with the first build, and every result before the check is untrustworthy.
- `deps.toml`, checksummed fetch and our Embree and OIDN release assets: once code depends on a networked or source-built route, removing it touches the whole build.
- The CPU and GPU doctest executables, with every lobe, light and sampler function pure and taking explicit uniforms (decision 14): kernel code written without that interface must be rewritten to be tested.
- The kernel resource gate on 0002's baseline, serial compile timing (B-05), work counters and the tripwire (B-16): budgets need a baseline from the first kernel, and growth found late forces splits.
- Agreement steps 17(i)-(iv): one-source divergence between CPU and GPU is cheapest to find while there is one kernel.
- ABI recording and replay, scene builders, A1, A5, D2 and the Blender smoke: every scene, benchmark and bug report is a recording, so a late format means rewriting all builders.
- The `fast` lints (0001 V2-V4, V9; 0002 D1; ruling 20's dependency check) and the gate checks V1, V5 (dependents), V6, V11: violations accumulate silently otherwise.
- Hosted CI, the posted `astroray/gate` status and branch protection: merges without the GPU gate were v1's main regression path.
- `budgets.toml` with the declared family sizes: thresholds must not move as tests are added (decision 15).
- CLAUDE.md and settings (appendix), three agent files, the `land` skill with the canary check, STATUS: the process every task follows.
- The bench history line format and a report skeleton (build times, throughput, B-1M/B-5M first frame): history is comparable only if its format is fixed from the first line.
- The `spikes-final` tag, and the measurements that reset the PROVISIONAL limits (doctest vs Catch2 compile cost, Blender headless start per process, hosted `ci` duration and toolset, nvcc pool RAM peak, edit-to-reload on real stages, research 13 spikes P1/P4/P5/P8): every budget above depends on them.

**Can be added later without redesign:** A2-A14; O1-O4 with cross-oracle agreement; the full statistical suite and lattice audit; steps 17(v)-(vi) with the null calibration; node coverage and edit round trip; the windowed viewport harness; V5 inside Blender; perf A/B and the full report; the showcase gallery; the clean-profile install test of the release zip; `ASSETS_NOTICE.md`; the release preset; B-NB; Blender 5.4 in the matrix; an oracle re-render rot check before each release.

## Verification

| decision | check | pass |
|---|---|---|
| 1, 6, 10, 21, 31 | `lint.py` in `fast` | zero findings; seeded violations in `tests/cpu/lint_cases/` (one per lint, including 0001 V2-V4 and 0002 D1) are caught |
| 2, 4, 7 | `build.ps1` version check; clean-build audit in `full` | versions match (CI: ≥ 19.44 logged); B-01 met with no cache |
| 5 | delete one output, touch one header, run `check` | `ninja -n` check fails until rebuilt; sha = HEAD in every tier; a test run with `ASTRORAY_KERNEL_DIR` set fails `check` |
| 8 | measurement task: two `check gate` runs started together, and a `fast` started during a gate | second gate waits; `fast` finishes without waiting, inside B-08; no overlap; PC stable |
| 9-13 | tier JSON summaries | durations, excluding lock wait, within B-08..B-11 over 5 consecutive runs |
| 13 | lint: `budgets.toml` ids and values = table above; grep of `docs/decisions/` for a time limit not given as a B-id | exact match; no restated time budget outside 0007 D35 |
| 13 (B-17..B-37) | engine budgets in `full`; a test kernel made to run 150 ms per launch | within limits; the 150 ms launch fails B-17 |
| 14-15 | detection-power task (research 09 spike 3): inject missing cos, wrong Jacobian, 1% albedo error, dropped transmission, flipped front face | each caught at gate counts by at least one of energy/chi-squared; adding a test beyond m fails lint |
| 16 | cross-oracle check on O1 | Mitsuba vs pbrt-v4 pass the same Z-test |
| 17 | agreement suite on the first kernels | 0002 D12 thresholds met; deliberate FMA-on in the nofma build trips (ii) |
| 17(v) | null calibration: CPU vs CPU with independent seeds, 8 vs 8, 20 repeats on three scenes | ≤ 2 failing repeats per scene |
| 17a | Blender smoke in gate | state dumps equal and analytic pass; a deliberately dropped node trips coverage in `full` |
| 18 | resource gate, counters, tripwire | an extra live array in a capped kernel raises spills or stack and fails until re-blessed, and its uncapped count shows as a `full` warning only; an extra shadow ray per bounce fails the counters; an injected 1.5x slowdown trips the tripwire |
| 19 | replay of an addon recording in the CLI | AOVs equal to the in-Blender render |
| 22-23 | milestone close | report opens offline with all plots; release carries gallery and report |
| 24-26 | first landed task, and one with a canary patch that does not break its test | PR shows `ci` and `astroray/gate`; squash commit on origin/main; task file gone; the inert canary is refused by `prepare` |
| 27-30 | P5 lane dry run (research 13) | reviewer, reading only `out/land/NNN/`, catches 3 planted defects; implementer shows `check fast` tail |

## Risks and open items

- Budgets B-01/B-02/B-05 and 0002's per-kernel limits rest on a spike that omits upsampling, textures, traversal and the VM at full size (spike A §0). Re-measure at each milestone; a broken budget leads to a split, never a silent raise.
- B-14, B-15 and B-20 depend on kernel throughput and shader sync not yet measured (spike B §4 caveat); they may force acceleration-structure caching (0007 D35 remedies).
- B-17..B-37 were collected from 0002-0005, 0007 and 0008 when rulings G2 and 29 moved every time budget here; all are PROVISIONAL, none has been measured on product code, B-28 to B-30 have no number yet, and B-34's schedule values were copied from Cycles' `render_scheduler.cpp` (task 086).
- Where astropy is installed is open: ruling 32 allows no separate virtual environment, and Blender's bundled Python does not carry it (decision 2).
- The hosted runner (task 091): `ci` ran 241-345 s on windows-2025-vs2026 with `cl` 19.51.36260.0 (the pinned local toolset is 14.44); its 14 GB disk and 4 vCPU are unmeasured against `ci` with the `bpy` wheel suite (B-11).
- Blender start-up time per process inside the 3-minute gate is unmeasured. If it is large, the gate smoke reuses one Blender process for all its checks.
- The no-FMA build differs from the shipped binary; 17(iii) covers the shipped one statistically against double only.
- Mitsuba's Windows wheel Python version and pbrt-v4's source build on this machine are unverified (research 09 spikes 7, 11). Without two witnesses, O scenes are self-consistency checks only.
- The Welch configuration's false-positive rate on this engine is uncalibrated until the null-calibration task runs.
- The machine lock does not prove the PC will not hard-reset under load (cause unknown, research 13); `fast` running unlocked beside a gate adds a little CPU load.
- Statistical power numbers in research 09 §3 are derived, not measured; the detection-power task settles gate sample counts and m.
- Release builds ship architectures other than sm_120 untested.
- The `gh` token on this PC can post statuses and merge; it must not leave the machine.
- **Owner questions:** (3) should an open-weight worker exist? (4) may a superseding ADR delete the superseded file? (Questions 1 and 2 left this record: the first-frame number is 0007's under ruling 21, and ruling G3 excludes scheduled jobs.)

## Sources

- Research: [08 build/toolchain](../research/08-build-toolchain.md) (§2, §7, §10, S7), [09 test/validation](../research/09-test-validation.md) (§3, §6, §8, §9), [13 agent process](../research/13-agent-process.md), [16 scenes and assets](../research/16-test-scenes-and-assets.md), [14 reference implementations §2.6, §4](../research/14-reference-implementations.md), [11 licensing](../research/11-licensing.md), [01 Blender boundary](../research/01-blender-boundary.md).
- Records: [0001](0001-product-shape.md) D1, D2.7, D3, D4, D5.2, D9, V1-V11; [0002](0002-kernel-architecture.md) C2, D1, D3, D4, D10-D14; [0004](0004-integrator.md) D25; [0005](0005-material-model.md) Verification 1-3, 7, M-8; [0007](0007-blender-integration.md) D10, D23, D25, D29-D35, D45, D46, V8; [0008](0008-film-outputs-denoise.md) (frame budget, Risks 9); [RULINGS](RULINGS.md).
- Spikes: [A kernel budget](../spikes/A-kernel-budget/RESULTS.md) §1, §4-7; [B Blender end-to-end](../spikes/B-blender-end-to-end/RESULTS.md) §0-4.
- v1: [toolchain-and-build](../v1-lessons/toolchain-and-build.md), [testing-and-process](../v1-lessons/testing-and-process.md) (allocation shifts), [research/testing](../v1-lessons/research/testing.md) (tile Welch test).
- Statistics: K. Pearson 1900, Philosophical Magazine 50 (chi-squared); Z. Šidák 1967, JASA 62; B. L. Welch 1947, Biometrika 34; S. Holm 1979, Scandinavian Journal of Statistics 6; F. Wilcoxon 1945, Biometrics Bulletin 1 (signed-rank test).
- Physics truths: E. Veach 1997, PhD thesis, Stanford; M. Pharr, W. Jakob, G. Humphreys 2023, *Physically Based Rendering* 4th ed.; G. B. Rybicki and A. P. Lightman 1979, *Radiative Processes in Astrophysics*; M. Born and E. Wolf 1999, *Principles of Optics* 7th ed.; J. R. Lakowicz 2006, *Principles of Fluorescence Spectroscopy* 3rd ed.; J. M. Bardeen 1973, in *Black Holes* (Les Houches).
- Reference test code: pbrt-v4 `src/pbrt/bsdfs_test.cpp`, `lights_test.cpp`; Mitsuba 3 `src/python/python/chi2.py`, `src/render/tests/test_renders.py`; Cycles `tests/python/cycles_render_tests.py` (headless pattern); Embree `tests/CMakeLists.txt` (intensity levels, not adopted).
- Tooling: GitHub self-hosted runner security guidance and hosted-runner specs (URLs in research 08 §10); Claude Code memory, skills, sub-agents, permissions and prompt-caching pages (URLs in research 13 §B).

## Assumptions about other records

- **0001:** `.arrec` record/replay generated from `astroray.h`; `ar_build_info()` (extended here with the kernel source); the CLI uses only the public ABI; release architectures in D3.4; `/MT`; kernels embedded as `RCDATA`; OptiX headers fetched at a pinned commit. This record fixes the flag spellings (`ASTRORAY_RECORD`, `ASTRORAY_KERNEL_DIR`, `info`, `render`, `bench`, `dump`). 0001's named scripts (`check_abi_header.py`, `check_vendor_seam.py`, `check_licences.py`, `check_data_manifest.py`) are sections of `tools/lint.py`, and `fetch_deps.ps1` is `fetch.ps1 deps`. V3's allowed directories follow ruling 39 (`src/device/`, `src/kernel_gpu/`, `src/denoise/dlss/`). Toolchain pins are stated only here (decision 2, ruling 31). Also 0001's: the closed dependency list (D9.1) that `deps.toml` mirrors (ruling 20), and the OIDN build and its pinned version (ruling 12).
- **0002:** owns per-kernel resource budgets and their measurement, `budget.toml`, the agreement thresholds (D12), the kernel list and the file layout under `src/kernel*`; restricted C++ with one `.cu` and one `.cpp` per stage; a launch-bounds macro that a define compiles out; module images loaded through the driver API, nvcc only (ruling 16); `Real` switchable to double. Its time rows (compile, context, idle, partition, kernel time, gate kernel part) are B-05, B-09, B-18..B-20 here.
- **0003:** owns the sampler (ruling 3): counter-based, keyed on (pixel, sample, dimension, seed), no special seed value, an "independent" mode for decorrelated frames; CPU and GPU consume identical numbers.
- **0004-0006:** pure sample/eval/pdf functions with explicit uniforms; lobes declare reciprocity; 0005 V1's lossless furnace target is A1's truth; a light-tracing pass usable by A11, splatting with 0008's pixel filter (ruling 2); a double CPU path for A14. Their time targets are B-22..B-24, B-29, B-30, B-35 and B-36 here; 0003's `specgen` threshold is in B-37.
- **0007:** addon record mode; the first-frame gate (D35, ruling 21) and its harness; presentation, whose time limits are B-13 here with V8's method; scenes S1-S3; the resolution controller and progressive batching (B-33, B-34) and its timing constants (B-37); viewport timestamps for B-14; sync and latency rows B-21, B-31, B-32; the headless sync suite for CI (D46); the extension zip carries no loose kernel modules (ruling 13).
- **0008:** owns the per-sample fp32 buffer and its resolve into fp64 moments (ruling 1), which the analytic tests read; statistics buffers, mean/variance film dumps as FITS (0008 decision 13), film and denoiser tests for the GPU tier, FITS output checked with astropy; the frame-budget formula's terms (B-25 to B-28) and the OIDN at-rest delay (B-37).

## Appendix A — critique of the current CLAUDE.md and settings.json

CLAUDE.md (50 lines) is short and free of dated directives; rules 4-8 each answer a v1 failure. Problems: (1) "Phase: planning" is state and will be false at sign-off; it belongs in STATUS.md. (2) No commands, the first thing Anthropic's guidance asks for (research 13 R1). (3) Rule 9 duplicates budgets that now live here and in `budgets.toml`. (4) "Do not run two GPU builds" and "keep documents small" are machine-checkable and move to the lock and the lint. (5) "Not against another renderer" contradicts the oracle tests; the intent is no Cycles parity. (6) "Ask the lead for a cited lookup": a subagent cannot ask, it can only report. (7) Missing: done-definition, task scope, worktree check, WIP commits, canary, no skip without task, numpy for positions.

settings.json: the `SessionStart` hook printing `git status -sb` duplicates the git status Claude Code already injects at session start, so it costs a process per session for nothing; remove it (zero hooks). Add `autoMemoryEnabled: false`. Keep the plugin choices (Superpowers off, context7 on) and the main-session 1-hour cache (the lead idles while the owner reads). Drop `subagentPromptCacheTtl` until research 13 spike P8 shows lanes idle more than 5 minutes (builds are now seconds; 1-hour writes cost more). Denies that name personal paths go in the git-ignored `settings.local.json`.

Proposed `CLAUDE.md`:
```markdown
# Astroray 2.0

Spectral path tracer for Blender and astrophysical rendering. Correct means
physics, analytic results and the OpenPBR 1.1.2 specification. Independent
spectral renderers are cross-checks; Cycles pixels are never a target.

## Commands (their output is the only evidence that something works)
- `tools/build.ps1`: configure if needed, build, print the build id.
- `tools/check.ps1 fast|gate|full`: tiers and budgets in docs/decisions/0009.
- `tools/land.ps1 prepare|merge NNN`: lead only.
Before saying "done", run `tools/check.ps1 fast` and the task's acceptance
commands and paste the last lines. A failed budget, size or lint check is yours
to fix, never to raise.

## Rules
1. Your task is `tasks/NNN-*.md`. Change only files in its Scope. If the task
   has two readings or a simpler design exists, say so before coding. Report
   unrelated problems; do not fix them.
2. In a worktree, first check `git rev-parse --show-toplevel` is that worktree.
   Commit work in progress after each acceptance step. Never `git stash`.
3. Physics, sampling, numerics: implement from the source the task names and
   put `Ref:` (paper, section or equation, reference file) in the code. Never
   invent an algorithm or a constant.
4. One source: a formula, material or sampler exists once and compiles for CPU
   and GPU. Never hand-port.
5. Fail loudly: unsupported input raises or warns visibly. No silent fallback,
   no feature merged switched off, no process-global mutable state.
6. Every new or changed test needs `tasks/NNN.canary.patch`: a deliberate
   breakage that turns it red (`land` applies it). No skip or xfail without an
   open task id. Never loosen a threshold to pass: investigate code and test.
7. Look at renders (Read the PNG) as well as numbers. Measure positions and
   shifts with numpy, never by eye.
8. Comments explain why, never ticket history.

## Where things are
- `docs/ARCHITECTURE.md`; `docs/decisions/` (accepted records); `docs/STATUS.md`
  (one page, lead edits); `tasks/` (open work only).
- `docs/v1-lessons/` is the only channel to Astroray 1.0. Never open the v1
  repository; if you need a v1 fact, name it in your report.
- `.claude/rules/*.md` load when you touch matching files.

## Environment
Windows 11, pwsh 7, RTX 5070 Ti, 8 cores / 16 threads. Builds, GPU runs and
Blender runs go through `tools/`, which hold the machine lock. Do not call
cmake, ninja, nvcc or blender directly.
```

Proposed `.claude/settings.json` (committed):
```json
{
  "$schema": "https://json.schemastore.org/claude-code-settings.json",
  "autoMemoryEnabled": false,
  "promptCacheTtl": "1h",
  "enabledPlugins": {
    "superpowers@claude-plugins-official": false,
    "context7@claude-plugins-official": true
  },
  "permissions": {
    "deny": [
      "Bash(git stash*)", "PowerShell(git stash*)",
      "Bash(git push --force*)", "PowerShell(git push --force*)",
      "Bash(git push -f*)", "PowerShell(git push -f*)"
    ]
  }
}
```
`.claude/settings.local.json` (git-ignored, this PC only) adds `"Read(//<v1 checkout>/**)"` and `"Edit(//<v1 checkout>/**)"` to `permissions.deny`.

History: docs/history/0009-review-and-reconciliation.md (review and reconciliation of this record; the Decision section above is current).
