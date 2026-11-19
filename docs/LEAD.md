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

## Handover (2026-11-19, overnight lead session)
State for the next lead. Agent ids do not carry across sessions; a task still open goes to a new
implementer on its existing branch and worktree, briefed from this section and its task file.
- Landed overnight: 083 (#67), 075 (#68), 078 (#69), 084 (#70), 106 (#71), 110 (#72), 076 (#73), 089 (#74),
  107 (#75), 085 (#76), 090 (#77), 093 (#78), 077 (#79), 099 (#80), 118 (#81), 121 (#82), 086 (#83), 111 (#84),
  109 (#85), 119 (#86), 097 (#87), 087 (#88), 108 (#89), 092 (#90), 120 (#91), 094 (#92), 122 (#93), 123 (#94), 095 (#95), 125 (#96), 124 (#97), 128 (#98), 126 (#99), 096 (#100), 129 (#101), 091 (#102), 130 (#103). Rulings 76-83 added. 120's dropped graph attempt is kept on the local branch keep/120-graphs-B.
- In flight (branches task/NNN-*): 131 (check full red at test_images: cause c50168f, render_final.cache_dir;
  fix and B-10), 132 (GPU gate hang), 127 (V7a in the process, owner decision 35 / ruling 83; continues on its
  branch). Not started: 100 (M1a close; needs 127 and the B-31/B-19/B-20 decisions).
- Wavefront after 120: B-1M 14.7 ms, B-5M 16.9 ms per 1080p pass (from 17.0 / 28.8); B-19 still fails (idle
  ~18-20 %, ~1.7 ms of it between passes); B-17 passes; B-20 2.00x / 1.63x Cycles. CUDA graphs measured and
  dropped (OptiX refuses stream capture); the material sort is a net loss on single-program scenes (ruling 80).
- M-1 probe (092): stack 480 B fits; spills 130/376 B over 128/192 -> D10 ladder, first M1b task (STATUS).
- R = 2 GiB (097, P5 growth reading) in 0007 d14 and kCudaReserveBytes.
- Owner questions raised: exact JPEG parity (ruling 78). The file out/tmp/blend_cornell_area.png under the C: drive
  root is a stray file from 090 that a safety check kept the lead from deleting (drive root): the owner removes it.
- After M1a (task 100): the owner agreed to a Fable planning session for M1b, an Opus review of
  that plan, and a fresh coordinator session.
- Landing notes learned the hard way: a Canary line names C++ cases after the executable
  (`astroray_tests_cpu "suite/case"`) and Python cases after the file; land.ps1 cleanup fails with
  "Permission denied" if any shell's cwd is inside the worktree or the implementer agent still runs (stop it, remove
  the directory); a lead commit on local main rides into a landing's squash only if the task branch was rebased on
  it; after each merge, `git rebase origin/main` on main (never reset away a local-only commit) and
  push main when it still carries lead commits.
- land.ps1 runs Canary commands verbatim (every required argument, e.g. `--device cpu`), and it does not
  rebuild out/scenes: after main changes the ABI, a branch's recordings are stale and analytic/integrator
  canaries fail "without the patch". Rerun `tools/scenes.py` in the worktree (through tools/lib/python.ps1)
  and land again. Keep every shell's cwd out of a worktree that is about to land.
- The Canary line must read `tasks/NNN.canary.patch must turn red: ...`; land runs every `*.py` token on it as a
  test, even in prose. Windowed-Blender canaries make prepare take about an hour (land-pr.sh allows 80 min).
- Any change under tests/ (including a re-blessed tests/budgets/counters.toml) needs a canary patch.
- If hosted `ci` never starts for a landing PR ("ci is not success" with no run listed), close and reopen
  the PR to fire pull_request again, wait for it, then `tools/land.ps1 merge NNN`. Hosted ci also runs the
  headless sync suite against the bpy wheel, which the local gate does not.
- A prepare that passes and then fails (ci red, refused merge) leaves a "(land)" commit on the branch that
  deletes the task file; `git revert` it in the worktree before the implementer continues or you re-land.
- Start landings only with run_in_background, never `( ... &)` from a foreground shell: that does start, Git Bash's
  ps does not show the pwsh, and two prepares in one worktree race on the canary patch (tree left dirty). Check
  Win32_Process for land.ps1 before relaunching. Chain landings by waiting for the previous land_with_body/land-pr
  process to exit, not for log text.
- A task that adds a tools/lib switch its own canary needs (085's -Gui) cannot prepare with main's
  land.ps1; run the worktree's tools/land.ps1 prepare, then open the PR and merge with main's.
- A stopped agent's worktree stays locked by the harness ("claude agent ... pid N"): land cleanup then fails; run
  `git worktree remove -f -f <dir>` and `git branch -D task/NNN-*` after the merge.
- Stale worktree directories with no git registration: .claude/worktrees/agent-a3a823bde1b1e2a0c,
  agent-a61c36ff96e523f48, agent-a631435169fa2c847 (safe to delete after a look).
