# Lead runbook

How the lead (coordinator) session runs the build phase. Implementers follow CLAUDE.md and
`.claude/agents/implementer.md`; this page is the lead's half. State lives in docs/STATUS.md, the plan in
docs/PLAN-M1b.md, order in tasks/README.md, unowned items in docs/BACKLOG.md, rulings in docs/decisions/RULINGS.md.

## Loop per task
1. Ready: every task in "Depends on" has landed (its file is gone from tasks/) and its lane has room
   (tasks/README.md: cpu 5-8, kernel three with one on `shade_surface.h`, gpu one, blender one, tools two).
2. Dispatch one `implementer` agent per task in its own worktree. The agent definition carries the standing
   brief (branch, early commit, canary recipe, stop-and-ask rules, report shape). The brief names only: the
   task number, contracts already on main that the task must reuse, and warnings specific to it. Give Opus
   to tasks marked O; use effort high for kernel and physics tasks.
3. Rule on questions in the task file (Scope line, Progress line) or in RULINGS.md, then reply to the same
   agent so it keeps its context. A question that would change a number, a gate or an owner decision goes to
   STATUS's owner list; the task waits or takes the stated default.
4. Review: `tools/lead/review-bundle.sh NNN task/NNN-slug`, then a `reviewer` agent (Opus) on out/land/NNN
   for kernel, physics, sampling, ABI, concurrency and process diffs and for any diff that edits a record
   (O31, ruling 87); host-only plumbing may skip. FAIL: findings back to the implementer, then review again.
   PASS WITH FIXES: the implementer fixes, the lead reads the fix diff and continues; send it back to the
   reviewer only when the fix changes control flow in concurrency, lifetime or teardown code.
5. Land: `tools/lead/land-pr.sh NNN PASS|SKIP "title"` until task 138 lands, then
   `pwsh tools/land.ps1 submit NNN PASS|SKIP` (run in the background; it holds the machine lock). Land the
   oldest green branch first. Do not commit to main while a landing runs. A task with a `Look at:` line
   needs the lead to view the image (Read the PNG) and post a PR comment starting `evidence:` first.
6. After a landing: update the gate ledger line in tasks/README.md; move notes for later tasks into their
   files; move unowned gaps into docs/BACKLOG.md. Every 10 landings, and at each slice close, run
   `pwsh tools/check.ps1 full`.
7. Slice review (docs/PLAN-M1b.md section 2): read the slice's measurements, write the next batch of task
   files with a scope pre-flight each (a Sonnet agent per five tasks, briefed with the outline, the task
   format and "grep every symbol the Do list touches; name call sites, registration lists, budget rows and
   forced tests in Scope"), run an Opus critique on a batch of more than ten, update tasks/README.md and STATUS.

## Measurement tasks
docs/PLAN-M1b.md section 5. A capture runs with the gpu and blender lanes idle. A budget row gets at most two
remedy tasks in a milestone; then it goes to the owner with the numbers. A limit a task proposes to change is
listed in its PR under "Limits changed" and in STATUS's owner list; it changes only with a ruling that names
the old and the new value, never in the landing whose test it failed.

## Environment for agents and scripts
Tools read the user environment themselves. A pwsh that calls other programs directly starts with:
```
$env:ASTRORAY_DEPS_CACHE = 'C:\Astroray2.0\third_party\_cache'
$env:ASTRORAY_BLENDER = [Environment]::GetEnvironmentVariable('ASTRORAY_BLENDER','User')
```
Worktrees share the dependency cache through that variable; never link a worktree to the cache.
`check fast` times lint and tests; the build step is outside B-08 (owner decision 36). A red B-08 is now a
real finding: fix it, do not rerun.

## Never
`git stash`, force push, opening the Astroray 1.0 repository, calling cmake/ninja/nvcc/blender directly,
personal names, emails and paths in commits, PRs or external requests, starting anything the owner has made
conditional on a go-ahead, changing `.claude/settings.json` or CLAUDE.md's rules without the owner's own
word, pushing to main with lint or `check fast` red.

## Session hygiene
Hand over to a fresh lead session every 25-30 landings or at the first compaction: update STATUS and the
handover below, then stop. Send long landing logs to a `digest` agent. `tools/lead/deepseek.sh` (DeepSeek
through opencode, owner decisions 32 and 36) may draft bench-history lines, backlog rows and digests from
repository text; treat its answer as a draft.

## Handover (M1b slice 0 in flight, 2026-11-23)
State for the next lead. Agent ids do not carry across sessions; a task still open goes to a new implementer
on its existing branch and worktree, briefed from its task file and the rulings below.
- Landed in slice 0: 155, 146, 157, 143, 152, 137, 144, 142, 153, 159, 149 (ledger in tasks/README.md, owner
  decision 38). The 10-landing `check full` is due after 162 lands (162 fixes its known red).
- Open PRs, reviewed, ready to land with land-pr.sh: 151 (#122
  PASS), 136 (#124 PASS), 162 (#123 SKIP, evidence posted).
- Branches with work, not yet PRs:
  - 154 light-tree builder: rebase on main, cite ruling 90 in its 0004 D10 sentence, open the PR, land.
  - 145 BLACKBODY/CAUCHY_ABBE: reviewed; B-05 k_shade_surface compile 4.96-5.35 s under load against ruling
    72's 5.0 s (quiet 4.6 s baseline). Land in a quiet window (no builds beside the gate). If it still fails,
    owner item under ruling 72: raise the limit, the D10 split, or stub emission sites (about 8 inlined
    Planck copies). `#pragma unroll 1` broke k_shade_background's stack row: do not retry.
  - 147 opacity entry: after 145 lands, rebase, renumber to opcode 18, refresh canary, land.
  - 138 land submit/undo/cleanup: PR #126, rulings applied (per-repository busy marker; clean case restored,
    fast_landing 1.9 s, accepted); gate 134.3 s. Needs Opus review; after it lands use
    `pwsh tools/land.ps1 submit`. Unrelated: CRLF ghost diffs on .ps1 files (`git add --renormalize`).
  - 141 visible lock and test runner: PR #125, rulings applied (fast_lock 2.2 s scaled with a ready-file
    handshake; waiting lines throttled after 3). Needs Opus review; gate not rerun after the rework. Its env
    defaults (3600 s, 30 s, Globalstroray2, %ProgramData%stroray2) go into 0009 d8 as a lead record edit.
  - 150 half/float textures: done (gate 164.8 s, B-05 4.62-4.69 s, canary PASS); half_to_float's one exact float
    subtraction accepted (integer-only form broke B-05). PR #127; needs Opus review (kernel, 0007 d13
    record edit, vendored tinyexr v1.0.12).
- Not dispatched: 161 (blender, after 162), 139 (blender), 140/160 (after 136, 138), 148, 156, 158 (spine
  queue after 147). Then the slice-0 review writes the next batch (PLAN-M1b section 2 outlines).
- Also owed: 0009 d28 write-back of 137's canary rules (record edit, Opus review).
- Gate under load reaches 175-179 s (compile timing re-times every TU that includes a changed header, plus
  contention); quiet gate max 132.7 s. Land kernel-header changes when few implementers build.
- Briefs must set ASTRORAY_DEPS_CACHE and ASTRORAY_BLENDER (User env) and say: scratch under out/tmp/NNN/,
  never a shared scratchpad; no commit while a tools/ build runs.
- Process changes made by the planning session, confirmed by the owner (decision 36): B-08, lanes, reviewer
  verdicts, DeepSeek wrapper, rulings 86-88, the implementer and reviewer definitions, and `process/guards`
  (merged: deny rules for direct cmake, ninja, nvcc, blender; an allow-list; CLAUDE.md rule 1's scope clause).
- Landing lessons (until task 138's `land.ps1 submit` absorbs them):
  - land.ps1 cleanup fails while an agent holds its worktree lock or a shell's cwd is inside it: after the
    merge run `git worktree remove -f -f <dir>` and `git branch -D task/NNN-*`. Cleanup deletes the
    worktree's out/: copy the PR body and evidence out first. land-pr.sh reuses an open PR, so open it with
    the real body (and the `evidence:` comment for a `Look at:` task) before running it.
  - A failed prepare leaves a "(land)" commit; land-pr.sh reverts it.
  - land.ps1 does not rebuild out/scenes: after an ABI or recording change rerun `tools/scenes.py` in the
    worktree. A leftover Blender can hold out/bin/astroray_core.dll (LNK1168): check before landing.
  - Start landings with run_in_background only. If hosted ci never starts, close and reopen the PR.
  - A task that adds a tools/lib switch its own canary needs: prepare with the worktree's land.ps1.
  - Auto mode refused a landing that combined REVIEW SKIP with the lead's own `evidence:` comment; run the reviewer.
  - A GitHub 5xx while posting the gate status fails prepare after a passing gate: reland (138 retries 5xx).
  - Hosted ci flake on one case: `gh run rerun --failed`, then `land.ps1 merge NNN`.
- Housekeeping: docs/BACKLOG.md last row (stale worktree directories and branches). The file
  out/tmp/blend_cornell_area.png under the drive root is a stray from task 090: the owner removes it.
