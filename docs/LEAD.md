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

8. Next batch by agent (owner, 2026-11-24): when the written tasks run out (every remaining task file is
   dispatched or blocked on the spine), spawn a planning agent on model `fable` without asking. Brief it with
   docs/PLAN-M1b.md, docs/ROADMAP.md, STATUS, BACKLOG, the ledger in tasks/README.md, the measured rows since the
   last batch and the task-file format. It analyses the slice and writes the next batch's task files. The lead
   then runs step 7's scope pre-flight and Opus critique before dispatching anything.

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

## Handover (M1b second batch under way, third batch written, 2026-11-24)
State for the next lead. Agent ids do not carry across sessions; an open task goes to a new implementer briefed from
its task file. An implementer always gets a fresh worktree: to continue a branch, `git -C <old> switch --detach`, then
brief the agent to `git switch task/NNN-*` first in its own worktree. A worktree whose agent changed nothing vanishes.
Brief every implementer with the environment lines below (fresh worktrees have no deps cache).
- Landed this session (10): 145, 178, 180, 185, 147, 200, 179, 199, 148, 168 (ledger in tasks/README.md). `check full`
  is due (last green after 161; 10+ landings since): run it in a quiet window before the next spine landing.
- Owner decision 40 (B-05 shade_surface 7.0 s for M1b; B-31 accepted; Opus for O tasks; rulings 64-90 folded by 185).
  Ruling 92: spine queue after 175 is 192, false interfaces, extension routing; coating IOR a constant (n, k), PROVISIONAL.
  Ruling 93: k_shade_surface spills up to 16 / 16 B for M1b (old 0 / 0); owner should see it (lead ruling on a D10 row).
  Row now 128 regs, stack 400, spills 4 / 4 B, compile 5.33 s (after 148). Third batch 186-201 written (planner, Opus
  critique, rulings a-f in README); 202 (Mix part 2, after 148) and 203 (fast tier under load, perf after a gate, -Queue).
- In flight (branch, worktree under .claude/worktrees): 156 alpha pass-through (kernel, Opus; task/156-alpha-pass-through,
  agent-a2c90306...): all done but the gate; lead ruling 2026-11-25 gives d3_alpha_cards `[truth] size 128, spp 128` (same
  2M samples; 32x32 at 2048 spp cost about +11 s); row after 156 spills 8 / 8 B, compile 5.60 s; if no report arrives,
  brief a new implementer to rerun the full gate on that branch, then Opus review and land; 203 (tools, Sonnet; worktree agent-ae49e78d..., measures under load). 176 OIDN (task/176-oidn-backend,
  agent-a1465cd7...): reviewed PASS (third round), its landing passes gate, b1m and b5m but fails B-19 `b1m.idle_share`
  1.29 against 1.239 twice. Not 176: on the same loaded machine main (d9f1149) read 0.171 and 0.169, and 160's own commit
  7723d1a (the baseline's code) read 0.2005 then 0.1283 back to back; the stored line's spread is 0.21. b1m's idle share
  is host launch gaps and moves with CPU load. Land 176 in a quiet window (no implementer compiling); if it still fails
  quiet, profile with 123's nsys method; never move the baseline without a ruling naming old and new. 203 step 3 owns the
  method. Then 202 (kernel, Opus; compiler.cpp after 156 and 168: rebases), 158 after 156, then 163 (spine order 158, 163).
- Landings: `pwsh <main>/tools/land.ps1 submit NNN PASS|SKIP -Title t -Evidence t` from the task's worktree, in the
  background. It refuses while another land.ps1 runs (203 adds -Queue). To chain, wait on the previous task's output file:
  `until grep -qE "merge|REFUSE|FAIL|exited" "$f"; do sleep 20; done` (background pwsh shells hide their command line, so
  a process-list watcher sees nothing and a regex on the command line matches itself). Never commit to main while one runs.
- Load: landings beside implementers fail B-08 (`check fast` 10.0-11.6 s; quiet 7.3-8.5 s) and check perf right after a
  gate on a hot GPU (176 read b1m 1.050; main reads 1.006-1.013 cool). Both are task 203; until it lands, retry once and
  say so in the evidence. Gate under load 143-183 s (hold line 170 s measured, decision 38).
- After 163 lands: spawn the fable planner (loop step 8) for what README "Still to write" lists: extension routing with the
  TableArena, false interfaces, `shade_light_path_ext` rows, Depth and Alpha AOVs, DLSS hit distance, 192's place, slice D.
- Review lessons: Opus reviews found real defects in 147, 148, 168, 176, 179 and 199. A chained `check fast | grep && git
  commit` once committed on a red fast: gate commits on `grep -q "^PASS fast"`. A (land) commit left by a refused merge
  must be reverted before reuse (land does this; check the worktree log).
- Housekeeping: delete the local branch backup/148-before-179 and old worktree-agent-* branches; leftover worktree
  directories: agent-a0b454c7 (plan/m1b-batch-3, merged), agent-aa3ef21a (merged docs branch), detached agent-a6b876d6,
  a9e75b55, abbd2383, abec299b, ac16ade2, acc6e172, aef65fb6; remove when no process holds them. cli/bench.cpp still
  stamps COMPUTERNAME (BACKLOG); perf.ps1 rewrites it. Never commit a bench line from bench.ps1 directly.
