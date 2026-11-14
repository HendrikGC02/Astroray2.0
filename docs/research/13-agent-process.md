# Agent development process for Astroray 2.0 (W13)

Date: 2026-11-14. Evidence tags: VERIFIED = read in a primary source or in a local file; REPORTED = secondary or a v1 note I did not re-verify; UNVERIFIED = my reasoning or a claim I could not check.

## Question

Design a lean process for building Astroray 2.0 with Claude Code (Sonnet, Haiku, Opus) plus an open-weight worker (DeepSeek V4.1 Flash via opencode). Owner complaints about v1: too many rules, hooks and status documents; long builds and tests; superseded directives piling up. What goes in CLAUDE.md, which few skills, agents and hooks earn their place, what a task looks like, how work is verified before merge, how status, decisions and memory are kept from growing, and how v1 knowledge is used without contaminating v2 context. Every item below names the failure it prevents.

## Findings

### A. What v1's process cost (local files, read)

- v1 session floor: CLAUDE.md 11,842 B, AGENTS.md 14,703 B, plus skill and agent listings and a SessionStart hook. The recon estimates ~4.5K tokens floor, 10-14K when following the rules, 90K+ if an agent obeys "read STATUS.md first" (chars/4 heuristic, not measured). VERIFIED (recon `00-recon.md` lines 258-262; sizes by `wc -c`).
- `STATUS.md` is 3,484 lines; `.astroray_plan/packages/` holds 334 files; one spec (pkg329) is 13,715 B against a 4,206 B template. VERIFIED (`wc`, `ls`).
- `.claude/` holds 9 agents, 15 skills, 8 hook scripts, ~135 KB of prompt text (agents + skill bodies + hooks). `.opencode/` and `.codex/` duplicate the roles; the shared agents differ by 20-87 lines between copies and nothing checks sync. VERIFIED (`wc -c`; recon agent-tooling section).
- CLAUDE.md section 5 accretes dated directives ("owner 2026-09-22", "2026-09-23", "2026-09-29", "2026-10-18", "2026-10-24", ...), some of them explicitly superseding earlier lines, and names model IDs that change every few weeks. VERIFIED (read v1 CLAUDE.md).
- `stop_journal.ps1` appended placeholders: 1,983 of 1,986 journal lines unfilled. `pyd_shadow_guard` walks the whole repo on each pytest call. VERIFIED (recon, agent-tooling digest).
- Auto memory: 144 files, 696 KB, MEMORY.md 140 lines / 13,734 B. Most entries are facts about v1 code, several flagged stale. VERIFIED (`ls`, `wc`; recon memory-lessons).
- One v1 rule is already obsolete: "never switch model, effort, tools or MCP servers mid-session". Anthropic now documents that effort changes keep the cache on Opus 5.5, Sonnet 5.5 and Haiku 5.5 (API key or subscription), and that deferred MCP tools do not disturb it. Only a model switch is costly. VERIFIED: https://code.claude.com/docs/en/prompt-caching ("Changing effort level", "Connecting or removing an MCP server").

### B. Current Anthropic guidance (primary pages, fetched 2026-11-14)

- **CLAUDE.md** is context, not enforcement; "target under 200 lines"; imports organise but do not cut context cost; contradictory lines make Claude "pick one arbitrarily". Block-level HTML comments are stripped before injection, so maintainer notes are free. VERIFIED: https://code.claude.com/docs/en/memory
- **What belongs in it** (best-practices page): how to confirm work (exact test and lint commands), places where the code misleads, decisions not in the repo, "Anything you've had to correct twice already". Describing architecture and file layout is on the page's "you can stop doing" list; so are ALL-CAPS emphasis and "keep going, don't be lazy" lines. If a rule cannot bend, "turn it into a hook or a deny rule". VERIFIED: https://code.claude.com/docs/en/best-practices
- **Path-scoped rules**: `.claude/rules/*.md` with `paths:` frontmatter load only when Claude reads, writes or edits a matching file; rules without `paths` load at launch. VERIFIED: memory page, "Path-specific rules".
- **Prompt audit**: `/doctor prompt-audit` (v2.1.283+) flags instructions written for older models, dead references and contradictions across CLAUDE.md, rules, skills and agents; it changes nothing until asked. VERIFIED: memory page.
- **AGENTS.md** is read only when no CLAUDE.md (or `.claude/CLAUDE.md`, or `CLAUDE.local.md`) exists in or above the working directory (default setting; needs v2.1.277+). `~/.claude/CLAUDE.md` and `.claude/rules/` do not count for that check. VERIFIED: memory page, "AGENTS.md".
- **Auto memory**: machine-local, one directory per git repo shared by all its worktrees, first 200 lines or 25 KB of MEMORY.md loaded each session, four types (user, feedback, project, reference); Claude skips what it can derive from code. Switch off per project with `autoMemoryEnabled: false`; the directory can be moved with `autoMemoryDirectory`. Subagents do not get the main conversation's memory. VERIFIED: memory page.
- **Skills**: every listed skill's description (cap 1,536 chars) "adds to your context on every turn"; `disable-model-invocation: true` removes the description from context and leaves the skill invocable as `/name`; keep SKILL.md under 500 lines; `/skill-doctor` (v2.1.252+) shows per-skill cost. VERIFIED: https://code.claude.com/docs/en/skills
- **Subagents**: a non-fork subagent sees its own prompt, the delegation message, the full CLAUDE.md hierarchy and git status, not the conversation; `omitClaudeMd: true` skips CLAUDE.md; `isolation: worktree`, `tools`/`disallowedTools`, `model`, `effort`, `maxTurns` exist; "Define a custom subagent when you keep spawning the same kind of worker"; combined descriptions over 15,000 tokens warn at startup. Model resolution: per-invocation parameter, then frontmatter, then env, then main model. VERIFIED: https://code.claude.com/docs/en/sub-agents
- **Delegation is not a quality lever**: "Handing the same task to a subagent doesn't make the answer any better. A fresh reviewer does help, because it hasn't seen your reasoning." Reviewer pattern: give a reviewer only `Read, Grep, Glob` so the limit is "enforced by Claude Code rather than requested in a prompt"; tell it to skip style, because "a reviewer that's asked to find problems will always find something". VERIFIED: best-practices page.
- **Hooks**: deterministic; exit 2 blocks (stderr reaches Claude); other non-zero exits let the action proceed unless `onFailure: "block"`; `SessionStart` with matcher `compact` re-injects context; `/goal` is "a session-scoped Stop hook" whose evaluator judges only what the transcript shows, works with `claude -p`, and accepts a turn clause ("or stop after 20 turns"). VERIFIED: https://code.claude.com/docs/en/hooks-guide, https://code.claude.com/docs/en/goal
- **Permissions**: deny rules are checked first and cannot be carved out by allow; `Read(//c/path/**)` is the absolute-path form on Windows; a Read deny also blocks Edit/Write on the path and is "best-effort" for Grep/Glob; it does not cover a command that reads without naming the file (`grep -r` from inside the directory). Bash deny rules "aren't a security boundary" (they match the command text as written). The PowerShell tool has its own rule family, `PowerShell(...)`, so a `Bash(...)` deny does not obviously cover it; write both. Bash/PowerShell commands can still write files regardless of Edit/Write denies. VERIFIED: https://code.claude.com/docs/en/permissions; the sub-agents page says a `Bash(git push *)` deny "applies to the main conversation and to subagents".
- **`claude --bare -p`** skips CLAUDE.md, hooks, skills, subagents, plugins, MCP and auto memory, but "never reads OAuth credentials or the system keychain": it needs `ANTHROPIC_API_KEY`. v1 states it has no API key. The same page says Bedrock/Vertex/Foundry credentials still work, and that bare mode will become the `-p` default in a future release (watch for this: plain `claude -p` would then also skip CLAUDE.md and hooks). VERIFIED for the flag behaviour: https://code.claude.com/docs/en/headless; the v1 "no API key" fact is REPORTED (v1 AGENTS.md line 95, not re-checked, and it is the owner's current state to confirm).
- **Cache**: model switch recomputes everything (this includes a skill or command whose frontmatter names a different `model`, and `opusplan` toggles); subagents get a 5-minute TTL even on a subscription unless `subagentPromptCacheTtl: "1h"` (or per-agent `experimental: {cacheTtl: 1h}`, v2.1.248+) is set; the main conversation gets 1h only on a Claude subscription within plan usage, and 5 minutes with an API key or after going over plan usage unless `promptCacheTtl` is set; cache is scoped to machine plus directory, so worktrees do not share it; enabling or disabling plugin skills/agents/hooks keeps it. VERIFIED: prompt-caching page.
- **Default-effort regression**: Anthropic's March 2026 quality drop included a Claude Code default-effort change from high to medium (Mar 4, reverted Apr 7). Reason to pin `effort:` in every agent file. VERIFIED: https://www.anthropic.com/engineering/april-23-postmortem
- **Orchestration economics**: delegation "came out ahead in only two situations": routine work with a long cost tail, and work larger than one context window; for one dependent chain the orchestrator's model alone at lower effort "came out ahead in every case measured on earlier models". Caveats from the page: results are Anthropic-internal and directional; Haiku 5.5 coding subagents under Opus 5.5 `medium` cut cost 32% but lost about 4 points of pass rate, and the page says to compare with lower effort first. VERIFIED (re-fetched in the verification pass): https://platform.claude.com/docs/en/about-claude/models/optimizing-for-cost-and-intelligence
- **Haiku 5.5 bills 5x above 100K-token prompts** ($0.10 vs $0.50 per MTok input). The prompt length counts cache reads and writes, and each request is priced on its own. VERIFIED: https://platform.claude.com/docs/en/about-claude/pricing

### C. v1 failures that a process can prevent (mechanism checked by me, not by v1)

| v1 failure (source) | Cheapest rung that stops it in v2 |
|---|---|
| Implementer declared done without building (v1 memory `implementer-ships-without-building`) | reviewer re-runs `check fast` itself (10 s) |
| Stale or shadow `.pyd`, mtime checks (memory `stale-build-artifacts`) | one build tree, one output path, smoke test prints the loaded module path (design, W8) |
| Stale local main reverted landed work (memory `stale-local-main...`) | `land` script fetches and rebases on `origin/main`, checks branch name |
| Gamma clamp hid energy gain in furnace tests (v1 CLAUDE.md) | test API renders linear by default (design) plus the "canary" rule |
| xfail marker left in after the fix (memory `xfail-gated-features-must-unxfail`) | lint ties each marker to a live task file |
| Concurrent nvcc/GPU runs killed work, PC hard resets (memories) | one machine lock inside `build`/`check` |
| Lanes edited main from a worktree; shared `git stash` (memories) | `isolation: worktree`; deny `Bash(git stash *)` |
| Usage limit killed lanes mid-task (memory) | WIP commit rule plus 10-line Progress block in the task file |
| Features landed switched off (memory `landed-features-off-by-default-audit`) | rule 7 in CLAUDE.md plus a reviewer checklist item |
| Windows false greens: LF `.bat`, `cmd /c`, `write_text` CRLF (memories) | no `.bat`; pwsh 7 and Python only; `.gitattributes` pins line endings (UNVERIFIED that this removes all of them) |
| STATUS 3,484 lines; superseded directives | size lint, derive status, delete superseded records |

## Options and tradeoffs

These are the real forks. Not every one needs the owner; those that do are repeated under "Open questions".

1. **Where a rule is enforced.** Ladder, cheapest and most reliable first: compiler or type error, then test, then `tools/check` script or git hook, then Claude hook or permission rule, then CLAUDE.md prose. Prose is advisory (VERIFIED, memory page) and costs context on every turn and every subagent spawn. A Claude hook costs a process spawn per matching call and runs only in Claude Code. A git hook or script runs for every harness, including the opencode worker. Tradeoff: higher rungs need code, which is itself a thing to maintain.
2. **Where tasks live.** (a) `tasks/NNN-slug.md` in the repo, deleted by the merge commit; (b) GitHub issues; (c) one `TASKS.md`. (a) versions with the branch, is readable in a worktree with no network, and its size is bounded by work in flight; its cost is that the owner has no web UI for it. (b) gives a UI and history but needs `gh` calls and grows without bound. (c) becomes STATUS.md. Idea backlog can stay in issues; only specced work becomes a file.
3. **Auto memory.** (a) Off for the project; durable lessons go into tracked files (path-scoped rules, v1-lessons) that are reviewed in PRs; (b) on, with an instruction to save only `user` and `feedback` types; (c) on, unconstrained as in v1. Memory is unreviewable, machine-local and invisible to other lanes and CI; v1 shows it turning into stale code facts. (b) relies on prose to police a store, which is the failure being avoided. (a) loses unprompted learning from corrections. Owner-level preferences (model routing) are unaffected: they belong in `~/.claude/CLAUDE.md`.
4. **How hard to wall off v1.** (a) Convention only; (b) project `permissions.deny` on `Read(//c/Users/hgcom/OneDrive/Astroray/Astroray_repo/**)`; (c) OS sandbox. (b) is declarative, zero context cost, and blocks Read/Edit/Write plus best-effort Grep/Glob; it is not a boundary against `grep -r` run from inside the path (VERIFIED). (b) also blocks the lead's own lookups, so lookups need a separate session or a temporary rule edit.
5. **Merge authority.** (a) Lead session merges autonomously when check, review and visual look pass; (b) owner approves every merge; (c) owner approves at milestone boundaries only, lead merges in between. v1 used (a) with a checklist reviewer that self-assessed; recon flags auto-merge on self-assessment as a risk (VERIFIED, recon). (a) is fastest; (c) bounds the damage if the reviewer misses something.
6. **Model naming in agent files.** Aliases (`sonnet`, `opus`, `haiku`) keep working across releases and keep prose free of IDs, but move silently (the `haiku` alias changed meaning when Haiku 5.5 became default, REPORTED). Full IDs are stable and need edits each release. Effort is pinned either way.
7. **Parallel lanes.** More lanes shorten wall-clock for independent work but multiply quota burn ("ten agents ... roughly ten times", REPORTED) and v1 saw hard resets under combined build plus test load (memory title, REPORTED). Dependent work should stay on one agent. The GPU and 16 threads are the shared resource.
8. **Open-weight worker.** Keep a thin wrapper for DeepSeek via opencode, or drop it and use Haiku/Sonnet for mechanical work. Owner wants it; v1 evidence says it stalls on multi-file physics and exhausts budget on broad research (memory titles, REPORTED), and that opencode ignores the subprocess cwd so edits land in the main checkout unless `--dir` is passed (v1 delegate SKILL, VERIFIED as v1's claim). It is cheap only if its blast radius is bounded.
9. **One harness or three.** v1 kept Claude, opencode and Codex copies of roles. One harness is simpler; the cost is lock-in and no second opinion model. Owner's stated models are all Claude plus DeepSeek, so Codex is not in the plan.

## Recommendation for Astroray 2.0

Principle: put each rule at the highest rung of the ladder that is cheap; keep always-loaded text to what Claude cannot learn from the repo; make every artifact bounded by construction (open work, one page, accepted decisions) so nothing needs pruning by goodwill.

### R1. CLAUDE.md: critique of the current draft and proposed text

Critique of `C:\Astroray2.0\CLAUDE.md` (50 lines, 2,459 B, about 0.6K tokens, so well inside the guidance):

1. Keep: it is short, has no dated directives, no model IDs, no hooks, and rules 4-8 each map to a v1 failure (section C).
2. "Phase: planning. No engine code until ..." is state, not a rule. It will be false at sign-off and is exactly how v1 rotted. Move to STATUS.md.
3. It has no commands, the first item Anthropic's guidance lists. Add three entry points, in the same commit that creates them (no pointers to things that do not exist; TESTING.md is referenced nowhere yet and ROADMAP/ARCHITECTURE do not exist).
4. Rule 9 carries numbers (10 s, 3 min, 30 s, 5 min) that will also live in the check script: two copies drift. State them once, in the script header, and have CLAUDE.md name the script.
5. Rules 8-10 and the "no two GPU runs" line are things a machine can check. Demote to enforcement (lock, test, lint); keep one line saying the checks exist and must not be raised to pass.
6. Rules 1-3 are generic and models largely follow them unprompted (best-practices page, "what you can stop doing"). v1 reported them as the cause of most rework only in its package-implementer prompt (REPORTED), so keep the two that bite (ask when two readings exist; stay inside the task's file scope) and drop the rest. Not tested; see spike P5.
7. "Ask the lead for a cited lookup" gives an undefined role: a non-fork subagent cannot ask anyone, it can only return. Say "report what v1 fact you need; do not open v1".
8. Missing: the done-definition (what output to show), the escalation path, commit and branch convention, the canary rule for tests, the rule that tests must not hide behind skip/xfail.
9. CLAUDE.md is paid in full by every non-fork subagent (VERIFIED), so lane-irrelevant lines cost per spawn. Use `omitClaudeMd: true` on the read-only `digest` agent.
10. Add HTML block comments naming the v1 incident behind each rule: free (stripped before injection, VERIFIED) and they make later pruning an evidence decision.

Proposed text (about 30 lines; lint cap 100). Commands are placeholders until spike S3 fixes them.

```markdown
# Astroray 2.0
Spectral GPU path tracer for Blender and astrophysical rendering. Correct means
physics, analytic tests and the OpenPBR spec; never another renderer's pixels.

## Working rules
- Commands: `tools/check fast|gate|full`, `tools/build`, `tools/land`. Their
  output is the only evidence that something works. Before saying "done", run
  `check fast` and show its last lines. A failed budget or size check is yours
  to fix, never to raise.
- Your task is `tasks/NNN-*.md`; its acceptance commands define done. Touch only
  the files in its scope. Report unrelated problems; do not fix them.
- Two readings of the task, or a simpler design exists: say so before coding.
- Physics, sampling, numerics: use a cited source (`docs/research/14-*`), put
  `Ref:` and the licence in the code. Never invent an algorithm.
- A formula, material or sampler exists once and compiles for CPU and GPU. Never
  hand-port.
- Unsupported input throws or warns visibly. No silent fallback, no feature
  merged switched off, no process-global mutable state.
- A new test must be shown to fail once against deliberately broken code; put a
  `canary:` line in the PR. No skip or xfail without a live `tasks/NNN` id.
- Look at renders (Read the PNG) as well as numbers; measure positions and shifts
  with numpy, do not judge them by eye.

## Where things are
- `docs/ARCHITECTURE.md`; `docs/decisions/` (accepted only); `docs/STATUS.md`
  (one page, lead edits); `tasks/` (open work only); `docs/v1-lessons/` (the only
  v1 channel: do not read the v1 repo; say which v1 fact you need).
- `.claude/rules/*.md` load when you touch matching files.

## Environment
Windows 11, pwsh 7, RTX 5070 Ti, 8c/16t. Builds and GPU runs go through `tools/`,
which take the machine lock.
```

### R2. Enforcement outside prose: three scripts, one lock

- `tools/check.ps1 <fast|gate|full>` is the single verification entry. It prints at most 20 lines (step, seconds, pass/fail, final `PASS gate 141 s (budget 180 s)`), writes full output to `out/check-<tier>.log`, exits non-zero on failure. Budgets live only in its header. Failure prevented: agents reading thousands of log lines (Anthropic's guidance is to pre-digest output, VERIFIED best-practices page) and budget numbers drifting between docs.
- `fast` (target under 10 s, no GPU) includes the cheap hygiene checks that replace v1 hooks: size caps (below), no dated directives or model IDs in always-loaded files (`20\d\d-\d\d-\d\d`, `claude-`, `deepseek` grep), diag-marker grep over the diff (replaces `pre_commit_diag_check`), every `skip`/`xfail` names a task file that still exists. Run as a git `pre-commit` hook so it also binds the opencode worker and the owner by hand. Failure prevented: leftover debug code (v1 hook), markers that outlive their task, doc growth.
- `gate` (target under 3 min) = `fast` + build + GPU tests. `full` = on demand at milestone close, never scheduled (v1's Windows Task orchestrator is not carried over).
- Machine lock: `build` and `gate`/`full` take one named mutex (for example `Global\astroray2`), so two lanes queue instead of crashing the GPU or the PC (v1 memories, REPORTED). This replaces the CLAUDE.md sentence. Budgets are provisional until the build spike measures them.
- `tools/land.ps1 <task>`: refuse unless on the task branch; `git fetch`; rebase on `origin/main`; run `gate` under the lock; `git rm tasks/<task>.md`; print the evidence block for the PR body. Failure prevented: merging from a stale main, forgetting to delete the task, un-gated merges.
- `tools/status.ps1` prints branches, open PRs, last gate result (sha, seconds) from git and `gh`; nothing is stored. Failure prevented: a hand-written status that lags reality (v1 SessionStart hook did this at 0.3K tokens per session; on demand costs nothing until asked).

### R3. Hooks and permissions

- Committed Claude hooks: zero at the start. Candidate hooks are rejected because a cheaper rung covers them: SessionStart summary (use `status.ps1`), format-on-edit (use the pre-commit hook), pyd/shadow guard (design removes shadows), signature sweep (the compiler fails a stale call site in a typed language; v1's sweep existed because Python tests and bindings hid callers, UNVERIFIED for v2), journal Stop hook (v1: 1,983 of 1,986 lines empty).
- `permissions.deny` (declarative, no context cost): `Bash(git stash *)`, `Bash(git push --force *)` plus the same two as `PowerShell(...)` rules (the PowerShell tool has its own rule family; owner's primary shell is pwsh), and optionally the v1 Read rule from option 4. Bash denies are documented to apply to subagents (VERIFIED, sub-agents page) but match command text only, so `git -C x stash` style variants can slip past: a guard, not a boundary. Failure prevented: v1 stash collisions and force-push accidents. Whether the PowerShell rules bind subagents identically is still UNVERIFIED (spike P4).
- Unattended runs use `/goal` with a checkable condition and a turn clause, for example `/goal tasks/012 acceptance commands pass and check gate is green, or stop after 25 turns`. It is session-scoped, so nothing is committed; the evaluator sees only the transcript, so the condition must name the command (VERIFIED). Failure prevented: a text-only end of turn treated as completion.
- Add a hook only after the same failure happens twice and no cheaper rung can stop it; the PR adding it names both incidents.

### R4. Agents (three files) and one non-Claude worker

| Agent | Model / effort | Tools | Why it exists (failure prevented) |
|---|---|---|---|
| `implementer` | sonnet, `medium` (`high` for physics tasks, via per-invocation effort or a second brief) | default; `isolation: worktree`; `maxTurns` set | Same worker spawned repeatedly (Anthropic's criterion). Body (about 25 lines): first action `git rev-parse --show-toplevel` equals the worktree, else abort (worktree contamination); read task file; commit WIP after each acceptance step (usage-limit kills); never report done without `check fast` tail; stop and report if the spec is ambiguous. |
| `reviewer` | opus, `medium` (`high` for kernel or physics diffs) | `Read, Grep, Glob, Bash`; `disallowedTools: Edit, Write` | Fresh context helps (VERIFIED). Edit/Write are removed by the harness, but Bash can still write files, so this reviewer is "no-edit by tool list", not read-only; Anthropic's enforced-read-only pattern is `tools: Read, Grep, Glob` only. Keep Bash only because it must re-run `check fast`; its prompt forbids writes and the lead checks `git status` after it returns (or use a Read/Grep/Glob-only reviewer and let `land` run the checks). Brief: diff plus task file plus CLAUDE.md; report only (a) acceptance not met, (b) CLAUDE.md rule broken, (c) test that would pass on broken code, including a named deliberate breakage; skip style (VERIFIED reason). |
| `digest` | haiku, `medium`; `omitClaudeMd: true` | `Read, Grep, Glob` | Owner's reading/summarising tier. Returns cited digest of at most ~60 lines; each call under 100K tokens including cache reads (5x billing cliff, VERIFIED). `omitClaudeMd` avoids paying CLAUDE.md per call (VERIFIED field; spike P7). |

- Debugging and hard reasoning use Opus as the main session or as a per-invocation `model` override on a general Agent call (VERIFIED precedence). No standing `debugger` agent: it would be spawned too rarely to earn a file.
- Not carried: architect, strategy-review, dispatch-next, close-round, docs-updater, hardware-verifier (the lead does these; a lane-per-gate agent was machinery for v1's package/round system), pr-reviewer (self-assessed auto-merge), cpp-abi-guard and cycles-parity-reviewer (toolchain- and goal-specific; if ABI or physics review becomes recurring, add a path-scoped rule first).
- Effort is pinned in every file (postmortem, VERIFIED). Top-tier models are not pinned anywhere: per owner, they are the planning lead only. Never switch the main session's model mid-task (VERIFIED cost); change effort freely on 5.5-generation models.
- Concurrency: at most two implementers at once unless the lead has measured the machine under combined load (v1 resets, REPORTED); dependent chains stay on one lane. Fan out only for independent tasks (VERIFIED economics above). Failure prevented: quota burn and thrash.
- Open-weight worker: `tools/delegate.ps1 -Dir <worktree> -PromptFile <file>` wraps `opencode run`, always passes `--dir`, captures `git diff --stat` and the transcript tail, and returns evidence only (v1's contract, the one proven piece). Allowed work: renames, boilerplate bindings, fixtures, formatting, doc edits, lint fixes. Not allowed: kernels, physics, anything whose mistake ships silently. The model ID lives only in opencode config. Whether it earns its keep is spike P6; drop it if success is below the bar.

### R5. Skills (two, both hidden from the model)

- `land` (about 30 lines, `disable-model-invocation: true`, invoked `/land NNN`): runs `tools/land`, spawns `reviewer` with diff and task, requires the lead to open the saved render, checks the canary line, then merges. Failure prevented: skipping review or the visual look under time pressure. It is a skill, not CLAUDE.md text, because it is a procedure that matters only at merge.
- `delegate` (about 25 lines, hidden): when and how to hand work to the open-weight worker, evidence contract, the not-allowed list. Failure prevented: delegating hard work or trusting a "completed" status.
- Hidden skills cost no per-turn listing (VERIFIED), so the always-loaded surface is zero skills.
- Not carried: cite-algorithm (rule 5 plus `docs/research/14-*` and a path-scoped rule `.claude/rules/physics.md` for `src/**` covering how to write a `Ref:` line; loads only when engine code is touched), lint (greenfield has no legacy debt to diff against; add only if debt appears), visual-check (opening the PNG is a Read call), architect, pkg-ship, roadmap-orchestrator, team-overnight, run-report, rebuild-pyd, verify, close-round, dispatch-next, file-followup, strategy-review.
- User-level skills load into v2 too. This session's own skill listing shows several dozen plugin skills; each description costs context every turn (VERIFIED mechanism, count only observed). Measure with `/skill-doctor` and trim user-level plugins for this project before blaming project files (spike P2).

### R6. Path-scoped rules instead of lessons-in-memory

- `.claude/rules/<area>.md` with `paths:` globs, at most 40 lines each, created only when a failure recurs or costs over an hour. Candidates from v1 knowledge: kernels and device code (register pressure, large by-value structs, no global bindings), Blender glue (pixels bottom-up, no id()-keyed caches), tests (linear render, canary, deterministic seed, seed 0 is not "random"), toolchain (MSVC, nvcc serialisation). Which survive depends on ADRs, so write them after sign-off.
- Each rule line carries, in an HTML comment (free), the v1 incident and the v2 guard (test or script) if one exists. A line with a guard and no violation in a milestone is deleted; a prose-only line violated twice is promoted up the ladder.
- Failure prevented: facts that matter for one area load for every task (v1 AGENTS.md loaded 14.7 KB of everything), and memory-resident facts that no one can review.

### R7. Task (spec) format: one file, about 25 lines, read in one go

`tasks/NNN-slug.md`, created by the lead, lint cap 60 lines, deleted by `land`.

```markdown
# NNN Title
Goal: before -> after, two sentences.
Why now: one line, or omit.
Scope: files or dirs that may change. Nothing else.
Out of scope: what a reader might assume is included.
Reference: paper/section, reference file path, licence (required for physics).
Acceptance:
- `tools/check fast` passes
- <exact command> prints <expected value or threshold>
- Look at: out/evidence/NNN/<name>.png and say what it shows
Canary: the deliberate breakage that must turn a new test red.
Budget: expected effect on build/test time (usually "none").
Size: expected diff; stop and report at 2x.
Lane: implementer | delegate; effort hint.
Progress: (lane appends at most 10 lines; WIP commit after each)
```

- Open questions must be empty before dispatch; an unresolved question means the lead is still planning. Failure prevented: Sonnet guessing at design (v1 `implementer-ships-without-building`, wrong-API tests) and v1's 13.7 KB specs.
- "Acceptance" must name commands: Anthropic's `/goal` and reviewer pattern both depend on checks the transcript can show (VERIFIED). "Look at" forces the visual step v1 learned late (flipped ROI survived numeric gates).
- Reference field makes rule 5 checkable at review rather than a mood.
- Lint (inside `check fast`): at most 60 lines, has `Acceptance` with at least one backticked command, no task file older than its branch's merge.

### R8. Verification before merge, without a gate apparatus

1. Implementer runs the task's acceptance commands and `check fast`; PR body carries the output tail and the `canary:` line.
2. `tools/land` rebases, runs `gate` under the lock, prints one PASS/FAIL line with time against budget.
3. `reviewer` (fresh, read-only) gets diff and task; it re-runs `check fast` itself and tries to name a breakage the new test would miss.
4. Visual changes: render saved under `out/evidence/NNN/` (gitignored; v1 hit a `.gitignore` trap with evidence PNGs, REPORTED). The lead opens the image and states what it shows next to the numbers. Vision models invent pixel shifts (v1 memory, REPORTED), so positions are checked with numpy. A cheap vision model such as DeepSeek Flash may pre-screen; it never decides.
5. A numerical failure is investigated on both sides: the code and the test or reference (v1: tests calibrated on a broken engine, two placeholder gate scenes). Fix a wrong test in the same PR.
6. Merge per owner choice in Option 5. Recommended default: lead merges after 1-5; owner is shown a milestone summary and holds a veto on decisions only.
- No gate letters, no per-feature manifests, no extra reviewer roles. Hardware CI is out of scope here (W8); if CI exists it runs exactly `check fast`, so CI and local cannot diverge. CI has no GPU in v1, which is why `gate` runs locally (REPORTED).

### R9. Status and decisions without growth

- `docs/STATUS.md`: at most 50 lines, overwritten, lead-only (lanes never edit it, which also avoids merge conflicts). Contains only what git cannot say: current milestone and its goal, open owner questions, known-red items with task ids. Branches, PRs and last gate come from `tools/status`. History is `git log`; there is no journal, round log or standup.
- Decisions: `docs/decisions/NNN-title.md`, at most 80 lines (context, options, evidence with links, decision, consequences), accepted only. A superseding record deletes the old file in the same commit and says `Supersedes: NNN (git show <sha>:docs/decisions/NNN-...)`. Git is the archive, so a grep of the working tree never returns a dead directive. This amends the current planning rule "a decision changes only by a new record that supersedes the old", which leaves the old file in the tree.
- Research files: after sign-off, tag `planning-final` and delete research files no ADR cites; cite by path at the tag if needed. `00-recon.md` (79 KB) must not stay where agents grep.
- Failure prevented: STATUS.md at 3,484 lines and CLAUDE.md section 5 as dated history.

### R10. Memory

- Recommend Option 3(a): `autoMemoryEnabled: false` in the project's `.claude/settings.json`. Learning from a correction goes where a reviewer can see it: a CLAUDE.md line if it applies everywhere, a path-scoped rule if it applies to an area, a test or check if it can be mechanical (Anthropic's own test: "corrected twice"). Owner preferences (model routing, communication style) stay in `~/.claude/CLAUDE.md`. Re-evaluate after two milestones with the cost of memory visible.
- Audit `~/.claude/CLAUDE.md` and `~/.claude/rules/` for v1 content before the first v2 session: they load into every project (VERIFIED load order). v1 memory itself does not load, because the directory is per git repo (VERIFIED) and C:\Astroray2.0 is a different repo.
- Failure prevented: 144 unreviewable files, stale facts surviving subagents (v1 memory titles), a MEMORY.md near the 25 KB read cap (v1: 13.7 KB, VERIFIED).

### R11. v1 knowledge without contamination

- Channel: `docs/v1-lessons/` only (W12 writes it). Entry format, at most 6 lines each: claim; evidence pointer (v1 path:line or commit sha); still true in v2? (yes / unknown / made moot by ADR NNN); guard (test or check name, or "none"). File cap 150 lines.
- After each ADR, review lessons: a lesson the ADR makes moot (for example fused-kernel register pressure, MinGW miscompiles) is deleted, not annotated. Failure prevented: v1 constraints bleeding into a design that no longer has them.
- Lookup: when a lane needs a v1 fact, it reports the question. The lead runs one `digest` call with explicit v1 paths (Haiku, read-only), then writes the answer back as a new lesson with its citation, so the same question is never asked twice. If the owner picks the hard wall (Option 4b), the lookup runs from a session rooted in v1 or with the rule lifted for that one call. `--bare` is not usable for a lookup process without an API key (VERIFIED).
- Code is never copied from v1 by an agent that has read it; ported items (Jakob-Hanika tables, CIE data, the Dormand-Prince integrator design) are named in tasks with the v1 path and copied by the lead or by script. Failure prevented: by-value struct hazards and RGB-era patterns coming along with the copy.
- Do not create AGENTS.md or `.codex/` or `.agents/` in v2. Claude Code ignores AGENTS.md when CLAUDE.md exists (VERIFIED), so a second file could only diverge.

### R12. Rules that keep the process from accreting

All checked by `check fast` unless noted. Caps are initial and adjustable only by the owner.

| Rule | Mechanism | Failure prevented |
|---|---|---|
| CLAUDE.md at most 100 lines; each `.claude/rules/*.md` at most 40; agent file at most 60; skill at most 80; STATUS at most 50; task at most 60; ADR at most 80; v1-lessons file at most 150 | line-count lint | STATUS 3,484 lines; CLAUDE.md 11.8 KB |
| At most 3 agent files, 2 skill files, 0 committed Claude hooks, no `.codex`/`.opencode`/`.agents` directories | count lint | three copies of roles, 15 skills, 8 hooks |
| No dates or model IDs in CLAUDE.md or rules; edit a rule in place, never append "(updated ...)" | grep lint | section-5-style dated directive pile |
| Adding a rule, hook, agent or skill needs the incident (PR or commit) in the PR body, and if the file is at cap, a deletion | reviewer checklist | additions with no failure behind them |
| Every prose rule is tagged with its guard in an HTML comment; at each milestone close, run `/doctor prompt-audit`, delete prose rules with no incident, promote rules violated twice | milestone ritual, 30 minutes, lead | rules that outlive their cause; contradictions (VERIFIED: Claude "may pick one arbitrarily") |
| `tasks/` holds open work only; `land` deletes the task; lint fails a task whose branch merged | script plus lint | spec corpus (334 files) |
| New scripts live under `tools/`, listed in a 15-line `tools/README` only if reusable; one-off scripts are deleted when the task closes | reviewer checklist | five parallel contact-sheet generators (v1 CLAUDE.md 5b) |
| Process changes (anything under `.claude/`, `tools/`, CLAUDE.md) go to main as their own small commit, never inside a feature diff | reviewer checklist | tooling mixed into physics diffs |

### R13. Settings

`.claude/settings.json` (project): `autoMemoryEnabled: false`; the two permission denies; `subagentPromptCacheTtl: "1h"` if lanes idle 5-60 minutes behind builds (VERIFIED: setting exists since v2.1.242; 1h writes cost more than 5m writes, so it only pays when lanes really idle). Main-conversation 1h is automatic only on a subscription within plan usage; on an API key set `promptCacheTtl: "1h"` too (VERIFIED). Which applies to the owner is settled by P8. `idleCompaction` left at default. No `env` for experimental agent teams: v1 recon found repo-wide `CLAUDE_CODE_EXPERIMENTAL_AGENT_TEAMS=1` turns named subagents into teammates (about 7x tokens, REPORTED); v2 does not enable teams.

## What a measurement spike must settle

Process spikes are small and cheap; run them before sign-off.

- **P1 Context floor.** In an empty v2 repo holding the proposed CLAUDE.md, settings and three agents, record input tokens of the first request (`claude -p "hello" --output-format json`, `usage`). Target: project-controlled text at most ~1K tokens. Report the same number with user-level plugins disabled, so the two sources are separable.
- **P2 User-level leakage.** List what loads from `~/.claude` (CLAUDE.md, rules, plugin skills) with an `InstructionsLoaded` log hook and `/skill-doctor`; trim until the floor is acceptable.
- **P3 Check budgets.** After the build spike (S3): is `check fast` under 10 s and `gate` under 3 min on real code? If not, the budget or the test tiers change, not the rule.
- **P4 Deny-rule coverage.** Does `Read(//c/Users/hgcom/OneDrive/Astroray/Astroray_repo/**)` stop Read, Grep, Glob, PowerShell `Get-Content`/`Select-String` and a subagent? Does `Bash(git stash *)` deny reach subagents? Documented as best-effort, so measure.
- **P5 Lane dry run.** One toy task (triangle to image) through `implementer`, `reviewer`, `land`. Record turns, tokens, wall-clock, whether `check fast` was run, and whether the reviewer catches three planted defects: a silent fallback, a hand-ported duplicate, a test that cannot fail. Run a variant with rules 1-3 removed from CLAUDE.md to see if behaviour changes.
- **P6 Delegate viability.** Three mechanical tasks on the open-weight worker through `delegate.ps1`: success rate, stalls, whether `--dir` containment holds. Define the bar before running (for example, at least 2 of 3 mergeable with no unscoped edits).
- **P7 `omitClaudeMd`.** Confirm the `digest` agent starts without CLAUDE.md on the installed Claude Code version.
- **P8 Cache TTL.** Confirm `usage.cache_creation.ephemeral_1h_input_tokens` appears for main and subagent requests under the owner's actual auth.
- **P9 Lint noise.** Run the size and grep lints over the planning docs; false-positive rate must be near zero or the lints are removed.
- **P10 Concurrency.** Two implementers plus a build under the machine lock; confirm the PC stays stable (v1: hard resets).

## Unverified or conflicting

- v1 token estimates (4.5K / 10-14K / 90K+) are chars/4 heuristics from recon, not measured; the v2 floor must be measured (P1).
- (Resolved in the verification pass: orchestrator economics and Haiku 5.5 pricing are now VERIFIED against primary pages.)
- Bash deny rules apply to subagents (documented); whether PowerShell rules do, and whether `Bash(...)` denies cover the PowerShell tool, is not documented; P4.
- `--max-turns`/`maxTurns`, per-invocation `effort`, and exact agent field behaviour were read from docs but not exercised on the installed version.
- The claim that models need rules 1-3 less than in v1 is Anthropic's general statement (best-practices "what you can stop doing"), not a measurement on this project; P5 tests it.
- DeepSeek V4.1 Flash behaviour (stalls, vision, budget exhaustion) is known only through v1 notes and memory titles; no primary source. `--dir` containment is v1's claim about opencode, not re-tested.
- The v1 memory titles used as evidence (e.g. `pc-hard-resets-under-combined-load`, `implementer-ships-without-building`) were read as index lines in MEMORY.md, not as full files.
- Whether `.gitattributes` plus pwsh 7 removes all v1 Windows false greens is untested.
- Whether a machine mutex is enough to prevent the PC hard resets is unknown; cause (power, thermal, driver) not established in the sources.
- Conflict: the planning rule "a decision changes only by a new record that supersedes the old" (CLAUDE.md, PLANNING_PLAN) versus R9 (delete the old record). The owner should pick; R9 follows the owner's complaint about accumulating superseded directives.
- Conflict: v1 doctrine "autonomous merge once gates and independent reviews pass" versus recon's flag that review was self-assessed. Merge authority is left to the owner (Option 5).

## Verification pass

Independent re-check on 2026-11-14 against primary pages (all fetched in this pass). Recommendation is unchanged; three statements were corrected (reviewer "read-only", deny coverage of the PowerShell tool, main-conversation 1h TTL being auth-dependent).

| # | Claim | Result | Source |
|---|---|---|---|
| 1 | CLAUDE.md is context, not enforced; blocking needs a PreToolUse hook; "target under 200 lines"; subagents get the CLAUDE.md hierarchy and git status; `omitClaudeMd` (v2.1.271+) skips user/project/local files but managed policy still loads | CONFIRMED | https://code.claude.com/docs/en/memory ; https://code.claude.com/docs/en/sub-agents |
| 2 | Path-scoped rules load on Read/Write/Edit (or single-file `cat`/`head`) of a matching file; unscoped rules load at launch | CONFIRMED | https://code.claude.com/docs/en/memory |
| 2b | `disable-model-invocation: true` removes the skill from context entirely; listed skills cost context every turn; 1,536-char description cap; SKILL.md under 500 lines; `/skill-doctor` v2.1.252+ | CONFIRMED | https://code.claude.com/docs/en/skills |
| 3 | Fresh reviewer helps, same-task delegation does not; `tools: Read, Grep, Glob` is a harness-enforced limit; "skip style" because a reviewer "will always find something" | CONFIRMED for the quotes. CORRECTED for the design: the proposed reviewer includes Bash, which can write, so it is not read-only (fixed in R4). | https://code.claude.com/docs/en/best-practices |
| 4 | `--bare -p` skips CLAUDE.md, hooks, skills, subagents, plugins, MCP, auto memory; never reads OAuth or keychain; needs `ANTHROPIC_API_KEY` or `apiKeyHelper` | CONFIRMED. Added: cloud-provider creds still work; bare will become the `-p` default. That v1 has no API key stays REPORTED. | https://code.claude.com/docs/en/headless |
| 5a | Effort changes keep the cache on Fable 5.1+, Opus 5.5, Sonnet 5.5, Haiku 5.5 with an API key or subscription (not Bedrock/Vertex/gateway/`CLAUDE_CODE_DISABLE_EXPERIMENTAL_BETAS`); model switch recomputes everything | CONFIRMED. v1's "never switch effort" is obsolete on these models; "never switch model" still holds. | https://code.claude.com/docs/en/prompt-caching |
| 5b | Subagents get 5 minutes unless `subagentPromptCacheTtl` set | CONFIRMED. CORRECTED/refined: the main conversation gets 1h only on a subscription within plan usage; with an API key it is 5 minutes unless `promptCacheTtl` is set. Per-agent `experimental.cacheTtl` (v2.1.248+) also exists. | https://code.claude.com/docs/en/prompt-caching ; https://code.claude.com/docs/en/sub-agents |
| 5c | Anthropic's Mar-Apr 2026 regression included a Claude Code default-effort change high to medium (Mar 4, reverted Apr 7) | CONFIRMED (default afterwards xhigh for Opus 4.7, high for others) | https://www.anthropic.com/engineering/april-23-postmortem |
| 6 | AGENTS.md ignored when a CLAUDE.md exists in or above the cwd | CONFIRMED (v2.1.277+; `.claude/CLAUDE.md` and `CLAUDE.local.md` also count) | https://code.claude.com/docs/en/memory |
| 7 | Block-level HTML comments stripped before injection; `/doctor prompt-audit` v2.1.283+; `autoMemoryEnabled`, `autoMemoryDirectory`; MEMORY.md first 200 lines or 25KB; one memory dir per git repo | CONFIRMED | https://code.claude.com/docs/en/memory |
| 8 | Deny first, `Read(//c/...)` Windows form, Read deny is best-effort for Grep/Glob, Bash rules "not a security boundary" | CONFIRMED. CORRECTED: PowerShell has separate `PowerShell(...)` rules; add them next to the Bash denies. Bash deny applying to subagents is documented. | https://code.claude.com/docs/en/permissions ; https://code.claude.com/docs/en/sub-agents |
| 9 | `/goal` is a session-scoped prompt-based Stop hook; evaluator (small fast model) judges the transcript only; works with `-p`; turn clause supported | CONFIRMED. Note: unavailable if `disableAllHooks` is true; evaluator is a small model, so conditions must name a command whose output shows in the transcript. | https://code.claude.com/docs/en/goal |
| 10 | Hooks: exit 2 blocks, other non-zero proceeds unless `onFailure: "block"`; `SessionStart` matcher `compact` re-injects context | CONFIRMED | https://code.claude.com/docs/en/hooks-guide |
| 11 | Orchestrator economics (was REPORTED) | CONFIRMED, with caveats now in section B (Anthropic-internal, directional; Haiku coding subagents lost ~4 pts pass rate) | https://platform.claude.com/docs/en/about-claude/models/optimizing-for-cost-and-intelligence |
| 12 | Haiku 5.5 5x above 100K prompt tokens (was REPORTED) | CONFIRMED; cache reads count toward the 100K | https://platform.claude.com/docs/en/about-claude/pricing |
| 13 | `isolation: worktree`, `effort`, `maxTurns`, `disallowedTools`, model resolution order, 15,000-token description warning | CONFIRMED | https://code.claude.com/docs/en/sub-agents |
| 14 | v1 token floor estimates, v1 memory/incident titles, DeepSeek behaviour, `opencode --dir` containment, hard-reset cause, `.gitattributes` removing all Windows false greens, "ten agents ~10x" quota | COULD NOT VERIFY (local v1 facts or no primary source; remain REPORTED/UNVERIFIED) | none |
| 15 | `haiku` alias changed meaning when Haiku 5.5 became default | COULD NOT VERIFY (prompt-caching page only shows aliases resolving to current models, e.g. `sonnet` to `claude-sonnet-5-5`); stays REPORTED | https://code.claude.com/docs/en/prompt-caching |

Recommendation after verification: unchanged. Enforce rules at the highest cheap rung (scripts, git pre-commit, deny rules; zero committed Claude hooks), a roughly 30-line CLAUDE.md, three agents with pinned effort, two hidden skills, path-scoped rules, auto memory off, bounded STATUS/tasks/ADRs, and v1 reachable only through `docs/v1-lessons/`. Adjustments: the reviewer is no-edit rather than read-only (or drop Bash and let `land` run checks); write PowerShell deny rules alongside Bash ones; set `promptCacheTtl` as well if the owner authenticates by API key.
