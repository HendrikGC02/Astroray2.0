# Lead runbook

How the lead (coordinator) session runs the build phase. Implementers follow CLAUDE.md; this page
is the lead's half. Project state lives in docs/STATUS.md, docs/decisions/RULINGS.md and tasks/.

## Loop per task
1. Ready: every task in "Depends on" has landed (its file is gone from tasks/).
2. Dispatch one `implementer` agent per task in its own worktree (at most two at once, 0009 d27).
   The brief names: branch `task/NNN-slug`, an early first commit (a worktree with no change is
   removed when the agent stops), the env lines below, contracts already on main that the task must
   reuse, the canary rule (`git diff -- src` only, never a task file, `git apply --check` first),
   and "if a change contradicts a record or weakens an existing test, stop and ask; never choose".
3. Rule on questions in the task file (Scope line, Progress line) or in RULINGS.md, then reply to
   the same agent so it keeps its context.
4. Review: `tools/lead/review-bundle.sh NNN task/NNN-slug`, then a `reviewer` agent (Opus) on
   out/land/NNN for kernel, physics, sampling, ABI and process diffs (O31); host-only plumbing may
   skip. Send must-fix findings back to the implementer; minor ones too when cheap.
5. Land: `tools/lead/land-pr.sh NNN PASS|SKIP "title"` (run in the background; it holds the
   machine lock through land.ps1). Do not commit to main while a landing runs: main moving forces
   a re-prepare. If cleanup fails because the agent still holds its worktree lock, stop the agent
   and remove the worktree and branch by hand. A task with a `Look at:` line refuses to merge
   until the lead has viewed that image (Read the PNG) and posted a PR comment starting
   `evidence:` that says what it shows; then run `tools/land.ps1 merge NNN`.
6. Notes that a later task must honour go into that task's file (a Do item or Scope addition);
   unowned gaps go into STATUS.

## Delegation
- Sonnet implements by default: film, readback, FITS, scene builders, Blender sync, measurements.
- Opus implements rendering physics (071-078), 102 and any task that edits records or shared
  contracts; Opus reviews as in step 4. Haiku or `digest` agents read long logs.
- One agent per task; follow-ups go to the same agent, a new agent only when its context is spent.

## Environment for agents and scripts
Agent shells do not inherit the user environment. Each pwsh starts with:
```
$env:ASTRORAY_DEPS_CACHE = 'C:\Astroray2.0\third_party\_cache'
$env:ASTRORAY_BLENDER = [Environment]::GetEnvironmentVariable('ASTRORAY_BLENDER','User')
```
Worktrees share the dependency cache through that variable; never link a worktree to the cache.
The first `check fast` after a widely included header changes can exceed B-08 from the rebuild
alone; rerun once before investigating.

## Never
`git stash`, force push, opening the Astroray 1.0 repository, calling cmake/ninja/nvcc/blender
directly, or personal names, emails and paths in commits, PRs or external requests.

## Handover (2026-11-23, M1a built)
State for the next lead. Agent ids do not carry across sessions; a task still open goes to a new
implementer on its existing branch and worktree, briefed from this section and its task file.
- Landed since 2026-11-19: 133 (#107), 135 (#108), 100 (#109), 134 (#110). Rulings 84, 85. History lines:
  bench/history/2026.jsonl. Gallery and report: `tools/gallery.py --milestone M1a`, `tools/report.py` (outputs
  under out/, not tracked). Retrospective: docs/retro/M1a-retrospective.md (Fable).
- M1a is built; the close waits on the owner decisions in STATUS (B-19 idle and B-5M, B-20, B-31, M-1). Next: the
  Fable planning session (M1b plan and process changes, from the retrospective), an Opus critique of that plan,
  then a fresh coordinator. Open task: 136 (B-5M bench hang, unrelated to 134; blocks B-5M rows of B-14/B-19).
- Stale directories with no git registration: .claude/worktrees/agent-a934dd254fe6d8f2d,
  agent-a3a823bde1b1e2a0c, agent-a61c36ff96e523f48, agent-a631435169fa2c847 (delete after a look). The file
  out/tmp/blend_cornell_area.png under the C: drive root is a stray from 090: the owner removes it.
- Landing lessons (kept until land.ps1 absorbs them, retrospective R3):
  - Canary line: `tasks/NNN.canary.patch must turn red: astroray_tests_cpu "suite/case", path/test.py "Class.method"`;
    every `*.py` token is run as a test, and a .py token without quoted names after it is refused.
  - land.ps1 cleanup fails while an agent holds its worktree lock or any shell's cwd is inside it: after the merge
    run `git worktree remove -f -f <dir>` and `git branch -D task/NNN-*`. Cleanup deletes the worktree's out/:
    copy the PR body and evidence out first. land-pr.sh reuses an open PR, so open it with the real body (and the
    `evidence:` comment for a `Look at:` task) before running it.
  - A failed prepare leaves a "(land)" commit; land-pr.sh reverts it. `check fast` over B-08 right after a rebuild
    fails a prepare: rerun the landing.
  - land.ps1 does not rebuild out/scenes: after an ABI or recording change rerun `tools/scenes.py` in the worktree.
  - A leftover Blender can hold out/bin/astroray_core.dll (LNK1168): check before landing.
  - Lead commits on main: push them before a landing starts, never during one.
  - Start landings with run_in_background only. If hosted ci never starts, close and reopen the PR.
  - A task that adds a tools/lib switch its own canary needs: prepare with the worktree's land.ps1.
  - Auto mode refused a landing that combined REVIEW SKIP with the lead's own `evidence:` comment; run the reviewer.
