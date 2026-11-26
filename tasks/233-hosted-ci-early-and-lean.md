# 233 Hosted ci runs beside the landing gate, and `check ci` keeps only what this PC cannot catch
Goal: `land submit` opens the PR only after canary, gate and perf, so hosted ci (6-10 min, `ci_wait_s` 410-625 s in out/land/history.jsonl) starts after them and every landing pays both in series. `check ci` also repeats the CPU gate part every landing gate runs here (owner decision 42) -> submit pushes the landing branch and opens or reuses the PR right after the deletion commit, before the canary, so ci runs beside canary, gate and perf. `check ci` runs the clean-checkout `cpu` preset build, the fast tier and the headless `bpy` suite, and no longer the CPU gate steps. `ci` and `astroray/gate` stay required on the same sha. [lane tools; Sonnet; Opus review (process)]
Read first: owner decision 42; 0009 d12 (`ci`), d24, d25, d26 and row B-11; tools/land.ps1 (Invoke-Prepare: order of rebase, canary, deletion commit, push at the `push --force-with-lease`, gate; Invoke-Submit and Wait-Ci; the undo paths Undo-OwnLandCommit and Restore-OriginLand); tools/check.ps1 (every `$isCi` branch and the steps tagged `cpu`); .github/workflows/ci.yml.
Scope: tools/land.ps1 (push and PR before the canary; a failure after the push still undoes with a pushed revert, never a force push; the ci wait starts from the push), tools/check.ps1 (`ci` tier steps), tests/tools/fast_landing.ps1 (the DryRun plan order), docs/decisions/0009-build-test-process.md d12, d26 and B-11 (write-back of decision 42's text; B-11's limit unchanged, its basis notes the leaner tier), tasks/233.canary.patch.
Out of scope: branch protection settings (required checks stay as they are); a self-hosted runner; what the local gate runs; merging before ci is green.
Reference: 0009 d25-d26; GitHub Actions `pull_request` trigger (ci runs on the PR's head sha; a later push to the branch starts a new run).
Do:
1. Reorder prepare inside submit: rebase, deletion commit, push, open or reuse the PR, then canary, Blender suites, gate, perf, status. A canary or gate failure reverts as today; the PR stays open.
2. Wait-Ci waits only for what is left: record `ci_wait_s` as the time after the gate ends, so history shows the saving.
3. `check ci`: drop the gate-part steps; keep the steps that need a clean checkout or the hosted image. State the old and new hosted job time from two runs each.
4. DryRun fixture: the printed plan shows push and PR before canary.
Acceptance: `pwsh tools/check.ps1 fast` passes; `pwsh tests/tools/fast_landing.ps1` prints PASS; two hosted ci runs of the new tier on this branch, with their job times; `pwsh tools/check.ps1 gate` passes.
Look at: none
Canary: tasks/233.canary.patch must turn red: `pwsh -NoProfile -File tests/tools/fast_landing.ps1` (the patch moves the push and PR back after the gate).
Budget: gate +0; hosted ci job time down (state it); landing wall time down by the canary, gate and perf seconds that now overlap ci.
Depends on: none. Collisions: tools/check.ps1 with 231, 232, 234; the later one rebases.
Size: S (about 120 lines without tests); stop and report at 2x.
Progress:
