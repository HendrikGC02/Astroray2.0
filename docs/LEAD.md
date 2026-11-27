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

## Handover (seventh lead, 2026-11-28)
State for the next lead (the sixth lead's notes below still hold where not superseded). Agent ids do not carry across sessions:
detach the old worktree (`git -C <wt> switch --detach`, `git worktree unlock`), brief a new implementer from the task's Progress
and `git log main..task/NNN-*`.
- Landed this session (3): 208 (8c36e7ac; second submit after hosted ci's headless Depth test; reload spill 40 -> 36 / 36 B by
  `AR_NOINLINE` on the transparent tail, the row's second and last remedy, allowance unchanged; kernelgate now adds out-of-line
  callee spills to their entry and refuses callee frames and unattributable blocks), 230 (ca0e224e; guide RGB packed 3x10-bit,
  volatile read keeps k_shade_surface at 128 / 416 / 4 / 16 B), 218 (d27b8797; addon Depth as Blender's Z pass, Combined alpha).
- In progress (PC crash 2026-11-27 night; both branches clean and committed, worktrees locked):
  206 on task/206-extension-routing-table-arena (worktree agent-a92f5f76822510797): host arena, ShadeKind template, Ext entry,
  tests, compile timing done (shade_surface 7.44 s, +0.09 s from the template by interleaved A/B, accepted; shade_surface_ext
  5.5-5.6 s, ruling 97 read as "takes shade_surface's 8.0 s", the owner may correct it; k_shade_surface_ext 124 / 400 / 0 / 0).
  Lead rulings in its Progress: guides on an Ext delta sample keep the tint (PROVISIONAL, alternative: total efficiency),
  GRATING_TABLE_LIMIT also for a TableArena past 4 GiB (0001 row amended), scenes/ext_grating_card built by the scenes step.
  Needs: checks, canary, evidence, Opus review (kernel O), land in a quiet window.
  236 on task/236-denoise-small-texture-detail (worktree agent-a8893832248f81927): resumed after 230 fixed its cause (magenta
  albedo guide); re-runs the bisection on main and picks a documented OIDN mode if b_tx still denoises worse than raw. No OIDN
  docs in refs: cite the installed OIDN headers; no downloads.
- Next: 209 after 206 (shade_surface.h, one at a time); 215 after 206 and 209; 213 after 215; 214 after 206; 188 after 206.
  `check full` is overdue (last before 208): run it after 236's capture, with `fetch.ps1 assets` for cornell_box and
  studio_small_09 in the main checkout first (decision 43).
- Lock: owner.json named a build (PID 12600) that died in the crash; tell agents to report a refusal rather than delete it.
- Lessons: (12) the headless sync suite runs only on hosted ci (BACKLOG): run it through Invoke-ArBlender before an addon or
  pass-list landing. (13) canary.ps1 skips its per-hunk matrix above 8 hunks: have each hunk applied alone. (14) Task Budget
  lines older than decision 43 are stale: brief k_shade_surface 128 / 416 / 4 / 16 B and B-05 8.0 s.

## Handover (sixth lead, 2026-11-27)
State for the next lead. Agent ids do not carry across sessions; an open task goes to a new implementer briefed from its task
file, its Progress and `git log main..task/NNN-*`. To continue a branch, `git -C <old worktree> switch --detach`, then brief the
agent to `git switch task/NNN-*` first in its own worktree.
- Landed this session (1): 193 (62e48261, second submit; the first failed hosted ci on a test lambda the hosted compiler rejects,
  fixed with `static constexpr`). Gate 170.4 s, at decision 38's 170 s hold line (+20.5 s against 220, the new capped-equals-
  uncapped checks): watch the next landings; hold one only if its measured max passes 170 s. Ruling 111 (shadow tracker side
  buffer) and the B-17 owner item went in with 193's squash.
- Nothing runs. 208 (Depth and Alpha) is complete and clean on task/208-depth-alpha-aovs at 5d52c97c (worktree
  agent-a3e669b1d98df0318, locked: detach it before a new implementer switches to the branch), tests and canary (13 cases red)
  done, evidence viewed and copied to out/land-evidence/208/{depth,alpha}.png (alpha steps 0.25/0.5/0.75/1.0; depth speckle is
  the sample-0 rule). One blocker: kernelgate's `k_shade_background` reload variant (tests/gpu/kernels/reload_variant.cu)
  spills 40 / 36 B against the 36 / 36 B allowance in src/kernel/budget.toml (production cubin within its rows). Five
  attempts inside 208's Scope failed: a noinline helper 44 / 44, a merged tail 44 / 36, a hoisted sample id 44 / 36, an early
  return 52 / 56, no finite check 40 / 36.
- Owner, 2026-11-27: "remedy first" (no allowance raise now). Next step: the second and last remedy for this row (LEAD.md
  "Measurement tasks"): extend 208's Scope by a Scope line in its task file on the branch (simpler than a new task: same branch,
  same diff) to allow src/kernel/portability.h (an `AR_NOINLINE` macro; none exists today: `AR_FN` is `__forceinline__`) and a
  restructure of shade_background.h's transparent-background tail, e.g. the transparency write moved out of line or the
  branch folded into the existing film write. Allowance stays 36 / 36 B. If the reload variant still spills past 36 B, stop:
  STATUS owner item with the five plus the new numbers, proposed 36 -> 40 B store (loads 36). Then Opus review
  (`tools/lead/review-bundle.sh 208 task/208-depth-alpha-aovs`), land in a quiet window with
  `-Evidence` from the two PNGs; 208 states the k_shade_surface row before and after.
- Spine after 208: 230 (guide RGB; unblocks 236, which waits on branch task/236-denoise-small-texture-detail, worktree locked),
  206, 209 (all shade_surface.h, one at a time). 218 (addon side of 208) after 208. 215 after 206, 209 (193 landed).
  213/214/216/217 at the slice close. When nothing is dispatchable, loop step 8 (fable planner).
- Owner list (STATUS): rulings 109 and 110 and the decision-44 reading; B-17 cap (4,096 PROVISIONAL or about 1,000); the
  optional coincident-hit tie rule (235); B-10; B-11 basis. `check full` still needs `fetch.ps1 assets` for cornell_box and
  studio_small_09 in the main checkout (decision 43 covers it); landings since the last full: 9 (205..220, 193): run
  `check full` after the next landing.
- Housekeeping: many stale agent worktrees under .claude/worktrees (`git worktree list`); agent-a4abb4c25eadf1a66 could not
  be removed (a process holds a file). This session could not start or hand off sessions (Remote Control refused): the owner
  starts the next lead by hand.
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
  (9) Hosted ci builds CPU-only and uploads no build.log on a compile failure; a local `check ci` does not reproduce it
  (BACKLOG): read the error from the PR's ci run page. (10) A ruling that adds bytes to a pool item checks the item budget
  first (ruling 111 moved to a side buffer for the shadow item's 128 B). (11) After a landing whose squash contains lead
  commits, the local main rebase conflicts: `git rebase --skip` the already-landed ones.
