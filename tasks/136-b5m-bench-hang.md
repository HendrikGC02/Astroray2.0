# 136 B-5M bench hangs before its first pass (generation 1 never activates)
Goal: task 134 found that `astroray_cli bench out/scenes/b5m.arrec --size 1920x1080 --device cuda` often never completes a pass and ends "not converged after 10 minutes". It hangs at 1a7e364, 8b86bfd and 13edb94 (twice), ran at 8af25af (twice), 81553dd and once at 13edb94, and still hangs after `tools/scenes.py --force`. The last log line is "light table: 2 lights ... on 3 threads". "traversal structures of generation 1: 2001 built" appears only when the timeout tears the scene down, and in the two runs that rendered it took 110-124 s, against about 0.5 s at teardown. The process uses about 1.4 cores throughout. B-5M is a bench scene in every standing gate (B-14, B-19, V7a) -> find why activation of B-5M's generation 1 stalls or takes minutes, fix it at the cause, and make the CLI bench report the session and generation state when it times out instead of spinning.
Read first: 0007 d9-d14 (generations, activation, retirement); RULINGS 69, 70, 81; tasks 124 and 125's PR bodies (`gh pr view 97`, `gh pr view 96`: they touched the scene workers in this window); src/host/scene/ (generation activation, traversal-structure builds, worker pool); src/cli/ bench command; 134's PR body (hang section).
Scope: src/host/scene/ and src/host/ files the cause lies in (name each in the PR); src/cli/ (bench: a PAUSED or not-activating session fails loudly with its state, CLAUDE.md rule 5); tests/cpu or tests/gpu (a test that turns red on the cause).
Out of scope: the wavefront scheduler (134); Blender-side code unless the same cause reaches F12 (then say so).
Reference: 0007 d9, d12 as written; ruling 70.
Do:
1. Reproduce: run the B-5M bench 3-5 times at main through tools/ (machine lock, nothing else on the GPU). Log per stage of generation 1's activation: mesh ingest, GAS builds (count, time), TLAS, light table, upload. Note where time goes in a run that renders and where a hanging run stops (a thread dump of the stuck process if needed).
2. State the mechanism: for example a worker-pool deadlock or starvation (3 threads, 2001 builds), a lock-order wait, an O(n^2) in the build queue, or a wait on a condition that is never signalled. Name the commit that introduced it if the bisect is cheap: 8af25af runs, 1a7e364 hangs.
3. Fix at the cause. Generation 1 of B-5M activates in seconds, not minutes, on every run.
4. CLI: when the bench times out or the session pauses, print the context's committed, pending and active generation and the session state, and exit non-zero.
Acceptance: `pwsh tools/check.ps1 gate` passes; 5 consecutive B-5M benches complete, with activation times pasted; B-19 and B-14 on B-5M measured once after the fix, with 123's method (paste the rows; compare with 134's B-1M rows).
Look at: none.
Canary: tasks/136.canary.patch must turn red: astroray_tests_cpu "scene/generation 1 of a 2001-geometry scene activates" (rename to the case you write; keep this format).
Budget: under 2 s of gate time for the new test.
Depends on: 134.
Size: S-M (about 150 lines); stop and report at 2x, or after Do 2 if the cause lies outside Scope.
Progress:
