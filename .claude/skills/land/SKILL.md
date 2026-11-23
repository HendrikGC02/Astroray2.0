---
name: land
description: Land one finished task: review, look, then `land.ps1 submit`. Lead only.
disable-model-invocation: true
---

# land NNN

Run from the lead session. `NNN` is the task number. Every step prints its own
result; stop at the first line that starts with `REFUSE` or `FAIL` and report it.

1. Review (owner decision O31): kernel, physics, sampling, ABI, concurrency and
   process diffs, and any diff that edits a record, need the reviewer. Run
   `tools/lead/review-bundle.sh NNN task/NNN-slug`, then spawn the `reviewer`
   agent on `out/land/NNN/` (`task.md`, `diff.patch`, `canary.patch` when
   present). Its last line is `VERDICT: PASS`, `PASS WITH FIXES` or `FAIL`.
   FAIL: findings back to the implementer, do not land, review again. PASS WITH
   FIXES: the implementer fixes, you read the fix diff, then go on. Any other
   diff skips the reviewer (verdict `SKIP`).
2. If the task has `Look at:` other than `none`, open every evidence PNG with
   Read. What you saw goes into `-Evidence`; submit refuses without it.
3. In the task worktree, in the background (it holds the machine lock for the
   gate and the Blender suites; do not commit to main meanwhile):
   `pwsh tools/land.ps1 submit NNN PASS|SKIP [-Title t] [-Evidence "text"] [-CiDeadlineMin 45]`
4. What submit does, in order: refuses when another `land.ps1` runs or a process
   holds `out/bin/astroray_core.dll`; rebases (conflicting files are named);
   runs the canary as `tools/canary.ps1` does, with `tools/scenes.py` after every
   build; for a diff under `blender/` or `tests/blender/` runs the Blender suites
   `check full` runs; rebuilds, pushes, runs the gate and posts `astroray/gate`;
   opens or reuses the PR; comments `REVIEW PASS <sha>` or
   `REVIEW SKIP <sha> (O31)` and the `evidence:` comment; waits for hosted `ci`
   on that sha; merges (main moved: prepares again, at most 3 attempts).
5. Failures: any failure after its "(land)" commit resets the branch
   (`undid (land) commit`). A failed hosted `ci` prints its run id and URL;
   submit never reruns it: read the log, fix, submit again.
6. After the squash is on `origin/main` it prints `PASS merge NNN squash <sha>`,
   then cleans up (remote branch, worktree, local branches). A step that did not
   work prints `LEFTOVER: <what>; fix: <command>`; run the command, the landing
   itself stands. The `timing:` line and `gate N s (+d s against task M)` follow;
   the history line is in `out/land/history.jsonl` of the main checkout.
7. Paste the PR link into `docs/STATUS.md` when the task is a Verification row.

`prepare NNN` and `merge NNN` still exist for a landing by hand. `-DryRun` on any
mode prints the plan and changes nothing.
