# 020 Statistical test harness: Sidak thresholds, energy and chi-squared tests
Goal: lobe, light and sampler tests have no shared statistics -> one header gives per-test thresholds from the suite's declared family size, hashed seeds with no special value, the energy test, the pooled chi-squared test with its 2x-resolution re-run, a reproduce line on failure, and a hard stop when a family exceeds its m.
Read first: 0009 d15 (energy test, chi-squared, family size, seeds, rotated-seed rule), d10 (no p-values or random seeds in `fast`), R3; tools/budgets.toml `[family]`.
Scope: tests/common/stat.h, tests/common/stat.cpp, tests/cpu/main.cpp (`--seed`, exit-time family check), CMakeLists.txt (family sizes from budgets.toml into a generated header), tests/cpu/test_stat_harness.cpp.
Out of scope: concrete lobe or light tests (later tasks use the harness); the image-level Welch tests of 0009 d17(v) (M2).
Reference: Sidak 1967 (per-test alpha = 1 - (1 - 0.01)^(1/m)); Wichura 1988, Applied Statistics 37(3), algorithm AS241 (inverse normal); regularised incomplete gamma by series and continued fraction as described in Press et al., Numerical Recipes 3rd ed. section 6.2 (algorithm only, our own code); harness pattern of pbrt-v4 `src/pbrt/bsdfs_test.cpp` and Mitsuba 3 `src/python/python/chi2.py` (read for structure, not copied).
Do:
1. CMake reads `[family]` from tools/budgets.toml (regex over the file) into out/build/gen/families.h (`AR_FAMILY_GATE`, `AR_FAMILY_FULL`, `AR_FAMILY_ANALYTIC`).
2. `Family` objects count every statistical check executed; at program exit, a family whose count exceeds its m fails the run with "family <name> ran <n> checks, m = <m>; raising m is a reviewed budget change".
3. `seed_for(test_name)` = 64-bit FNV-1a of the name mixed with the repo constant 0x415354524159 through pbrt-v4's `MixBits`; `full` runs mix in the commit sha (passed by check.ps1 in `ASTRORAY_SEED_SHA`).
4. `energy_test(samples) -> {mean, se, z, pass}` against a truth with z from the family's per-test alpha (two-sided).
5. `chi2_test(observed[], expected[])`: pools cells with expected < 5 into one, dof = cells - 1, p from the incomplete gamma; `chi2_with_retry(gen, ires)` re-runs once at 2x ires before reporting a failure (0009 d15).
6. Every failure message prints the seed and `out/bin/<exe> -tc="<name>" --seed=<s>`; the test mains accept `--seed`.
7. tests/cpu/test_stat_harness.cpp (suite fast, deterministic only): inverse normal against tabulated quantiles (1e-9), incomplete gamma against tabulated chi-squared p-values (1e-8), pooling on a fixed histogram, Sidak alpha for m = 2000 (about 5.0e-6), a family of m = 3 fed 4 checks fails at exit (checked in a child process).
Acceptance: `pwsh tools/check.ps1 fast` passes; `out/bin/astroray_tests_cpu.exe --tier=fast -tc="stat_harness/*"` passes at least 5 cases; the over-m child test prints the family message.
Look at: none.
Canary: tasks/020.canary.patch must turn red: astroray_tests_cpu "stat_harness/family over m fails", "stat_harness/inverse normal quantiles".
Budget: none.
Depends on: 006, 018.
Size: M (about 350 lines); stop and report at 2x.
Progress:
