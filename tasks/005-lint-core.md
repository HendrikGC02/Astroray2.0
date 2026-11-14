# 005 tools/lint.py: core sections and seeded lint cases
Goal: nothing checks licences, NVIDIA files or process caps -> `tools/lint.py` checks SPDX headers, NVIDIA material, ASCII scripts, the anti-accretion caps, skip/xfail use and the task-file format, each section proven by seeded violations.
Read first: 0009 d10 (what `fast` lints), d28 (task format), d31 (caps); 0001 D7.1 and V4 (licence and NVIDIA rules); tasks/README.md (field order).
Scope: tools/lint.py, tests/cpu/lint_cases/, tools/README.md (one line).
Out of scope: kernel dialect and vendor seam (018), dependencies (008), budgets (006), ABI grammar (012, in the generator), data manifest (022), asset rules (088).
Reference: none.
Do:
1. tools/lint.py: Python 3.13 stdlib only, run as `<Get-ArPython> tools/lint.py [--self-test] [--section s]`; one function per section; findings print `path:line: [section] message`; exit 1 on any finding; the repository run skips tests/cpu/lint_cases/ (seeded violations) and `--self-test` runs each case with its own directory as the root.
2. Section `spdx`: every tracked .h .hpp .c .cpp .cu .py .ps1 .cmake and CMakeLists.txt has `SPDX-License-Identifier: <id>` in its first 5 lines; id is Apache-2.0 under include/ src/ cli/ tools/ tests/ data/ scenes/ bench/, GPL-3.0-or-later under blender/ except generated `_abi.py` and `_ops.py` (Apache-2.0); third_party/ is exempt.
3. Section `nvidia`: no tracked file named optix*.h, nvsdk_ngx*, nvrtc*, *.dll, *.lib, *.cubin, *.ptx, and no "NVIDIA Corporation" licence header outside docs/ (0001 V4, repository part).
4. Section `ascii-ps1`: every .ps1 is pure ASCII.
5. Section `caps` (0009 d31): CLAUDE.md <= 60 lines; .claude/rules/*.md <= 40; .claude/agents/*.md <= 60 and at most 3 files; .claude/skills/*/SKILL.md <= 80 and at most 2; docs/STATUS.md <= 50; tasks/NNN-*.md <= 60; tasks/README.md <= 40; tools/README.md <= 30; docs/v1-lessons/**/*.md <= 150; no `hooks` key in .claude/settings.json; no AGENTS.md, .codex/ or .opencode/; no dates (YYYY-MM-DD) or model ids (`claude-<word>-<digit>`) in CLAUDE.md, rules or agent files. The v1-lessons files already over 150 lines on this task's base commit go into a frozen list `V1_LESSONS_OVER_CAP` with their current line counts: a listed file may not grow and no other file may exceed the cap (the roadmap cites feature-inventory.md by line number, so nothing is split here).
6. Section `skip`: a doctest `skip`, `may_fail`, `should_fail` or Python `unittest.skip`/`expectedFailure` must sit on a line naming `task NNN`, and tasks/NNN-*.md must exist.
7. Section `taskfile`: each tasks/NNN-*.md starts with `# NNN ` matching its file name and has, in this order, lines starting `Goal:`, `Read first:`, `Scope:`, `Out of scope:`, `Reference:`, `Do:`, `Acceptance:`, `Look at:`, `Canary:`, `Budget:`, `Depends on:`, `Size:`, `Progress:`; at most 10 lines after `Progress:`.
8. tests/cpu/lint_cases/<section>/<case>/: a minimal file tree plus `expected.txt` (the exact finding lines); `--self-test` runs every section on every case and fails unless output equals expected; each section has a clean case with no finding.
Acceptance: `<python> tools/lint.py` exits 0 on the repository; `<python> tools/lint.py --self-test` prints `self-test: <n> cases passed` with at least 2 cases per section; removing the SPDX line of any one source file makes `tools/lint.py` exit 1 naming that file.
Look at: none.
Canary: tasks/005.canary.patch must turn red: `tools/lint.py --self-test` (the patch disables the agent-file count in `caps`).
Budget: lint run time reported in Progress; it must leave room inside B-08.
Depends on: 002, 004.
Size: M (about 400 lines Python plus cases); stop and report at 2x.
Progress:
