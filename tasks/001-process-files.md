# 001 Process files for the build phase
Goal: CLAUDE.md still says "Phase: planning" and no agent roles exist -> CLAUDE.md, settings, STATUS and the three agent files match 0009, so every later implementer reads build-phase rules.
Read first: 0009 Appendix A (proposed CLAUDE.md and settings), 0009 d27 (roles), d29 (STATUS), d31 (caps); docs/ROADMAP.md "Open items".
Scope: CLAUDE.md, .claude/settings.json, .claude/agents/implementer.md, reviewer.md, digest.md, docs/STATUS.md.
Out of scope: .claude/settings.local.json (git-ignored; the lead adds the v1 deny on this PC); the land skill (task 010); anything under tools/.
Reference: none (process text).
Do:
1. Replace CLAUDE.md with Appendix A's proposed text, <= 60 lines, no dates, no model ids. Add to "Where things are": `tasks/README.md` (order of open tasks). Add one line: scripts under `tools/` appear task by task; until `tools/check.ps1` exists, a task's own acceptance commands are the evidence.
2. Write .claude/settings.json exactly as Appendix A's committed JSON (no `hooks`, `autoMemoryEnabled: false`, no `subagentPromptCacheTtl`).
3. Agent files, each <= 60 lines, front matter `name`, `description`, `model` (alias), `effort`, `tools`:
   - implementer: model sonnet, effort medium, default tools, `isolation: worktree`, `maxTurns: 150`. Body: read CLAUDE.md, docs/ARCHITECTURE.md, the task file and its "Read first" only; change only files in Scope; WIP commit after each acceptance step; write tasks/NNN.canary.patch for every new or changed test; finish by pasting the tail of `tools/check.ps1 fast` and of every acceptance command; report unrelated problems, never fix them; stop and report at 2x the task's Size.
   - reviewer: model opus, effort medium, tools Read, Grep, Glob. Body: inputs are out/land/NNN/task.md and out/land/NNN/diff.patch only; report only acceptance unmet, a CLAUDE.md rule broken, a test that would pass on broken code (name the breakage), an algorithm without `Ref:`; no style comments; last line `VERDICT: PASS` or `VERDICT: FAIL`.
   - digest: model haiku, effort medium, `omitClaudeMd: true`, tools Read, Grep, Glob; returns cited digests <= 60 lines; refuses inputs above about 100K tokens.
4. docs/STATUS.md (<= 50 lines): milestone M1a and its goal (one sentence from the roadmap); open owner questions needed before or during M1a (from ROADMAP Open items); red items: none; budget headroom: not measured yet.
Acceptance: `pwsh -c "(Get-Content CLAUDE.md).Count"` prints a number <= 60; `pwsh -c "Select-String -Path CLAUDE.md,.claude/agents/*.md -Pattern 'Phase: planning','claude-[a-z]+-[0-9]','20[0-9][0-9]-[0-9][0-9]-'"` prints nothing; `pwsh -c "(Get-Content .claude/settings.json | ConvertFrom-Json).PSObject.Properties.Name"` lists no `hooks`; each agent file and STATUS is within its cap (`(Get-Content <file>).Count`).
Look at: none.
Canary: none: no test added or changed.
Budget: none.
Depends on: none.
Size: S (about 200 lines of text); stop and report at 2x.
Progress:
