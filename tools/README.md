# tools/

Scripts print at most 20 lines, log to out/check/, exit non-zero on failure (0009 d8, d9).

- build.ps1: configure if needed, build, prove no work is left; machine + tree lock, -NoMachineLock for CPU-only callers.
- lib/lock.ps1: Enter/Exit-ArMachineLock (Global\astroray2, nested via ASTRORAY_LOCK_PID), Enter/Exit-ArTreeLock <dir>.
- lib/summary.ps1: Start-ArSummary, Add-ArStep, Complete-ArSummary: 20-line summary plus out/check/<tier>.log and .json.
- lib/proc.ps1: Start-ArProcess (writes out/pids/<name>.pid), Stop-ArProcess -Id. No kill by name.
- lib/python.ps1: Get-ArPython: Blender 5.3 bundled python (python on CI); callers pass -I.
- lib/blender.ps1: Invoke-ArBlender -Script -Args -TimeoutS: isolated profile, machine lock, returns exit code and log.
- lint.py: SPDX, NVIDIA, ASCII ps1, process caps, skip/xfail, task-file format; `--self-test` runs tests/cpu/lint_cases/.
- budgets.toml: machine copy of 0009 d13 (B-01..B-37) and the d15 family sizes; lint section `budgets` checks it.
- check.ps1 fast: lint, `build.ps1 -Target astroray_tests_cpu -NoMachineLock -Jobs 4`, fast tests; fails on B-08 (tier) or B-12 (one test). No machine lock. Other tiers come later. build.ps1 -Jobs n passes -j n.
- report.py: offline out/report/index.html (tier durations vs B-08..B-11, build times, empty sections for later plots) from bench/history/*.jsonl and out/check/*.json; `--selftest`. History line keys: sha, machine, driver, cuda, scene, metric, value, unit, n, spread, date; lint section `history` checks them. bench/scenes.toml lists B scenes.
- deps/build_embree.ps1: recorded Embree 4.4.1 build (static, no TBB, /MT) into out/deps/embree-4.4.1-astroray-win64-mt.zip; machine lock; never called by build or check.
- fetch.ps1 deps: downloads the fetched-* entries of third_party/deps.toml into third_party/_cache/<name>-<version>/, verifies SHA-256, extracts; thirdparty.py renders THIRD_PARTY.md from it (lint section `deps` checks both and the D9.1 closed list). cmake/deps.cmake resolves the cache paths, no network.
- fetch.ps1 assets [-Name n]: downloads assets/manifest.toml entries into assets/_cache/<name>/, verifies SHA-256 and a non-empty spdx; names only, no crawl. Lint section `assets`: tests/data + scenes <= 50 MB, files <= 2 MB, committed assets licensed CC0/CC-BY/MIT/Apache-2.0, PNGs <= 2 MB and <= 1600 px (0009 d21).
