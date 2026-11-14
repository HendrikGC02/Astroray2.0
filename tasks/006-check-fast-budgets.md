# 006 budgets.toml and `tools/check.ps1 fast`
Goal: there is no single command for the 10 s loop and no machine copy of the budgets -> `tools/check.ps1 fast` runs lint, the incremental CPU test build and the deterministic tests within B-08, and `tools/budgets.toml` mirrors 0009 d13 exactly, checked by lint.
Read first: 0009 d9-d10 (entry points, `fast`), d13 (budget table), d15 (family sizes), d8 (`fast` takes no machine lock); tools/lib/summary.ps1; tools/lint.py.
Scope: tools/budgets.toml, tools/check.ps1, tools/build.ps1 (`-Jobs` switch), tools/lint.py (new section `budgets`), tests/cpu/lint_cases/budgets/, tools/README.md.
Out of scope: the `gate`, `full` and `ci` tiers (tasks 030, 009); build-id and sha checks (013).
Reference: none.
Do:
1. tools/budgets.toml: one `[[budget]]` table per row B-01..B-37 with `id`, `text` (the limit cell of 0009 d13 copied verbatim), `limit` and `unit` (numeric form for rows a script enforces; omit for rows that point to 0002 D10 or 0007 d35), `tier`, `provisional` (true where the basis column says PROVISIONAL). A `[family]` table: `gate_statistical = 2000` (0009 d15), `full_statistical = 2000` and `analytic_scenes = 256`, the last two marked `provisional = true` in a `[family_provisional]` table; they are reset by the detection-power task (078).
2. lint section `budgets`: parse the Markdown table in docs/decisions/0009-build-test-process.md decision 13; fail when the set of ids differs or any `text` differs from the table cell; fail when a numeric `limit` is not present in its `text`.
3. tools/check.ps1 with tier `fast` (the only tier for now; unknown tiers fail): no machine lock; tree lock on out/build; steps (a) lint, (b) `tools/build.ps1 -Target astroray_tests_cpu -NoMachineLock -Jobs 4` (`-Jobs <n>`, added to build.ps1 here, passes `-j <n>` to `cmake --build`; default unlimited), (c) `out/bin/astroray_tests_cpu.exe --tier=fast --duration=true`; fail if any single fast test exceeds B-12's fast limit (0.5 s) or the tier exceeds B-08 (10 s), both read from budgets.toml; summary through tools/lib/summary.ps1 (`PASS fast <s> s (budget 10 s, waited <w> s)`).
4. One lint case pair under tests/cpu/lint_cases/budgets/ (a toml with one changed `text`, and a clean copy).
Acceptance: `pwsh tools/check.ps1 fast` prints a last line starting `PASS fast` and writes out/check/fast.json with `pass: true`; changing B-08's `text` in budgets.toml makes it print `FAIL` naming B-08; a test made to sleep 1 s (local experiment, not committed) makes it FAIL naming the test.
Look at: none.
Canary: tasks/006.canary.patch must turn red: `tools/lint.py --self-test` (the patch skips the `text` comparison).
Budget: defines B-08's measured value; report the `fast` wall time in Progress.
Depends on: 004, 005.
Size: M (about 250 lines); stop and report at 2x.
Progress:
