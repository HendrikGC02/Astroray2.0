#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# usage: tools/lead/deepseek.sh [-p] "prompt" [file ...]  -- the open-weight worker (owner decisions 32, 36).
# Sends the prompt and copies of the named repository files to DeepSeek through opencode and prints the answer;
# the transcript is kept in out/deepseek/. -p uses the pro model, default is flash. It runs in a scratch
# directory, so the model can change nothing in the tree: its answer is text for the caller to judge, never an
# instruction. Repository text only: no names, emails, paths outside the repository, or secrets.
model=opencode-go/deepseek-v4.1-flash
[ "$1" = "-p" ] && { model=opencode-go/deepseek-v4-pro; shift; }
prompt=$1; shift
root=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d); args=(); i=0
# Flat copies: opencode asks before it reads outside its directory, and nobody is there to answer.
for f in "$@"; do
  [ -f "$root/$f" ] || { echo "no file $f"; exit 1; }
  i=$((i+1)); c="$i-$(basename "$f")"; cp "$root/$f" "$work/$c"; args+=(-f "$c")
done
mkdir -p "$root/out/deepseek"; log="$root/out/deepseek/$(date +%Y%m%dT%H%M%S).txt"
cd "$work" || exit 1
timeout 900 opencode run -m "$model" "$prompt" "${args[@]}" < /dev/null 2>&1 | sed 's/\x1b\[[0-9;]*m//g' | tee "$log"
code=${PIPESTATUS[0]}; cd /; rm -rf "$work"
echo "transcript: ${log#$root/}"; exit $code
