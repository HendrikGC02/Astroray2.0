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
Hand over to a fresh lead session every 25-30 landings or at the first compaction: update STATUS and the
handover below, then stop. Send long landing logs to a `digest` agent. `tools/lead/deepseek.sh` (DeepSeek
through opencode, owner decisions 32 and 36) may draft bench-history lines, backlog rows and digests from
repository text; treat its answer as a draft.

## Handover (M1b second batch, landing-load fixes in flight, 2026-11-25)
State for the next lead. Agent ids do not carry across sessions; an open task goes to a new implementer briefed from
its task file. To continue a branch, `git -C <old worktree> switch --detach`, then brief the agent to `git switch task/NNN-*`
first in its own worktree. Brief every implementer with the environment lines above and the current k_shade_surface row
(128 regs, stack 400 B, spills 8 / 8 B, compile 6.0-6.4 s under load of 7.0 s; task files' Budget lines still say 384 / 0 / 4.6:
stale, correct them in the brief).
- Landed this session (9): 169, 156, 203, 202, 158, 181, 176, 177, 165 (ledger in tasks/README.md). `check full` passed at the
  start (731.5 s on d0cc2d2) and is due after the next landing. Quiet main gate 145.5 s after 176 (gate_runs).
- Rulings this session: 94 (B-09 excludes the gate's build steps, PROVISIONAL, owner list in STATUS). Task-level rulings
  are in each task's Progress. Owner decision 40 lets the lead decide; record number or gate changes as rulings.
- The blocker: landings fail on machine load. A landing gate rebuilds after the canary (16-31 s of builds in the tier) and
  meets other worktrees' compiles: 181 183.2 s, 164 refused four times (build_info race, B-05 7.29 s, B-09 188.8 / 183.3 s).
  Until 204 lands, land in a quiet window: message each implementer to commit and pause, check `Get-Process cl,nvcc,ninja`,
  land with `-Queue`, then resume them. That worked for 176 and 165.
- In flight, all paused or idle (no agent of this session will report again):
  - 204 tools (task/204-build-info-stamp-race, worktree agent-a756de9e...): cause confirmed (git status/diff rewrite
    .git/index, build_info.cpp depends on it); canary and 0009 d5/d8/d11/B-09 text committed; state of the code unknown
    after a PC crash: a new implementer reads Progress and the log, finishes steps 2-4 (restat stamp, compile timing in
    the window, `gate -Landing` from land.ps1, ruling 94's clock), runs fast, gate, canary; then Opus review (process
    and concurrency), land in a quiet window. Lead answers already given: the shared cmake function, the extracted settle
    function and the fixture's ~1-1.5 s gate cost are accepted.
  - 164 cpu (task/164-compiler-flags-and-nan-rule, agent-a5de2621...): reviewed, PASS. Land right after 204.
  - 163 spine (task/163-metal-in-the-stack, agent-afe8fb64...): reviewed PASS WITH FIXES, fixes done (65b6853); lands after
    204 (its gate 169.6 s without builds). Then spawn the fable planner (loop step 8).
  - 182 kernel (task/182-shade-volume-skeleton, agent-ac84f650...): code and tests done, paused twice, not rebased on
    165/177; owes a rebase, the canary matrix, fast and gate, then an Opus review (kernel, upload, routing). Rulings in its
    Progress; k_route_hit 28 to 40 regs (state the row's limit); new k_shade_volume 34 regs.
- Next to dispatch: 172 (kernel S, ready: after 158 and 177), 170 (gpu measurement, quiet machine), then 183 after 182,
  186 after 163, 189 after 172, 173 (spine) after 163 and 172.
- The PC crashed at 05:59 on 2026-11-25: bugcheck 0x133 DPC_WATCHDOG_VIOLATION (dump C:\Windows\MinidumpJ526-12000-01.dmp).
  The System log carries nvlddmkm event 13 (Xid MMU NACK) about every 3-10 min since yesterday: the deliberate
  out-of-bounds kernel of src/kernel_gpu/test_fault.cu run by the gpu abi tests in every gate. The watchdog in the
  display driver during a fault recovery is the likely cause, unconfirmed without a debugger (none installed).
- Review lessons: Opus reviews found real defects in 156, 202, 169, 181 (three rounds), 158, 177 (the task text's own
  sigma rule), 164 and 165. Canary patches above 8 hunks skip canary.ps1's matrix: demand per-hunk evidence by hand.
  Task acceptance commands with comma lists or old suite names (`nee_agree/*`, `shade_surface_agree/*`) match nothing;
  accept the real case names.
- Backlog rows added this session: see docs/BACKLOG.md (Blackbody folding, TABLE world colour, stack.h silent zero,
  medium ray mask, Blackbody above 1e9 K, gate creep, stale incremental link, 203 review notes).
