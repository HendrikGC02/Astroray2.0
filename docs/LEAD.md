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
