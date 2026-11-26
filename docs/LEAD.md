# Lead runbook

How the lead (coordinator) session runs the build phase. Implementers follow CLAUDE.md and
`.claude/agents/implementer.md`; this page is the lead's half. State lives in docs/STATUS.md, the plan in
docs/PLAN-M1b.md, order in tasks/README.md, unowned items in docs/BACKLOG.md, rulings in docs/decisions/RULINGS.md.

## Loop per task
1. Ready: every task in "Depends on" has landed (its file is gone from tasks/) and fewer than two implementers
   are running (owner decision 42: two at once in all lanes together, at most one in gpu or blender; one at a time
   on `shade_surface.h`). Prefer the task that unblocks the spine. A landing runs beside the two.
2. Dispatch one `implementer` agent per task in its own worktree. The agent definition carries the standing
   brief (branch, early commit, canary recipe, stop-and-ask rules, report shape). The brief names only: the
   task number, contracts already on main that the task must reuse, and warnings specific to it. Give Opus
   to tasks marked O; use effort high for kernel and physics tasks.
3. Rule on questions in the task file (Scope line, Progress line) or in RULINGS.md, then reply to the same
   agent so it keeps its context. A question that would change a number, a gate or an owner decision goes to
   STATUS's owner list. It pauses the landing, not the work (decision 42): reply at once that the implementer
   finishes the rest on the stated default or your provisional ruling, unless the answer changes the design.
4. Review: `tools/lead/review-bundle.sh NNN task/NNN-slug`, then a `reviewer` agent (Opus) on out/land/NNN
   for kernel, physics, sampling, ABI, concurrency and process diffs and for any diff that edits a record
   (O31, ruling 87); host-only plumbing may skip. FAIL: findings back to the implementer, then review again.
   PASS WITH FIXES: the implementer fixes, the lead reads the fix diff and continues; send it back to the
   reviewer only when the fix changes control flow in concurrency, lifetime or teardown code.
5. Land: `pwsh tools/land.ps1 submit NNN PASS|SKIP [-Evidence "what I saw"] [-Queue]` (run in the background;
   `-Queue` waits for a running land.ps1 instead of being refused). Submit
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

8. Next batch by agent (owner, 2026-11-24): when the written tasks run out (every remaining task file is
   dispatched or blocked on the spine), spawn a planning agent on model `fable` without asking. Brief it with
   docs/PLAN-M1b.md, docs/ROADMAP.md, STATUS, BACKLOG, the ledger in tasks/README.md, the measured rows since the
   last batch and the task-file format. It analyses the slice and writes the next batch's task files. The lead
   then runs step 7's scope pre-flight and Opus critique before dispatching anything.

## Waiting (owner decision 42)
- Nobody polls. Run `land.ps1 submit` and long checks in the background and end the turn; the notification wakes
  you. Never `sleep`, `until` or a loop on an output file, and never ask an agent to wait that way.
- An implementer that reports "blocked on the lock" names the holder (`owner.json` in `$env:ProgramData\astroray2`).
  A holder past its tool's ceiling is hung: stop that one tools process by its PID, say so, resume the implementer.
