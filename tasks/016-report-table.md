# 016 The per-render report table
Goal: diagnostics have nowhere to go -> a host `Report` class implements 0001 D2.11's keyed, deduplicated entries with levels, scopes, lifetimes, revision, deterministic snapshots and the 512-entry cap, so later tasks raise codes into it and sessions expose it through the polling functions.
Read first: 0001 D2.11 (schema, levels, deduplication, scope and lifetime, polling, codes table), V15; 0007 d9 (generation outcomes).
Scope: src/host/report.{h,cpp}, src/host/report_codes.def, tests/cpu/test_report.cpp.
Out of scope: the ABI polling functions (wired to sessions in task 104); raising real codes (each later task); host-side Python diagnostics (addon).
Reference: none.
Do:
1. src/host/report_codes.def: X-macro `AR_REPORT_CODE(NAME, LEVEL, SCOPE)` with every row of D2.11's table in table order; a `static_assert` per row that `AR_REPORT_<NAME>` from astroray.h equals its 1-based position, so the def and the header cannot drift.
2. `Report`: key = (code, subject, detail); `raise(code, subject, detail, count, value[2], generation)` adds a new entry or adds `count`, updates `last_generation` and `value`; level and scope come from the def; `revision` (u64) increments on every add, remove or change and on nothing else; at most 512 distinct keys (constant from D2.11, PROVISIONAL), further distinct keys increment `dropped` while existing keys keep counting.
3. Lifetimes: `on_generation_active(gen, replaced_or_removed_subjects, reset)` removes SCENE entries of those subjects (all SCENE entries when `reset`) and removes `GENERATION_REJECTED`; `on_film_reset()` removes FILM entries; SESSION entries stay.
4. `snapshot()` stores entries ordered by level (descending), code, subject, detail; `info()` and `entry(i)` read the latest snapshot (`AR_ERR_INVALID_ARGUMENT` past the end). The class is used by one thread at a time per session; raising from worker threads goes through a mutex inside Report (document why: workers raise SCENE entries during generation builds).
5. tests/cpu/test_report.cpp (suite fast), the V15 parts that need no rendering: two raises of one key give one entry with count 2; three entries of two node types for one material give two entries; revision changes exactly when the table changes; two snapshots of an unchanged table compare equal; an active generation replacing a subject removes its SCENE entries; a film reset removes FILM entries; 513 distinct keys give 512 entries and `dropped` = 1.
Acceptance: `pwsh tools/check.ps1 fast` passes with the new tests counted; `out/bin/astroray_tests_cpu.exe --tier=fast -tc="report/*"` lists at least 7 passing cases.
Look at: none.
Canary: tasks/016.canary.patch must turn red: astroray_tests_cpu "report/overflow drops the 513th key", "report/revision only on change".
Budget: none.
Depends on: 013.
Size: S (about 250 lines); stop and report at 2x.
Progress:
