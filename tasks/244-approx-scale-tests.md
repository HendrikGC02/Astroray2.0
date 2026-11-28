# 244 Tests that cannot fail: doctest::Approx on values far below 1 gets `scale(0)` or a ratio (slice-0 debt, test debt)
Goal: `light_sampler/small q is exact at 2^20 bins` (tests/cpu/test_light_sampler.cpp line 677) compares a frequency with `doctest::Approx(pmf).epsilon(2e-3)`, which keeps doctest's default scale 1, so at pmf about 1e-8 it accepts any absolute difference below 2e-3: a 5x wrong frequency passed under a canary hunk (task 175) -> that case and every other Approx check whose expected value is far below 1 compare relatively, each proved by a canary hunk that the old form let through. [lane cpu; Sonnet; review skip ok (tests only; the lead reads the list)]
Read first: BACKLOG row "`light_sampler/small q is exact at 2^20 bins` cannot fail" (175); doctest's `Approx` (third_party/doctest: `epsilon` is relative to `scale + |value|`, default scale 1); tests/common/stat.h (the family helpers: a statistical comparison belongs there, not in Approx); `rg "Approx\(" tests/cpu tests/gpu` (91 `epsilon(` uses without `scale(` today): for each, the magnitude of the expected value.
Scope: tests/cpu/test_light_sampler.cpp (the named case: compare freq / pmf with 1, or `.scale(0)`), every tests/cpu/*.cpp and tests/gpu/*.cpp Approx check whose expected value can be below 1e-2 in magnitude (list them in the PR with the expected magnitude; a check on values of order 1 stays untouched), tools/lint.py only if a cheap `recurring` rule is possible (an `Approx(` with `epsilon(` and no `scale(` in a line that names pmf, pdf, prob or freq: say whether it has false positives on today's tree; do not add it if it has), tests/cpu/lint_cases for that rule if added, docs/BACKLOG.md (delete the row).
Out of scope: changing any tolerance value (a check that turns red once it compares relatively is a finding: stop and report the case and the measured difference, never loosen); statistical redesign of a case; engine code.
Reference: none (test mechanics). doctest documentation of `Approx::epsilon` and `Approx::scale` (cite the header line in the PR).
Do:
1. The list (file, case, expected magnitude, old absolute slack, new relative slack).
2. The edits; for the named case and for the three checks with the smallest expected values, a canary hunk each that scales the engine value by 2 and turns the case red (it stayed green before: show both runs for the named case).
3. Any case that now fails on main: reported, not fixed here.
Acceptance: `pwsh tools/check.ps1 fast` and `pwsh tools/check.ps1 gate` pass; `pwsh tools/test.ps1 -Exe astroray_tests_cpu -Tier all -Case "light_sampler/*"`; the list in the PR; `pwsh tools/check.ps1 full` is not required (the named case is in the full suite: run it alone and paste its line).
Look at: none.
Canary: tasks/244.canary.patch must turn red: astroray_tests_cpu "light_sampler/small q is exact at 2^20 bins"
Budget: gate +0 s (no new case; the named case stays in `full`); kernel rows: none.
Depends on: none. Collisions: test files with 239, 243, 247 and M2's lobe tests by neighbouring lines only; tools/lint.py with 249-251 only if the rule is added.
Size: S (test edits only); stop and report above 40 touched checks (then split by directory).
Progress:
