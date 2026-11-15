# tools/

Scripts print at most 20 lines, log to out/check/, exit non-zero on failure (0009 d8, d9).

- build.ps1: configure if needed, build, prove no work is left; machine + tree lock, -NoMachineLock for CPU-only callers.
- lib/lock.ps1: Enter/Exit-ArMachineLock (Global\astroray2, nested via ASTRORAY_LOCK_PID), Enter/Exit-ArTreeLock <dir>.
- lib/summary.ps1: Start-ArSummary, Add-ArStep, Complete-ArSummary: 20-line summary plus out/check/<tier>.log and .json.
- lib/proc.ps1: Start-ArProcess (writes out/pids/<name>.pid), Stop-ArProcess -Id. No kill by name.
- lib/python.ps1: Get-ArPython: Blender 5.3 bundled python (python on CI); callers pass -I.
- lib/blender.ps1: Invoke-ArBlender -Script -Args -TimeoutS: isolated profile, machine lock, returns exit code and log.
- lint.py: SPDX, NVIDIA, ASCII ps1, process caps, skip/xfail, task-file format; `--self-test` runs tests/cpu/lint_cases/.
