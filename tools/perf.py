# SPDX-License-Identifier: Apache-2.0
"""Pass-time regression check of `tools/perf.ps1`.

Run (Blender's python, -I first):
  perf.py compare MEASURED.jsonl [--limit L] [--history GLOB]   decide each measured pass_ms line
  perf.py spread RUN1.jsonl ... RUNn.jsonl                      tolerance measurement (5 runs)
  perf.py --self-test                                           tests/budgets/perf_cases/
Ref: docs/decisions/0009-build-test-process.md d13 (B-38, the limit), d18 (perf), d22 (history line).
The baseline of a scene is the last bench/history line with the same machine, scene and metric `pass_ms`.
Decision table, with limit L = 1 + tolerance and ratio = measured / baseline:
  ratio < 2 - L                   PASS, improved: the lead commits the new line
  ratio <= L                      PASS
  ratio <= 1 + 2 x tolerance      FAIL, profile; accepted only by a ruling naming old and new
  above                           FAIL, stop and ask the lead
  no baseline                     PASS, the lead commits the printed line
Exit 0 pass, 1 a scene failed, 2 usage or input error.
"""
import argparse
import glob
import json
import os
import sys
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CASES = os.path.join(ROOT, "tests", "budgets", "perf_cases")
METRIC = "pass_ms"
BUDGET_ID = "B-38"


class PerfError(Exception):
    """Unusable input: exit 2, never a silent pass."""


def read_lines(path):
    out = []
    with open(path, encoding="utf-8") as f:
        for n, ln in enumerate(f, 1):
            if not ln.strip():
                continue
            try:
                out.append(json.loads(ln))
            except ValueError as e:
                raise PerfError(f"{path}:{n}: not JSON: {e}")
    return out


def read_history(pattern):
    lines = []
    for p in sorted(glob.glob(pattern)):
        lines += read_lines(p)
    return lines


def budget_limit(path):
    with open(path, "rb") as f:
        rows = {b.get("id"): b for b in tomllib.load(f).get("budget", [])}
    if BUDGET_ID not in rows or "limit" not in rows[BUDGET_ID]:
        raise PerfError(f"{path}: {BUDGET_ID} has no limit")
    return float(rows[BUDGET_ID]["limit"])


def baseline(history, machine, scene):
    found = None
    for o in history:  # file order, then line order: the last match is the newest
        if o.get("machine") == machine and o.get("scene") == scene and o.get("metric") == METRIC:
            found = o
    return found


def decide(ratio, limit):
    """Returns (verdict, note) for one ratio."""
    tol = limit - 1.0
    if ratio < 2.0 - limit:
        return "PASS", "improved: lead commits the new line"
    if ratio <= limit:
        return "PASS", ""
    if ratio <= 1.0 + 2.0 * tol:
        return "FAIL", "profile with 123's nsys method; accept only by a ruling naming old and new"
    return "FAIL", "stop and ask the lead"


def compare(measured, history, limit):
    """Returns (lines, failed). One verdict line per measured pass_ms line, at most 20 lines in all."""
    out, failed = [], False
    seen = False
    for m in measured:
        if m.get("metric") != METRIC:
            continue
        seen = True
        scene, mach = m["scene"], m["machine"]
        base = baseline(history, mach, scene)
        if base is None:
            out.append(f"PASS {scene}: no baseline: lead commits the printed line")
            out.append(json.dumps(m, separators=(",", ":")))
            continue
        ratio = m["value"] / base["value"]
        verdict, note = decide(ratio, limit)
        failed = failed or verdict == "FAIL"
        out.append(f"{verdict} {scene}: {m['value']:.4g} ms against {base['value']:.4g} ms ({base['date']}) "
                   f"ratio {ratio:.3f}, limit {limit:.3f}" + (f"; {note}" if note else ""))
    if not seen:
        raise PerfError(f"no {METRIC} line in the measured file")
    return out[:20], failed


def spread_table(runs):
    """runs: one list of history lines per bench run. Returns (lines, tolerance)."""
    per = {}
    for r in runs:
        for o in r:
            if o.get("metric") == METRIC:
                per.setdefault(o["scene"], []).append(o["value"])
    if not per:
        raise PerfError(f"no {METRIC} line in the runs")
    lines, worst = [], 0.0
    for scene in sorted(per):
        v = per[scene]
        mean = sum(v) / len(v)
        sp = (max(v) - min(v)) / mean
        worst = max(worst, sp)
        lines.append(f"{scene}: {' '.join(f'{x:.4f}' for x in v)}  mean {mean:.4f}  spread {sp:.4f}")
    tol = 3.0 * worst
    lines.append(f"tolerance = 3 x {worst:.4f} = {tol:.4f}; B-38 limit = {1.0 + tol:.3f}")
    return lines, tol


def self_test():
    names = sorted(d for d in os.listdir(CASES) if os.path.isdir(os.path.join(CASES, d)))
    if not names:
        raise PerfError(f"{CASES}: no cases")
    bad = 0
    for name in names:
        d = os.path.join(CASES, name)
        with open(os.path.join(d, "expect.json"), encoding="utf-8") as f:
            exp = json.load(f)
        lines, failed = compare(read_lines(os.path.join(d, "measured.jsonl")),
                                read_history(os.path.join(d, "history*.jsonl")), exp["limit"])
        text = "\n".join(lines)
        ok = failed == exp["fail"] and all(s in text for s in exp["text"])
        print(f"{'ok  ' if ok else 'FAIL'} {name}" + ("" if ok else f": expected fail={exp['fail']} {exp['text']}, got fail={failed}\n{text}"))
        bad += not ok
    print(f"perf self-test: {len(names) - bad} of {len(names)} cases ok")
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true")
    sub = ap.add_subparsers(dest="cmd")
    c = sub.add_parser("compare")
    c.add_argument("measured")
    c.add_argument("--limit", type=float)
    c.add_argument("--budgets", default=os.path.join(ROOT, "tools", "budgets.toml"))
    c.add_argument("--history", default=os.path.join(ROOT, "bench", "history", "*.jsonl"))
    s = sub.add_parser("spread")
    s.add_argument("runs", nargs="+")
    a = ap.parse_args()
    try:
        if a.self_test:
            return self_test()
        if a.cmd == "compare":
            limit = a.limit if a.limit else budget_limit(a.budgets)
            lines, failed = compare(read_lines(a.measured), read_history(a.history), limit)
            print("\n".join(lines))
            return 1 if failed else 0
        if a.cmd == "spread":
            lines, _ = spread_table([read_lines(p) for p in a.runs])
            print("\n".join(lines))
            return 0
        ap.print_usage()
        return 2
    except (PerfError, OSError, KeyError) as e:
        print(f"perf: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
