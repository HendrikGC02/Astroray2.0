# SPDX-License-Identifier: Apache-2.0
"""Pass-time regression check of `tools/perf.ps1`.

Run (Blender's python, -I first):
  perf.py compare MEASURED.jsonl [--limit L] [--history GLOB]   decide each measured pass_ms line
        [--cumulative SHA]   then print, per scene, the ratio of the measured pass_ms and kernel_ms_per_sample
                             to the history lines of commit SHA (a prefix); information, no limit attached
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
import tempfile
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
        lines += [o for o in read_lines(p) if o.get("metric") == METRIC]  # other metrics never answer
    return lines


def budget_limit(path):
    with open(path, "rb") as f:
        rows = {b.get("id"): b for b in tomllib.load(f).get("budget", [])}
    if BUDGET_ID not in rows or "limit" not in rows[BUDGET_ID]:
        raise PerfError(f"{path}: {BUDGET_ID} has no limit")
    return float(rows[BUDGET_ID]["limit"])


def baseline(history, scene):
    found = None
    for o in history:  # file order, then line order: the last match is the newest
        if o.get("scene") == scene:
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
        # Another machine's timings say nothing about this one.
        base = baseline([o for o in history if o.get("machine") == mach], scene)
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


CUMULATIVE_METRICS = ("pass_ms", "kernel_ms_per_sample")


def median(v):
    v = sorted(v)
    return v[len(v) // 2] if len(v) % 2 else 0.5 * (v[len(v) // 2 - 1] + v[len(v) // 2])


def cumulative(measured, history, sha):
    """One line per measured scene and metric: the measured median over the median of the lines of the baseline
    commit `sha` (prefix) with the same machine, scene and metric. A metric the baseline never recorded says so
    instead of being derived from another. Ref: tasks/215 (the drift since a milestone's close, which the
    per-landing B-38 comparison cannot show)."""
    out = []
    for scene in sorted({m["scene"] for m in measured}):
        for metric in CUMULATIVE_METRICS:
            cur = [m for m in measured if m["scene"] == scene and m.get("metric") == metric]
            if not cur:
                continue
            base = [o["value"] for o in history if o.get("scene") == scene and o.get("metric") == metric
                    and o.get("sha", "").startswith(sha) and o.get("machine") == cur[0]["machine"]]
            if not base:
                out.append(f"CUMULATIVE {scene} {metric}: no baseline line at {sha}")
                continue
            now, then = median([m["value"] for m in cur]), median(base)
            out.append(f"CUMULATIVE {scene} {metric}: {now:.6g} against {then:.6g} at {sha[:9]} ratio {now / then:.3f}")
    if not out:
        raise PerfError(f"no {' or '.join(CUMULATIVE_METRICS)} line in the measured file")
    return out


def read_all_history(pattern):
    lines = []
    for p in sorted(glob.glob(pattern)):
        lines += read_lines(p)
    return lines


def record(measured, path):
    """Appends the measured pass_ms lines, compact as bench writes them. Returns how many."""
    rows = [m for m in measured if m.get("metric") == METRIC]
    text = ""
    if os.path.exists(path):
        with open(path, encoding="utf-8", newline="") as f:
            text = f.read()
    if text and not text.endswith("\n"):
        text += "\n"
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text + "".join(json.dumps(m, separators=(",", ":")) + "\n" for m in rows))
    return len(rows)


def check(measured, history, limit, record_path=None):
    """compare, then record only a PASS: a FAIL becomes a baseline only through a ruling naming old and new."""
    lines, failed = compare(measured, history, limit)
    if record_path and not failed:
        lines.append(f"recorded {record(measured, record_path)} pass_ms lines in {os.path.basename(record_path)}")
    return lines, failed


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
        with tempfile.TemporaryDirectory() as tmp:
            rec = os.path.join(tmp, "recorded.jsonl")
            lines, failed = check(read_lines(os.path.join(d, "measured.jsonl")),
                                  read_history(os.path.join(d, "history*.jsonl")), exp["limit"], rec)
            recorded = len(read_lines(rec)) if os.path.exists(rec) else 0
        text = "\n".join(lines)
        ok = (failed == exp["fail"] and all(s in text for s in exp["text"])
              and not any(s in text for s in exp.get("absent", [])) and (recorded > 0) == (not exp["fail"]))
        print(f"{'ok  ' if ok else 'FAIL'} {name}" + ("" if ok else f": expected fail={exp['fail']} {exp['text']}, got fail={failed}\n{text}"))
        bad += not ok
    # cumulative: the baseline commit's median, never the current value, is the divisor; another machine's
    # lines and another commit's lines are ignored; kernel_ms_per_sample has no baseline here.
    def row(sha, machine, metric, value):
        return {"sha": sha, "machine": machine, "scene": "b1m", "metric": metric, "value": value}
    hist = [row("aaaa1", "m", "pass_ms", 20.0), row("bbbb2", "m", "pass_ms", 10.0), row("bbbb2", "m", "pass_ms", 14.0),
            row("bbbb2", "other", "pass_ms", 1.0), row("cccc3", "m", "pass_ms", 11.0)]
    now = [row("dddd4", "m", "pass_ms", 15.0), row("dddd4", "m", "kernel_ms_per_sample", 1e-5)]
    got = cumulative(now, hist, "bbbb")
    ok = len(got) == 2 and got[0].endswith("ratio 1.250") and "no baseline" in got[1]
    ok = ok and cumulative(now, hist, "aaaa")[0].endswith("ratio 0.750")
    print(f"{'ok  ' if ok else 'FAIL'} cumulative" + ("" if ok else f": {got}"))
    bad += not ok
    # The tolerance formula: 3 x the larger of the two scene spreads, (max - min) / mean.
    def rows(scene, v):
        return [{"scene": scene, "metric": METRIC, "value": x} for x in v]
    runs = [rows("b1m", [10.0]) + rows("b5m", [20.0]), rows("b1m", [10.1]) + rows("b5m", [20.6]),
            rows("b1m", [9.9]) + rows("b5m", [19.4]), rows("b1m", [10.0]) + rows("b5m", [20.0]),
            rows("b1m", [10.0]) + rows("b5m", [20.0])]
    lines, tol = spread_table(runs)
    ok = abs(tol - 0.18) < 1e-9 and "limit = 1.180" in lines[-1]
    print(f"{'ok  ' if ok else 'FAIL'} spread_table" + ("" if ok else f": tolerance {tol}, {lines[-1]}"))
    bad += not ok
    print(f"perf self-test: {len(names) + 2 - bad} of {len(names) + 2} cases ok")
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true")
    sub = ap.add_subparsers(dest="cmd")
    c = sub.add_parser("compare")
    c.add_argument("measured")
    c.add_argument("--limit", type=float)
    c.add_argument("--record", help="append the measured pass_ms lines to this history file when every scene passes")
    c.add_argument("--cumulative", metavar="SHA", help="also print the ratios to the history lines of this commit")
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
            lines, failed = check(read_lines(a.measured), read_history(a.history), limit, a.record)
            if a.cumulative:
                lines += cumulative(read_lines(a.measured), read_all_history(a.history), a.cumulative)
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
