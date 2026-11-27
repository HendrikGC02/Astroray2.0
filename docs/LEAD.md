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

## Handover (fifth lead, 2026-11-27)
State for the next lead. Agent ids do not carry across sessions; an open task goes to a new implementer briefed from its task
file, its Progress and `git log main..task/NNN-*`. To continue a branch, `git -C <old worktree> switch --detach`, then brief the
agent to `git switch task/NNN-*` first in its own worktree.
- Landed this session (7): 205, 190, 194, 170, 219, 201, 237 (ledger in tasks/README.md). 235 closed without a landing (the
  d2 colour difference is the declared coincident pair 301/302, a traversal tie; no engine defect). `check full` passed on
  201's branch (740.1 s). Gate landings 134-141 s.
- Owner decisions 43 and 44 answered every open item: B-05 shade_surface 8.0 s, now on the new clock (237: timing inside the
  build's MSVC environment, vcvarsall 0.70-0.76 s off every TU; shade_surface 7.04-7.14 s quiet, scatter now ~0.05 s);
  D10 1.8x accepted; v1 margin confirmed; V11 on cornell_area, shader_balls, cornell_point (met), b_tx's loss is task 236;
  B-19 pq accepted red on b_foliage. Rulings 109 (D10 ratio floor 1.70) and 110 (V11 device agreement within 5 %) are
  PROVISIONAL for the owner. The decision-44 reading (8.0 s kept on the new clock) is stated in the record for the owner.
- Running when this lead stopped (dispatched by this session; their reports return to this session, not to the next lead):
  - 220 spine (task/220-loud-lobe-values-and-limit-flags, Sonnet): non-finite lobe values INVALID, limit-end flags. Opus review
    (kernel diff) and a quiet-window landing. Then the spine: 208, 206, 209, and 230 (shade_surface.h).
  - 236 stopped with its cause named and now waits on 230: textured materials write the magenta placeholder albedo guide
    (0008 d15), so OIDN smooths the texture as noise; dispatch 230 (guide RGB, shade_surface.h, spine-serialised) after 220.
  If either has no fresh commit when you start, brief a new implementer from its Progress.
- Next: 193 after the lead rewrites its Reference; 218 after 208; 215 after 206, 209, 193; 213/214/216/217 at the slice close.
  When nothing is dispatchable, loop step 8 (fable planner).
- Process lessons: (1) land.ps1 refuses outside the task's worktree branch: run it from the worktree; an implementer's branch
  must be named task/NNN-* (237's was not until renamed). (2) Landing cleanup deletes the worktree's out/: copy history
  lines, PR bodies and evidence out before landing, or have the implementer commit them (190's history line was rebuilt from
  its Progress). (3) Two landings appending to bench/history conflict; resolve by date order (lint requires non-decreasing).
  (4) A measurement task's b1m/b5m pass_ms line becomes check perf's baseline (BACKLOG). (5) A merge refused with "ci is not
  success" while a second ci run was pending; a resubmit passed (BACKLOG). (6) Quiet windows: pause implementers explicitly
  before kernel landings and captures; 194's first landing failed B-05 beside 219's builds. (7) Keep to two implementers;
  pause one when a review fixer would make three. (8) Briefs: say "never run fetch.ps1 deps; set ASTRORAY_DEPS_CACHE" (235
  downloaded deps into its worktree), rows k_shade_surface 128 / 416 / 4 / 16, compile ~7.1 s of 8.0 s on the new clock.
- Cache defect (BACKLOG): scene recordings embed worktree asset paths; the HDRI fetched by 201 went with its worktree, so
  `check full`'s denoise step fails until `fetch.ps1 assets -Name studio_small_09` runs in the main checkout (decision 43 covers it;
  ask the owner if unsure). A tools task should make recordings path-independent.
- Owner list (STATUS): rulings 109 and 110 and the decision-44 reading to confirm; the optional coincident-hit tie rule (235);
  B-10 (check full ~740-790 s, unenforced); B-11 basis.
