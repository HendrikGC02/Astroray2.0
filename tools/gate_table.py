# SPDX-License-Identifier: Apache-2.0
"""Steps by runs table for several out/check/gate.json copies: median and max per step, the wall
time of each concurrent group (the slowest member, since members start together), the tier
`seconds` and PASS/FAIL per run. Exit 1 when any run failed.

Usage: <python> -I tools/gate_table.py [--check tools/check.ps1] run-1.json run-2.json ...
Group membership is read from the `Group = 'gN'` tags in the check script's gate table.

`--cases [--dir out/check/gate-steps] [--top 30]`: seconds per doctest case of the cpu and gpu gate
tests and per scene and device of the analytic and integrator steps, from the outputs one gate run
kept (check.ps1 `Keep`), slowest first, each joined with the commit that added it (`git log -S`), its
age in landings (first-parent commits since), the gate runs since and the red ones among them (from
out/land/history.jsonl of the main checkout: a run with another verdict than PASS/SKIP or a second
attempt; the history names no case, so a red is unattributed and the column informational).

`--cut N --list L [--prev P]` prints N doctest `first last` ranges; `--families OUT...` sums the `family count`
lines of shard outputs against each family m (check.ps1 runs both for the sharded cpu gate tests).
"""
import argparse
import datetime
import json
import os
import re
import statistics
import subprocess
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


# One regex per output shape: doctest's `--duration` line, analytic's and integrator's PASS/FAIL lines.
DOCTEST = re.compile(r"^\s*([0-9]+\.[0-9]+) s: (.+?)\s*$")
ANALYTIC = re.compile(r"^(?:PASS|FAIL) analytic (\S+) (\w+): .*, ([0-9.]+) s; reproduce")
INTEGRATOR = re.compile(r"^(?:PASS|FAIL) integrator (\S+) (\w+): .*\(([0-9.]+) s\)\s*$")
# step output file -> (what it holds, device)
CASE_FILES = {"cpu-gate-tests.out": ("case", "cpu"), "gpu-gate-tests.out": ("case", "cuda"),
              "analytic-cpu.out": ("scene", "cpu"), "analytic-cuda.out": ("scene", "cuda"),
              "integrator-cpu.out": ("integrator", "cpu"), "integrator-cuda.out": ("integrator", "cuda")}


def read_cases(directory):
    """[(seconds, kind, device, name)] from the kept step outputs of one gate run, slowest first.
    A missing file, or one with no timed line, is an error: a table without a step would mislead."""
    rows = []
    for fname, (kind, device) in CASE_FILES.items():
        path = os.path.join(directory, fname)
        if not os.path.isfile(path):
            raise SystemExit(f"FAIL gate_table: {path} is missing (run `check gate` first)")
        found = 0
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                if kind == "case":
                    m = DOCTEST.match(line)
                    row = (float(m.group(1)), kind, device, m.group(2)) if m else None
                else:
                    m = (ANALYTIC if kind == "scene" else INTEGRATOR).match(line)
                    row = (float(m.group(3)), kind, m.group(2), m.group(1)) if m else None
                if row:
                    rows.append(row)
                    found += 1
        if found == 0:
            raise SystemExit(f"FAIL gate_table: {path} holds no timed line")
    rows.sort(key=lambda r: -r[0])
    return rows


def git(*args):
    return subprocess.run(["git", *args], capture_output=True, text=True, encoding="utf-8").stdout


def added_commit(kind, name):
    """(short sha, commit datetime, landings since) of the commit that first held `name`, or None."""
    needle = name if kind == "case" else f'"{name}"'
    out = git("log", "-S" + needle, "--format=%h %cI", "--reverse").splitlines()
    if not out:
        return None
    sha, iso = out[0].split()
    when = datetime.datetime.fromisoformat(iso).replace(tzinfo=None)
    since = int(git("rev-list", "--first-parent", "--count", f"{sha}..HEAD").strip() or 0)
    return sha, when, since


def read_history(path):
    """[(datetime, red)] of the landings the main checkout recorded."""
    out = []
    if os.path.isfile(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                if line.strip():
                    d = json.loads(line)
                    out.append((datetime.datetime.fromisoformat(d["date"]),
                                d.get("verdict") not in ("PASS", "SKIP") or d.get("attempts", 1) > 1))
    return out


def history_path():
    # The main checkout is the first worktree git lists; a worktree has no out/land of its own.
    first = git("worktree", "list", "--porcelain").splitlines()[0].split(" ", 1)[1]
    return os.path.join(first, "out", "land", "history.jsonl")


def cases_table(directory, top):
    rows = read_cases(directory)
    hist = read_history(history_path())
    total = {}
    for sec, kind, device, _ in rows:
        total[(kind, device)] = total.get((kind, device), 0.0) + sec
    print("step totals: " + ", ".join(f"{k} {d} {v:.1f} s" for (k, d), v in sorted(total.items())))
    print("runs, reds: gate runs since the adding commit, and those red; history.jsonl names no case, so a red "
          "is unattributed and the column is informational")
    print(f"{'seconds':>8}  {'kind':<10} {'device':<6} {'added':<8} {'landings':>8} {'runs':>5} {'reds':>4}  name")
    for sec, kind, device, name in rows[:top]:
        a = added_commit(kind, name)
        if a is None:
            add, land, runs, reds = "?", "?", "?", "?"
        else:
            add, land = a[0], a[2]
            later = [r for d, r in hist if d >= a[1]]
            runs, reds = len(later), sum(1 for r in later if r)
        print(f"{sec:8.2f}  {kind:<10} {device:<6} {add:<8} {land:>8} {runs:>5} {reds:>4}  {name}")
    return 0


def list_names(text):
    """Test case names of `<exe> --list-test-cases`, in execution order: the lines between the
    'listing all test case names' line and the closing rule."""
    names, on = [], False
    for line in text.splitlines():
        if line.startswith("[doctest] listing all test case names"):
            on = True
        elif on and line.startswith("====="):
            if names:
                break
        elif on and line.strip():
            names.append(line.rstrip())
    return names


def cut_ends(names, seconds, n):
    """Last index (1-based, inclusive) of each of n contiguous doctest ranges over `names`, balancing
    seconds (name -> seconds of the last run; a missing name counts as the mean) and never cutting
    between the f32 and f64 instances of an AR_RT case (the name with and without a trailing ' f64'):
    they compare bitwise inside one process (sampler/f32 f64 bitwise). Without seconds the balance is by count. The ends are
    strictly increasing and the last is len(names), or ValueError."""
    total = len(names)
    if not 1 <= n <= total:
        raise ValueError(f"{n} shards over {total} cases")
    known = [seconds[x] for x in names if x in seconds]
    mean = sum(known) / len(known) if known else 1.0
    cost = [seconds.get(x, mean) if known else 1.0 for x in names]
    # A cut after index j (0-based) is allowed unless some name occurs on both sides of it.
    # AR_RT_SUFFIX names the f64 instance 'sampler/f32 f64 bitwise f64': the pair shares the name minus a trailing ' f64'.
    def key(x):
        return x[:-4] if x.endswith(" f64") else x

    first, last = {}, {}
    for j, x in enumerate(names):
        first.setdefault(key(x), j)
        last[key(x)] = j
    blocked = [False] * total
    for x in first:
        for j in range(first[x], last[x]):
            blocked[j] = True
    ends, acc, k, tsum = [], 0.0, 1, sum(cost)
    for j in range(total):
        acc += cost[j]
        if k < n and acc >= tsum * k / n and not blocked[j]:
            ends.append(j + 1)
            k += 1
    ends.append(total)
    # A run of blocked cuts can leave fewer than n ranges; that is an error, never a silent short split.
    if len(ends) != n or any(b <= a for a, b in zip(ends, ends[1:])) or ends[-1] != total:
        raise ValueError(f"cannot cut {total} cases into {n} ranges (got ends {ends})")
    return ends


def sum_families(texts):
    """({family: executed checks summed over the shard outputs}, {family: m}) from the
    `family count <name> <n> <m>` lines every test process prints."""
    counts, ms = {}, {}
    for text in texts:
        for line in text.splitlines():
            m = re.match(r"^family count (\S+) (\d+) (\d+)\s*$", line)
            if m:
                counts[m.group(1)] = counts.get(m.group(1), 0) + int(m.group(2))
                ms[m.group(1)] = int(m.group(3))
    return counts, ms


def check_families(paths):
    """Exit status of the family guard over shard outputs: no single shard sees a whole family, so the
    per-process check of tests/common/stat.cpp cannot fail for a split suite. A shard that printed no
    family line is an error too (the counts would silently be missing)."""
    texts = []
    for p in paths:
        with open(p, encoding="utf-8", errors="replace") as f:
            texts.append(f.read())
        if "family count " not in texts[-1]:
            print(f"FAIL families: {p} holds no `family count` line")
            return 1
    counts, ms = sum_families(texts)
    bad = 0
    for name in sorted(counts):
        if counts[name] > ms[name]:
            print(f"FAIL families: {name} ran {counts[name]} checks over the shards, m = {ms[name]}; "
                  "raising m is a reviewed budget change")
            bad += 1
    print("families: " + ", ".join(f"{k} {counts[k]} of {ms[k]}" for k in sorted(counts)))
    return 1 if bad else 0


def cut_main(n, list_path, prev_path):
    with open(list_path, encoding="utf-8", errors="replace") as f:
        names = list_names(f.read())
    seconds = {}
    if prev_path and os.path.isfile(prev_path):
        with open(prev_path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = DOCTEST.match(line)
                if m:
                    seconds[m.group(2)] = seconds.get(m.group(2), 0.0) + float(m.group(1))
    try:
        ends = cut_ends(names, seconds, n)
    except ValueError as e:
        print(f"FAIL gate_table --cut: {e}")
        return 1
    start = 1
    for e in ends:
        print(f"{start} {e}")
        start = e + 1
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--cut", type=int, metavar="N", help="print N doctest `first last` ranges over --list")
    ap.add_argument("--list", help="with --cut: the output of `<exe> --tier=gate --list-test-cases`")
    ap.add_argument("--prev", help="with --cut: the last run's per-case seconds (cpu-gate-tests.out)")
    ap.add_argument("--families", nargs="+", metavar="OUT", help="sum the `family count` lines of the shard outputs")
    ap.add_argument("files", nargs="*")
    ap.add_argument("--check", default="tools/check.ps1")
    ap.add_argument("--cases", action="store_true", help="per case and scene seconds of the last gate run")
    ap.add_argument("--dir", default="out/check/gate-steps", help="with --cases: where check.ps1 kept the outputs")
    ap.add_argument("--top", type=int, default=30, help="with --cases: rows printed")
    args = ap.parse_args()
    if args.cut:
        if not args.list:
            ap.error("--cut takes --list")
        return cut_main(args.cut, args.list, args.prev)
    if args.families:
        return check_families(args.families)
    if args.cases:
        return cases_table(args.dir, args.top)
    if not args.files:
        ap.error("give run files, or --cases")
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
