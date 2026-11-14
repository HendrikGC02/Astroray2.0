# 002 Repository layout, licences and the spikes-final tag
Goal: spike code sits on main and no directory carries a licence -> spike code is kept at tag `spikes-final` and removed from main, each spike RESULTS.md lives under docs/spikes/, and every licensed directory has its LICENSE.
Read first: 0009 d1 (layout, spikes-final), 0009 d21 (asset rules), 0001 D7.1 (licence by directory); .gitignore; .gitattributes.
Scope: spikes/ (removed), docs/spikes/, include/LICENSE, src/LICENSE, cli/LICENSE, tools/LICENSE, tests/LICENSE, data/LICENSE, blender/LICENSE, .gitignore, link paths under docs/ that point at spike RESULTS files.
Out of scope: any other directory (0009 d1: a directory exists only when a target, test or process needs it); editing record text beyond link paths.
Reference: none.
Do:
1. Lead step first: `git tag spikes-final` on the current main commit, then `git push origin spikes-final`. Later tasks read spike code with `git show spikes-final:<path>`.
2. `git mv spikes/A-kernel-budget/RESULTS.md docs/spikes/A-kernel-budget/RESULTS.md`, the same for `B-blender-end-to-end`; `git rm -r` everything else under spikes/.
3. Rewrite every Markdown link under docs/ whose target is a spike RESULTS.md so it resolves to the new path (from docs/decisions and docs/research the old `../../spikes/X/RESULTS.md` becomes `../spikes/X/RESULTS.md`; from docs/ itself `spikes/X/RESULTS.md`). Mentions of other spike files stay as text.
4. LICENSE files: the unmodified Apache-2.0 text from https://www.apache.org/licenses/LICENSE-2.0.txt in include/, src/, cli/, tools/, tests/, data/; the unmodified GPL-3.0 text from https://www.gnu.org/licenses/gpl-3.0.txt in blender/.
5. .gitignore becomes: out/, third_party/_cache/, assets/_cache/, __pycache__/, *.pyc, .vs/, .vscode/, .claude/settings.local.json (drop the spike, build/ and runs/ entries).
Acceptance: `git tag -l spikes-final` prints `spikes-final`; `git ls-files spikes` prints nothing; `git ls-files docs/spikes` lists the two RESULTS.md files; `git grep -n "\.\./\.\./spikes/" -- docs` prints nothing; `pwsh -c "(Get-Content src/LICENSE).Count; (Get-Content blender/LICENSE).Count"` prints 202 and 674.
Look at: none.
Canary: none: no test added or changed.
Budget: none.
Depends on: 001.
Size: S (moves and licence text); stop and report at 2x.
Progress:
