# 010 `tools/land.ps1`, `tools/status.ps1` and the land skill
Goal: tasks are landed by hand with no canary check and no posted GPU gate -> `land.ps1 prepare|merge NNN` scripts 0009 d26 end to end (canary verification, reviewer inputs, gate status), `status.ps1` prints live state, and the hidden `land` skill drives the sequence.
Read first: 0009 d26 (landing), d25 (status posting), d28 (task and canary format), d29 (status from scripts), d31 (skill cap); tasks/README.md.
Scope: tools/land.ps1, tools/status.ps1, .claude/skills/land/SKILL.md, tools/README.md, tests/tools/land_fixture/ (fixture repo builder script).
Out of scope: `check gate` contents (030; until then `prepare` runs `check fast` and posts its result under context `astroray/gate` with description prefix `fast-only`).
Reference: none.
Do:
1. `land.ps1 prepare NNN` (run in the task worktree): (1) refuse unless the branch is `task/NNN-*` and the tree is clean; (2) `git fetch` and rebase on origin/main; (3) canary: parse the `Canary:` line after `must turn red:`: parenthesised text is ignored; items are separated by `, ` or ` and `; a backticked span is a command that must exit non-zero; a bare out/bin executable name or a path ending `.py` starts a test item and the quoted names after it belong to it, so mixed lines (018, 054, 072) are valid; an executable runs `<exe> --tier=all -tc="<name>"` and must report at least one matching case (none: refuse `canary names no test`); tests/abi files run through `<python> -m unittest`, tests/blender files through Invoke-ArBlender (`-Gui` for `gui_*` files, a switch task 085 adds); `git apply` the patch, rebuild, require every item to fail, then `git apply -R`; refuse a diff that adds or changes files under tests/ or `TEST_CASE` lines anywhere without that patch; (4) write out/land/NNN/task.md (the task file as it stands) and out/land/NNN/diff.patch (diff against origin/main); (5) commit the deletion of tasks/NNN*, push, run `check gate` (or `fast`, see Out of scope), post status `astroray/gate` on the pushed sha with `gh api repos/{owner}/{repo}/statuses/<sha>` (state, context, description = summary line).
2. `land.ps1 merge NNN`: require PR head == gated sha, `ci` and `astroray/gate` success, a PR comment by the lead `REVIEW PASS <sha>`, and an `evidence:` comment when the task has a non-`none` `Look at:`; then `gh pr merge --squash`, confirm the squash commit is on origin/main, delete branch, worktree and its out/. If main moved, print "re-run prepare" and stop.
3. `-DryRun` switch: no push, no gh calls; prints what would happen. Used by the fixture test.
4. tools/status.ps1: task branches, open PRs with their `ci`/`astroray/gate` states, worktrees, last out/check/*.json summaries; at most 20 lines.
5. .claude/skills/land/SKILL.md (<= 80 lines, `disable-model-invocation: true`): prepare -> spawn reviewer on out/land/NNN/ -> open evidence PNGs -> comment `REVIEW PASS <sha>` -> merge.
6. tests/tools/land_fixture/make_fixture.ps1 builds a throwaway local repo with a task file, a test change and (a) an inert canary, (b) a real canary, (c) no canary, (d) a real canary on a mixed line naming a backticked command and a doctest case in suite `gate`.
Acceptance: with the fixture, `land.ps1 prepare 999 -DryRun` refuses case (a) printing `canary inert: <test> still passes`, refuses case (c) printing `missing canary patch`, accepts cases (b) and (d); the first real task landed after this one shows `ci` and `astroray/gate` on its PR and its squash commit on origin/main (0009 Verification rows 24-26; lead pastes the PR link into STATUS).
Look at: none.
Canary: none: the fixture cases are the test and are run by acceptance (a land script cannot canary itself).
Budget: none on tiers.
Depends on: 006, 009.
Size: M (about 400 lines PowerShell); stop and report at 2x.
Progress:
