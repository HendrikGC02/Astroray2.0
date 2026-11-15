#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# usage: tools/lead/land-pr.sh NNN PASS|SKIP "title"  -- the whole land.ps1 flow for task NNN: prepare, open the PR,
# post the REVIEW verdict, wait for checks, merge; retries when main moved ("re-run prepare"). Lead only.
n=$1; verdict=$2; title=$3
: "${ASTRORAY_BLENDER:=$(pwsh -NoProfile -Command "[Environment]::GetEnvironmentVariable('ASTRORAY_BLENDER','User')" | tr -d '\r')}"
export ASTRORAY_BLENDER
export ASTRORAY_DEPS_CACHE="${ASTRORAY_DEPS_CACHE:-C:\Astroray2.0\third_party\_cache}"
cd /c/Astroray2.0
wt=$(git worktree list --porcelain | awk -v b="refs/heads/task/$n-" '/^worktree /{w=$2} /^branch /{if (index($2,b)==1) print w}' | head -1)
br=$(git worktree list --porcelain | awk -v b="refs/heads/task/$n-" '/^branch /{if (index($2,b)==1) {sub("refs/heads/","",$2); print $2}}' | head -1)
[ -z "$wt" ] && { echo "no worktree for task $n"; exit 1; }
for attempt in 1 2 3; do
  (cd "$wt" && timeout 1500 pwsh -NoProfile -File /c/Astroray2.0/tools/land.ps1 prepare $n 2>&1 | tail -3) | tee /tmp/prep_$n.txt
  grep -q "^PASS prepare" /tmp/prep_$n.txt || { echo "prepare failed"; exit 1; }
  sha=$(git -C "$wt" rev-parse HEAD)
  pr=$(gh pr list --head "$br" --state open --json number -q '.[0].number')
  if [ -z "$pr" ]; then
    pr=$(gh pr create --base main --head "$br" --title "$n: $title" --body "Task $n. Landed through tools/land.ps1.

🤖 Generated with [Claude Code](https://claude.com/claude-code)" | grep -o '[0-9]*$')
  fi
  if [ "$verdict" = PASS ]; then gh pr comment $pr --body "REVIEW PASS $sha" >/dev/null; else gh pr comment $pr --body "REVIEW SKIP $sha (O31)" >/dev/null; fi
  sleep 15; timeout 1200 gh pr checks $pr --watch --interval 20 >/dev/null 2>&1
  out=$(timeout 600 pwsh -NoProfile -File tools/land.ps1 merge $n 2>&1 | tail -5); echo "$out"
  echo "$out" | grep -q "re-run prepare" || break
done
git pull -q --ff-only 2>/dev/null; git log --oneline -1