- Resume an idle agent within the hour (decision 33's cache). Past it, resume only a small context; otherwise
  brief a fresh implementer from the task's Progress and `git log main..task/NNN-*`.
- The hosted ci wait leaves the machine free: implementers keep working during it.

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
`check fast` times lint and tests; the build step is outside B-08 (owner decision 36). Inside a gate the fast step
holds the fast window (0009 d8, task 203), so a red B-08 there is a real finding: fix it, do not rerun. A
standalone `check fast` still meets unlocked builds; an implementer may rerun it once on time only and say so.

## Never
`git stash`, force push, opening the Astroray 1.0 repository, calling cmake/ninja/nvcc/blender directly,
personal names, emails and paths in commits, PRs or external requests, starting anything the owner has made
conditional on a go-ahead, changing `.claude/settings.json` or CLAUDE.md's rules without the owner's own
word, pushing to main with lint or `check fast` red.

## Session hygiene
Hand over to a fresh lead session every 15-20 landings (owner, 2026-11-25) or at the first compaction: update STATUS and the
handover below, then stop. Send long landing logs to a `digest` agent. `tools/lead/deepseek.sh` (DeepSeek
through opencode, owner decisions 32 and 36) may draft bench-history lines, backlog rows and digests from
repository text; treat its answer as a draft.

## Handover (fourth lead, night of 2026-11-26/27)
State for the next lead. Agent ids do not carry across sessions; an open task goes to a new implementer briefed from its task
file, its Progress and `git log main..task/NNN-*`. To continue a branch, `git -C <old worktree> switch --detach`, then brief the
agent to `git switch task/NNN-*` first in its own worktree. No agent of this session will report again.
- Landed this session (10): 175, 233, 231, 232, 196, 234, 192, 197, 191, 198 (ledger in tasks/README.md). `check full` passed on
  234's branch (777.5 s) and on 191's (789.4 s). Quiet main gate median after 234: 127.2 s (was 167 s); fast 5.6 s. B-38's
  baseline re-recorded on quiet main (ruling 108: b1m 12.25 ms, b5m 12.92 ms).
- Rulings this session: 104 (shadow medium pre-pass, D19 split), 105 (231's fixture gate cost), 106 (delta-light shadow items),
  107 (106 superseded once 205 routes by stack depth, PROVISIONAL until 205 lands), 108 (B-38 baseline). Task-level rulings
  are in each task's Progress.
- Ready to land once the owner answers (all reviewed, all green apart from the owner's number):
  - 205 spine (task/205-false-interfaces-medium-stack, head 5176484e, worktree agent-af6b96aa...): two Opus rounds, the
    last PASS WITH FIXES and those fixes done (canary 15 hunks, per-hunk evidence in its report: each hunk alone red).
    Blocked on B-05 (shade_surface 7.73-7.75 s quiet vs 7.5 s; main 7.19-7.29). With the owner's yes, land it in a quiet
    window: `land.ps1 submit 205 PASS -Evidence` from out/evidence/205/slab.png (Look at line; CPU before/after max |z| 3.27,
    no structure). ABI hash moves (new report code 47). Then the spine: 219, 220, 208, 206, 209, and 230 (shade_surface.h).
  - 190 (task/190-tree-vs-alias-d10-row, agent-a382cf20...): reviewed PASS WITH FIXES, fixes done. Blocked on the owner's
    2x row answer (1.795 measured) and the v1 margin fact; its history lines wait in that worktree's out/tmp/190/.
- Parked: 201 (task/201-v11-final-oidn-check, draft PR #190, agent-a5ab6cd2...): stopped at its decision table (b_tx denoise
  3x worse; owner item). Its tool is good; the full step must not land while a scene fails. Draft PR: `gh pr ready` before
  any land.
- New task 235 (lead-written): d2_aov_geometry renders different colours on CPU and CUDA (R and B off by ~0.03, G equal),
  found by 201. A correctness defect: dispatch it first (kernel, Opus, effort high).
- Next after 235: 194 (hero-lane variance, measurement), 170 (gpu measurement, quiet), 193 after the lead rewrites its
  Reference (task text says so). Most other written tasks wait behind the spine (205). When nothing is dispatchable,
  loop step 8 (fable planner).
- Process lessons: (1) land.ps1 runs from the task worktree: a branch not rebased after a land.ps1 change lands with the old
  script (231 ran pre-233). (2) Kernel and session landings, compile timings and captures need a quiet window: tell the
  implementers to commit and end their turn (they may keep editing source without building), do the quiet work, resume
  them. Tools that hold the machine lock serialise with each other. 232 re-runs B-38 under load; the B-19 share check has no
  re-run (BACKLOG). (3) A draft PR blocks the merge: `gh pr ready`, then `land.ps1 merge`. (4) I let three implementers run
  at times against decision 42's two (a short fixer beside two builders); keep to two. (5) Chain `check fast` and the push
  so a red stops the push. (6) Implementer briefs: state current rows (k_shade_surface 128 / 416 after 205, else 400; spills
  4 / 16; compile ~7.2-7.3 s on main, limit 7.5), "one pattern per call through tools/test.ps1", and the quiet-window rule.
- Owner list (STATUS): B-05 for shade_surface (blocks 205 and the spine), the D10 2x row (190), the v1 margin formula (190),
  V11 scenes and b_tx (201), B-10 (check full 777-789 s, unenforced), B-11 basis.
