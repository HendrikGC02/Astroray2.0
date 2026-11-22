# Astroray 2.0

Spectral path tracer for Blender and astrophysical rendering. Correct means
physics, analytic results and the OpenPBR specification. Independent spectral
renderers are cross-checks; Cycles pixels are never a target.

The project is in its build phase. Work comes from `tasks/` (order and
dependencies in `tasks/README.md`); the current state is in `docs/STATUS.md`. Do
not open the old Astroray repository.

## Commands (their output is the only evidence that something works)
- `tools/build.ps1`: configure if needed, build, print the build id.
- `tools/check.ps1 fast|gate|full`: tiers and budgets in docs/decisions/0009.
- `tools/land.ps1 prepare|merge NNN`: lead only.
Scripts under `tools/` appear task by task. Until `tools/check.ps1` exists, the
task's own acceptance commands are the evidence.
Before saying "done", run `tools/check.ps1 fast` and the task's acceptance
commands and paste the last lines. A failed budget, size or lint check is yours
to fix, never to raise.

## Rules
1. Your task is `tasks/NNN-*.md`. Change only files in its Scope, plus what
   its change forces: its own budget and counter rows, registration lists, and
   existing tests it breaks; name each in your report. If the task has two
   readings or a simpler design exists, say so before coding. Report unrelated
   problems; do not fix them.
2. In a worktree, first check `git rev-parse --show-toplevel` is that worktree.
   Commit work in progress after each acceptance step. Never `git stash`.
3. Physics, sampling, numerics: implement from the source the task names and
   put `Ref:` (paper, section or equation, reference file) in the code. Never
   invent an algorithm or a constant.
4. One source: a formula, material or sampler exists once and compiles for CPU
   and GPU. Never hand-port.
5. Fail loudly: unsupported input raises or warns visibly. No silent fallback,
   no feature merged switched off, no process-global mutable state.
6. Every new or changed test needs `tasks/NNN.canary.patch`: a deliberate
   breakage that turns it red (`land` applies it). No skip or xfail without an
   open task id. Never loosen a threshold to pass: investigate code and test.
7. Look at renders (Read the PNG) as well as numbers. Measure positions and
   shifts with numpy, never by eye.
8. Comments explain why, never ticket history.

## Where things are
- `docs/ARCHITECTURE.md` (read first); `docs/decisions/` (the records;
  precedence: 0000 owner decisions, then RULINGS.md, SPINE.md, the records);
  `docs/ROADMAP.md` (milestones); `docs/STATUS.md` (one page, lead edits);
  `tasks/` (open work only).
- `docs/v1-lessons/` is the only channel to Astroray 1.0. If you need a v1
  fact, name it in your report; the lead looks it up.
- `.claude/rules/*.md`, if present, load when you touch matching files.

## Environment
Windows 11, pwsh 7, RTX 5070 Ti, 8 cores / 16 threads. Builds, GPU runs and
Blender runs go through `tools/`, which hold the machine lock. Do not call
cmake, ninja, nvcc or blender directly.
