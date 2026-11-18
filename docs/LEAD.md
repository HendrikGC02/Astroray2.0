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

## Handover (2026-11-16, lead session paused by the owner)
State for the next lead. Agent ids do not carry across sessions; a task still open goes to a new
implementer on its existing branch and worktree, briefed from this section and its task file.
- Landed this stretch: 116 (#63), 117 (#64), 082 (#65), 074 (#66). Nothing is in flight. Rulings 72-75 added (72 is an owner decision).
- Open M1a tasks (26, `ls tasks`): 075 076 077 078 083 084 085 086 087 089 090 091-097 099 100 106 107
  108 109 110 111. Critical renderer chain: 075 (Opus) -> 106 -> 076; Blender chain: 083 -> 084 ...
- Ready now: exactly 075, 078 and 083 (checked against Depends on). 075 (Opus) first: area lamps are
  too dark by p_l/(p_l+p_b) until it lands, because BSDF rays cannot hit them yet; 078 (Opus); 083 (Sonnet). At most two implementers at once.
- Follow-ups not yet in a task file: 074's program-emission NEE test is position-independent
  (swapped barycentrics or vertex order would pass); lamp temperature is Cycles' RGB fold, the
  AR_SPECTRUM_BLACKBODY option is open; PASS_UNSUPPORTED entries persist for a session's life.
- shade_surface compiles in about 4.6 s of R72's 5.0 s; k_shade_background is 62 regs (accepted).
- After M1a (task 100): the owner agreed to a Fable planning session for M1b, an Opus review of
  that plan, and a fresh coordinator session.
- Landing notes learned the hard way: a Canary line names C++ cases after the executable
  (`astroray_tests_cpu "suite/case"`) and Python cases after the file; land.ps1 cleanup fails with
  "Permission denied" if any shell's cwd is inside the worktree (remove the directory afterwards);
  a lead commit on local main rides into a landing's squash only if the task branch was rebased on
  it; after each merge, `git rebase origin/main` on main (never reset away a local-only commit) and
  push main when it still carries lead commits.
- land.ps1 runs Canary commands verbatim (every required argument, e.g. `--device cpu`), and it does not
  rebuild out/scenes: after main changes the ABI, a branch's recordings are stale and analytic/integrator
  canaries fail "without the patch". Rerun `tools/scenes.py` in the worktree (through tools/lib/python.ps1)
  and land again. Keep every shell's cwd out of a worktree that is about to land.
- Any change under tests/ (including a re-blessed tests/budgets/counters.toml) needs a canary patch.
- If hosted `ci` never starts for a landing PR ("ci is not success" with no run listed), close and reopen
  the PR to fire pull_request again, wait for it, then `tools/land.ps1 merge NNN`.
- A prepare that passes and then fails (ci red, refused merge) leaves a "(land)" commit on the branch that
  deletes the task file; `git revert` it in the worktree before the implementer continues or you re-land.
- A task that adds a tools/lib switch its own canary needs (085's -Gui) cannot prepare with main's
  land.ps1; run the worktree's tools/land.ps1 prepare, then open the PR and merge with main's.
- Stale worktree directories with no git registration: .claude/worktrees/agent-a3a823bde1b1e2a0c,
  agent-a61c36ff96e523f48, agent-a631435169fa2c847 (safe to delete after a look).
