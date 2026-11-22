---
name: land
description: Land one finished task: prepare, review, look, comment, merge. Lead only.
disable-model-invocation: true
---

# land NNN

Run from the lead session. `NNN` is the task number. Every step prints its own
result; stop at the first line that starts with `REFUSE` or `FAIL` and report it.

1. Go to the task worktree (`git worktree list` shows it) and run
   `pwsh tools/land.ps1 prepare NNN`. It checks the branch and tree, rebases on
   `origin/main`, runs the canary patch (every item must turn red), writes
   `out/land/NNN/task.md` and `out/land/NNN/diff.patch`, commits the task-file
   deletion, pushes, runs the gate and posts `astroray/gate` on the pushed sha.
2. If it prints `no PR yet`, open one: `gh pr create --base main --fill`, and name
   the incident for any new rule, agent, skill or script in the PR body (0009 d31).
3. Review (owner decision O31): kernel, physics, sampling, ABI, concurrency and
   process diffs, and any diff that edits a record, need the reviewer. Spawn the
   `reviewer` agent on `out/land/NNN/` (`task.md`, `diff.patch`, `canary.patch`
   when present). Its last line is `VERDICT: PASS`, `PASS WITH FIXES` or `FAIL`.
   FAIL: findings back to the implementer, do not merge, review again. PASS WITH
   FIXES: the implementer fixes, you read the fix diff, then go on. For any other
   diff skip the reviewer.
4. If the task has `Look at:` other than `none`, open every evidence PNG with
   Read and write what you saw in a PR comment starting with `evidence:`.
5. Comment on the PR as the lead, one line each, with the gated sha from
   `out/land/NNN/gate.json`:
   - reviewed diff: `REVIEW PASS <sha>`
   - diff that O31 exempts from review: `REVIEW SKIP <sha> (O31)`
6. Wait for `ci` (hosted) to finish: `pwsh tools/status.ps1` shows `ci` and `gate`
   per PR.
7. `pwsh tools/land.ps1 merge NNN`. It requires the PR head to equal the gated
   sha, `ci` and `astroray/gate` success, the lead comment from step 5 and the
   `evidence:` comment from step 4, then squash-merges, confirms the squash
   commit is on `origin/main`, and deletes the branch, the worktree and its `out/`.
   If it prints `re-run prepare`, main moved: go back to step 1.
8. Paste the PR link into `docs/STATUS.md` when the task is a Verification row.

`-DryRun` on either command prints what would happen and changes nothing.
