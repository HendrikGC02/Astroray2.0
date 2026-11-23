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
5. Land: `pwsh tools/land.ps1 submit NNN PASS|SKIP [-Evidence "what I saw"]` (run in the background). Submit
   does prepare, the PR, the verdict comment, the hosted ci wait, the merge and a best-effort cleanup, undoes
   its own commit on a failure and appends its timing to out/land/history.jsonl of the main checkout; read its
   last `REFUSE` or `FAIL` line. It refuses while another land.ps1 runs. Land the oldest green branch first. Do not commit to main while a landing runs. A task with a `Look at:` line
   needs the lead to view the image (Read the PNG) first and pass what was seen as `-Evidence`; submit posts the
   `evidence:` comment.
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

## Handover (M1b slice 0 closed, second batch written, 2026-11-23)
State for the next lead. Agent ids do not carry across sessions; an open task goes to a new implementer briefed from
its task file. An implementer always gets a fresh worktree: to continue a branch, `git -C <old> switch --detach`, then
brief the agent to `git switch task/NNN-*` first in its own worktree. A worktree whose agent changed nothing vanishes.
- Landed this session (13): 162, 151, 136, 154, 150, 138, 141, 139, 140, 161, 171, 160, 167 (ledger in tasks/README.md).
  `check full` green at 810 s after 136 and 659 s after 161; next due after 10 more landings. Slice 0 is closed. Landings use
  `pwsh <main>/tools/land.ps1 submit NNN PASS|SKIP -Title t [-Evidence t]`, run from the task's worktree in the background.
- Owner decision 39: B-38 = 1.03 PROVISIONAL; decision 38 confirmed (cases move to `check full` when grouping runs out; the
  lead proposes the list with each case's history first). Ruling 91: spine queue 158, 163, 173, 174, 175; stop on
  spills or stack over 512 B; CPU and CUDA of a new statistical case in one concurrent group; new rows from first measurement.
- Second batch 163-183 written (README "Second batch"; 184 dropped). 171 landed. Reviewed PASS, waiting for 158:
  178 (PR #132), 180 (PR #130). 167 landed (BACKLOG rows 64 and the huge-texel half of the 150-review row are done;
  negative texels stay open).
- Next to dispatch: 176 (gpu lane, free now), then 170 after 156; 165 then 166 in the blender lane (free now).
- Branches not yet landed from the first batch: 145 (B-05 compile 4.96-5.35 s under load against ruling 72's 5.0 s: land
  in a quiet window, no builds beside the gate; if it fails, owner item under ruling 72 with the D10 split measured), then
  147 (rebase, renumber to opcode 18, refresh canary), then the spine 148, 156, 158 (all O, kernel lane, one at a time on
  shade_surface.h). Most of the second batch waits on 158.
- Review lessons this session: hosted `ci` must be green before a landing (151 and 136 were red from their first push);
  canary patches merge adjacent breakages into one hunk; Opus reviews found real defects in 138, 141, 150, 161, 171,
  178, 140 and the 0009 record edit. A (land) commit left by a refused merge must be reverted before reuse.
- Planning still owed before M1b closes: (1) third batch at the slice review after 158 and 163 land (spine numbers known):
  extension routing and false interfaces with a ruling extending 91's queue, grating scene and V8, 0005 V6, tree vs alias
  row and B-23, ratio tracking and per-launch cap (V6, check 26), HDRI/S3/shader balls, final denoise and F12 passes,
  Diagnostics panel, heat maps (docs/PLAN-M1b.md section 2 slice rows); (2) slice D's ~15 close tasks, B-20 with
  its decision table first; (3) M2's planning session during slice D, once M-1 at M1b scope and B-20 are measured;
  (4) the 0002 D10 ladder only if a spine landing spills or passes 512 B stack (145 compile past 5.0 s goes to the owner).
- `check fast` B-08/B-12 fail under concurrent builds (BACKLOG row); rerun once on a quiet machine and say so.
- Leftover worktree directories (LEFTOVER lines; a process held a file): agent-a564b0651c3231eca, agent-a78f32a32133f25b4,
  agent-a5d451cc451a9b96e, agent-a5d5f22542d950a96, and detached old worktrees (a9e75b55, abbd2383, abec299b, ac16ade2,
  acc6e172, aef65fb6) plus agent-aa3ef21a (merged docs branch): remove when no process holds them (BACKLOG housekeeping).
- The branch of PR #133 (merged, deleted) once carried the PC's computer name in bench lines; cli/bench.cpp still stamps
  COMPUTERNAME (BACKLOG row); perf.ps1 rewrites it. Never commit a bench line from bench.ps1 directly.
