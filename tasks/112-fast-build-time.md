# 112 Fast-tier rebuild time: precompiled test header
Goal: after a merge that touches shared kernel headers, `check fast` rebuilds most test TUs and runs 9-18 s against B-08's 10 s -> the test executable builds with a precompiled header for doctest and the standard headers, so a post-merge `check fast` stays under B-08 with margin.
Read first: 0009 d10 (`fast`), d13 rows B-02, B-08; cmake/sources.cmake; CMakeLists.txt (astroray_tests_cpu); tools/check.ps1 (fast step uses -Jobs 4).
Scope: CMakeLists.txt (astroray_tests_cpu only), cmake/sources.cmake, tests/cpu/pch.h (new), tools/README.md.
Out of scope: changing B-08 or -Jobs; kernel or test sources; the GPU targets.
Reference: CMake `target_precompile_headers` documentation; doctest docs "Build time" (DOCTEST_CONFIG_SUPER_FAST_ASSERTS is not used: it changes assertion behaviour).
Do:
1. tests/cpu/pch.h: doctest.h plus the standard headers the test files already include (cmath, cstdint, vector, string, array, algorithm, cstring); SPDX line.
2. `target_precompile_headers(astroray_tests_cpu PRIVATE tests/cpu/pch.h)`; the generated f64 wrapper TUs must use it too (they compile with AR_REAL_DOUBLE: the PCH must not depend on Real, so it includes no kernel header).
3. Measure on a clean tree: touch src/kernel/core/real.h (forces every kernel-including test TU), then time `tools/check.ps1 fast`; report before and after in Progress (three runs each).
Acceptance: `pwsh tools/check.ps1 fast` passes; after touching src/kernel/core/real.h the next `check fast` prints `PASS fast` under 7 s (report the three timings); `ninja -n` reports no work after a normal build (build.ps1 already checks it).
Look at: none.
Canary: none: no test added or changed.
Budget: B-08 headroom is the point; report it.
Depends on: 006.
Size: S (about 40 lines); stop and report at 2x.
Progress:
