# SPDX-License-Identifier: Apache-2.0
"""Steps by runs table for several out/check/gate.json copies: median and max per step, the wall
time of each concurrent group (the slowest member, since members start together), the tier
`seconds` and PASS/FAIL per run. Exit 1 when any run failed.

Usage: <python> -I tools/gate_table.py [--check tools/check.ps1] run-1.json run-2.json ...
Group membership is read from the `Group = 'gN'` tags in the check script's gate table.
"""
import argparse
import json
import re
import statistics
import sys


def read_groups(check_path):
    """{group: [step names]} from the gate table of tools/check.ps1 (Name and Group on one line)."""
    groups = {}
    with open(check_path, encoding="utf-8") as f:
        for line in f:
            n = re.search(r"@\{ Name = '([^']+)'", line)
            g = re.search(r"Group = '(\w+)'", line)
            if n and g:
                groups.setdefault(g.group(1), []).append(n.group(1))
    return groups


def load_runs(paths):
    runs = []
    for p in paths:
        with open(p, encoding="utf-8") as f:
            runs.append(json.load(f))
    return runs


def build_rows(runs, groups):
    """[(label, [per-run seconds or None])]: steps in first-run order, then group walls, then total."""
    order = []
    for r in runs:
        for s in r["steps"]:
            if s["name"] not in order:
                order.append(s["name"])
    per = [{s["name"]: s["seconds"] for s in r["steps"]} for r in runs]
    rows = [(n, [d.get(n) for d in per]) for n in order]
    for g, members in sorted(groups.items()):
        vals = []
        for d in per:
            have = [d[m] for m in members if m in d]
            vals.append(max(have) if have else None)
        if any(v is not None for v in vals):
            rows.append((f"group {g} wall", vals))
    rows.append(("TIER seconds", [r["seconds"] for r in runs]))
    return rows


def summarize(vals):
    have = [v for v in vals if v is not None]
    if not have:
        return None, None
    return statistics.median(have), max(have)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("files", nargs="+")
    ap.add_argument("--check", default="tools/check.ps1")
    args = ap.parse_args()
    runs = load_runs(args.files)
    rows = build_rows(runs, read_groups(args.check))
    w = max(len(r[0]) for r in rows)
    print(f"{'step':<{w}}  " + " ".join(f"{'run ' + str(i + 1):>7}" for i in range(len(runs))) + f" {'median':>8} {'max':>8}")
    for label, vals in rows:
        med, mx = summarize(vals)
        cells = " ".join(f"{v:7.1f}" if v is not None else f"{'-':>7}" for v in vals)
        tail = f" {med:8.1f} {mx:8.1f}" if med is not None else ""
        print(f"{label:<{w}}  {cells}{tail}")
    print(f"{'result':<{w}}  " + " ".join(f"{('PASS' if r['pass'] else 'FAIL'):>7}" for r in runs))
    return 0 if all(r["pass"] for r in runs) else 1


if __name__ == "__main__":
    sys.exit(main())
