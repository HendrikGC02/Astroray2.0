# 004 Script library: locks, summaries, processes, Python and the Blender launcher
Goal: build.ps1 runs unprotected and each script would reinvent output and process handling -> one library gives every tool the machine mutex, the per-tree lock, PID-only process control, the 20-line summary with its JSON, Blender's bundled Python and the isolated Blender launcher.
Read first: 0009 d8 (locks), d9 (output and JSON summary), d2 (Python), d17a (launcher), R32; tools/build.ps1.
Scope: tools/lib/*.ps1, tools/build.ps1, tools/README.md (new, <= 30 lines), tests/blender/print_version.py.
Out of scope: check.ps1 (task 006); measuring lock behaviour under load (091).
Reference: none.
Do:
1. tools/lib/lock.ps1: `Enter-ArMachineLock` takes the named mutex `Global\astroray2` and returns the seconds waited; if `$env:ASTRORAY_LOCK_PID` names a live process the call is nested and returns 0 without locking; the outermost caller sets ASTRORAY_LOCK_PID to its own PID and clears it on exit. `Enter-ArTreeLock <dir>` opens `<dir>/.tree.lock` with FileShare None, retrying, and returns seconds waited. Matching Exit functions, always released in `finally`.
2. tools/lib/summary.ps1: collects steps (name, seconds, pass); prints at most 20 lines; final line `PASS <tier> <s> s (budget <b> s, waited <w> s)` or `FAIL ...`; writes out/check/<tier>.log and out/check/<tier>.json = {tier, pass, seconds, waited, budget, sha, steps[]}. Tier time excludes lock wait.
3. tools/lib/proc.ps1: `Start-ArProcess` starts a process and writes its PID to out/pids/<name>.pid; `Stop-ArProcess -Id <pid>`. No tool kills by name or command line.
4. tools/lib/python.ps1: `Get-ArPython` returns the command for `$env:ASTRORAY_BLENDER\5.3\python\bin\python.exe -I`, or `python -I` when `$env:CI -eq 'true'`; fails with a clear message if the file is missing.
5. tools/lib/blender.ps1: `Invoke-ArBlender -Script <py> [-Args <list>] [-TimeoutS <n>]`: takes the machine lock; sets BLENDER_USER_CONFIG, _SCRIPTS, _EXTENSIONS and _DATAFILES under out/blender_home/<run-id>; runs `$env:ASTRORAY_BLENDER\blender.exe --background --factory-startup --python-exit-code 1 --python <script> -- <args>` through Start-ArProcess; on timeout kills by PID; returns the exit code and the log path.
6. build.ps1 takes the machine lock and the tree lock on out/build (switch `-NoMachineLock` for CPU-only callers, 0009 d8) and reports both waits in its summary.
7. tools/README.md: one line per script and lib file.
Acceptance: two `pwsh tools/build.ps1` started at once with Start-Process both end PASS and one summary shows `waited` > 0; `pwsh -c ". tools/lib/blender.ps1; Invoke-ArBlender -Script tests/blender/print_version.py"` prints `5.3.0` and exits 0; `pwsh -c "Select-String -Path tools/*.ps1,tools/lib/*.ps1 -Pattern 'Stop-Process -Name','taskkill /IM'"` prints nothing.
Look at: none.
Canary: none: no compiled or Python test added (exercised by acceptance commands here and by task 091's measurement).
Budget: lock wait reported separately; no tier time change.
Depends on: 003.
Size: M (about 300 lines of PowerShell); stop and report at 2x.
Progress:
