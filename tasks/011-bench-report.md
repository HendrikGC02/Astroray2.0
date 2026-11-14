# 011 Bench history format and report skeleton
Goal: benchmark history has no fixed format and there is no report -> the history line format is fixed and lint-checked from the first line, and `tools/report.py` writes an offline HTML report with build-time and tier-duration plots and empty sections for the plots M1a fills.
Read first: 0009 d22 (report contents, history line), d23 (gallery, for later), d29 (history committed by the lead only); tools/lib/summary.ps1 (JSON summary format).
Scope: bench/scenes.toml, tools/report.py, tools/lint.py (section `history`), tests/cpu/lint_cases/history/, tools/README.md. bench/history/ is created by the first committed history line (lead only).
Out of scope: tools/bench.ps1 and `astroray_cli bench` (069, 093); the gallery (100).
Reference: none.
Do:
1. History line (one JSON object per line in bench/history/<year>.jsonl): keys `sha`, `machine`, `driver`, `cuda`, `scene`, `metric`, `value`, `unit`, `n`, `spread`, `date` (ISO 8601 UTC); `n` integer >= 1; `spread` a number in the same unit (half the min-max range, or 0 for deterministic counters).
2. lint section `history`: every line of every bench/history/*.jsonl parses, has exactly these keys with the right types, and appears in non-decreasing `date` order; a lint case pair under tests/cpu/lint_cases/history/.
3. bench/scenes.toml: `[[scene]]` entries with `name`, `recording` (path under out/scenes/), `class` (B) and `sets` (milestones in the set); empty for now (B-1M and B-5M are added by task 089).
4. tools/report.py (stdlib + NumPy, no network, no external JS): reads bench/history/*.jsonl and out/check/*.json; writes out/report/index.html with inline SVG plots: (a) tier durations per check run with the B-08..B-11 budget lines from budgets.toml, (b) build times per edit class when present (metric names `build.clean`, `build.incr_header`, `build.incr_stage`, `build.host_cpp`), (c) placeholders titled throughput, work counters, first frame vs Cycles, viewport rate, kernel resources, heat maps, renders, each printing "no data yet" until lines with those metric prefixes exist. A helper embeds a PNG as base64 for later sections.
5. `report.py --selftest` writes a report from synthetic in-memory lines into out/report/selftest.html and checks the SVG contains one polyline per series.
Acceptance: `<python> tools/report.py --selftest` prints `selftest ok`; `<python> tools/report.py` writes out/report/index.html that opens offline and shows the tier-duration plot from the existing out/check/fast.json; a history line missing `unit` makes `tools/lint.py` fail naming the file and line.
Look at: out/evidence/011/report.png (a screenshot of the opened report).
Canary: tasks/011.canary.patch must turn red: `tools/lint.py --self-test` (the patch drops the key-set check).
Budget: none.
Depends on: 006.
Size: S (about 250 lines); stop and report at 2x.
Progress:
