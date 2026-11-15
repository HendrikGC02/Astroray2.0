#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# usage: tools/lead/review-bundle.sh NNN branch  -- writes out/land/NNN/{task.md,diff.patch} for the reviewer agent.
cd /c/Astroray2.0; n=$1; b=$2
mkdir -p out/land/$n
git show $b:$(git ls-tree -r --name-only $b tasks | grep "^tasks/$n-.*\.md$") > out/land/$n/task.md
git diff main...$b -- . ":!tasks/$n-*.md" > out/land/$n/diff.patch
wc -l out/land/$n/diff.patch
