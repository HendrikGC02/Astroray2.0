# SPDX-License-Identifier: Apache-2.0
"""GPU idle share and partition-plus-sort share of one pass, from an Nsight Systems sqlite export.

Run (Blender's python, -I first):
  perf_shares.py EXPORT.sqlite [--scene NAME]     prints `NAME idle_share=... pq_share=... passes=N`
  perf_shares.py lines SCENE=EXPORT.sqlite ... --sha S --machine M --out FILE   history lines of one capture
  perf_shares.py compare MEASURED.jsonl [--record FILE]    each line against the last of the same machine,
                                                           scene and metric; worse beyond the tolerance FAILs
  perf_shares.py spread RUN1.jsonl ... RUNn.jsonl          tolerance measurement (5 captures)
  perf_shares.py --self-test                      synthetic exports and fixtures, no nsys needed
Ref: docs/decisions/0009-build-test-process.md d13 (B-19 row), d18; the definitions are 120's and 123's
(PR 94): a pass runs from one `k_film_build_active` launch to the next; idle = pass period minus the union of
the kernel intervals inside it; partition plus sort = the six kernels below. A share is the ratio of the
median over the passes of the numerator to the median of the period. The first BURN_IN passes of the capture
(bench's --warmup) are not measured.
History lines: metrics idle_share and pq_share, unit fraction, n = passes, spread = max - min of the per-pass
ratio (as bench's spread is max - min of its samples). Tolerance: tools/budgets.toml B-19 `shares_tolerance`,
relative: FAIL when measured / baseline > 1 + tolerance. Exit 0 pass, 1 a share failed, 2 refused or unusable.

Export schema (nsys 2024.6.2, `nsys export --type sqlite`; read from a real export of b1m):
  CUPTI_ACTIVITY_KIND_KERNEL(start INTEGER ns, end INTEGER ns, shortName INTEGER, demangledName INTEGER, ...)
  StringIds(id INTEGER PRIMARY KEY, value TEXT)          shortName and demangledName index it
The kernel name is the shortName string (the plain function name of an extern "C" __global__).
"""
import argparse
import datetime
import glob
import json
import os
import sqlite3
import statistics
import sys
import tempfile
import tomllib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I drops the script directory
import perf  # noqa: E402  read_lines and PerfError: the history reader of the pass-time check

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CASES = os.path.join(ROOT, "tests", "budgets", "perf_cases", "shares_cases.json")
METRICS = ("idle_share", "pq_share")
BUDGET_ID = "B-19"
BOUNDARY = "k_film_build_active"
PARTITION_SORT = frozenset((
    "k_queue_build_count", "k_queue_build_scan", "k_queue_build_scatter",
    "k_sort_count", "k_sort_scan", "k_sort_scatter"))
MIN_PASSES = 60
BURN_IN = 8  # bench's --warmup: its boundary kernels are in the capture and are not measured


class SharesError(perf.PerfError):
    """An export or input that cannot give a share: exit 2, never a silent number."""


def union_length(intervals):
    """Total length covered by (start, end) intervals; overlapping and nested ones count once."""
    total, cur_s, cur_e = 0, None, None
    for s, e in sorted(intervals):
        if cur_e is None or s > cur_e:
            if cur_e is not None:
                total += cur_e - cur_s
            cur_s, cur_e = s, e
        elif e > cur_e:
            cur_e = e
    if cur_e is not None:
        total += cur_e - cur_s
    return total


def load_kernels(path):
    """Returns [(start, end, name)] sorted by start."""
    if not os.path.exists(path):
        raise SharesError(f"{path}: no such export")
    con = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    try:
        try:
            rows = con.execute(
                "SELECT k.start, k.end, s.value FROM CUPTI_ACTIVITY_KIND_KERNEL k "
                "JOIN StringIds s ON s.id = k.shortName").fetchall()
        except sqlite3.Error as e:
            raise SharesError(f"{path}: not an nsys export with kernel rows ({e})")
    finally:
        con.close()
    if not rows:
        raise SharesError(f"{path}: no kernel rows (was the capture run with --trace=cuda?)")
    return sorted(rows)


def shares(kernels, burn_in=BURN_IN):
    """Returns {idle_share, pq_share, passes, idle_share_spread, pq_share_spread}. Passes are the periods
    between consecutive boundary kernels after the first `burn_in` passes; the last boundary opens no
    complete pass. Fewer than MIN_PASSES refuses (a short capture is noise)."""
    marks = [k[0] for k in kernels if k[2] == BOUNDARY][burn_in:]
    if not marks:
        raise SharesError(f"no {BOUNDARY} kernel after {burn_in} burn-in passes: cannot find the pass boundaries")
    periods, idles, pqs = [], [], []
    for a, b in zip(marks, marks[1:]):
        inside = [(max(s, a), min(e, b)) for s, e, _ in kernels if a <= s < b]
        pq = sum(min(e, b) - s for s, e, n in kernels if a <= s < b and n in PARTITION_SORT)
        periods.append(b - a)
        idles.append((b - a) - union_length(inside))
        pqs.append(pq)
    if len(periods) < MIN_PASSES:
        raise SharesError(f"only {len(periods)} complete passes, need at least {MIN_PASSES}")
    p = statistics.median(periods)

    def band(xs):
        r = [x / q for x, q in zip(xs, periods)]
        return max(r) - min(r)
    return {"idle_share": statistics.median(idles) / p, "pq_share": statistics.median(pqs) / p,
            "passes": len(periods), "idle_share_spread": band(idles), "pq_share_spread": band(pqs)}


def history_lines(results, sha, machine, driver="unknown", cuda="cuda", date=None):
    """results: {scene: shares()}. One line per scene and metric, keys as lint section `history` requires."""
    date = date or datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    return [{"sha": sha, "machine": machine, "driver": driver, "cuda": cuda, "scene": scene, "metric": m,
             "value": round(r[m], 6), "unit": "fraction", "n": r["passes"], "spread": round(r[m + "_spread"], 6),
             "date": date} for scene, r in sorted(results.items()) for m in METRICS]


def read_history(pattern):
    lines = []
    for p in sorted(glob.glob(pattern)):
        lines += [o for o in perf.read_lines(p) if o.get("metric") in METRICS]  # pass_ms lines never answer
    return lines


def tolerances(path):
    with open(path, "rb") as f:
        rows = {b.get("id"): b for b in tomllib.load(f).get("budget", [])}
    tol = rows.get(BUDGET_ID, {}).get("shares_tolerance")
    if not tol:
        raise SharesError(f"{path}: {BUDGET_ID} has no shares_tolerance")
    return {k: float(v) for k, v in tol.items()}


def compare(measured, history, tol):
    """Returns (lines, failed). Only a higher share is worse; a lower one beyond the tolerance is an improvement
    the lead commits. No baseline of this machine, scene and metric: PASS, the lead commits the printed line."""
    out, failed, seen = [], False, False
    for m in measured:
        if m.get("metric") not in METRICS:
            continue
        seen = True
        key = f"{m['scene']}.{m['metric']}"
        if key not in tol:
            raise SharesError(f"no tolerance for {key}")
        base = None
        for o in history:  # the last match is the newest
            if (o.get("machine"), o.get("scene"), o.get("metric")) == (m["machine"], m["scene"], m["metric"]):
                base = o
        if base is None:
            out.append(f"PASS {key}: no baseline: lead commits the printed line")
            out.append(json.dumps(m, separators=(",", ":")))
            continue
        ratio = m["value"] / base["value"]
        limit = 1.0 + tol[key]
        if ratio > limit:
            failed = True
            out.append(f"FAIL {key}: {m['value']:.4f} (sha {m['sha'][:9]}, {m['date']}) against {base['value']:.4f} "
                       f"(sha {base['sha'][:9]}, {base['date']}) ratio {ratio:.3f}, limit {limit:.3f}; "
                       "profile with 123's nsys method; accept only by a ruling naming old and new")
        else:
            note = "; improved: lead commits the new line" if ratio < 1.0 - tol[key] else ""
            out.append(f"PASS {key}: {m['value']:.4f} against {base['value']:.4f} ({base['date']}) "
                       f"ratio {ratio:.3f}, limit {limit:.3f}{note}")
    if not seen:
        raise SharesError("no idle_share or pq_share line in the measured file")
    return out[:20], failed


def record(measured, path):
    """Appends the share lines, compact as bench writes them. Returns how many."""
    rows = [m for m in measured if m.get("metric") in METRICS]
    text = ""
    if os.path.exists(path):
        with open(path, encoding="utf-8", newline="") as f:
            text = f.read()
    if text and not text.endswith("\n"):
        text += "\n"
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text + "".join(json.dumps(m, separators=(",", ":")) + "\n" for m in rows))
    return len(rows)


def spread_table(runs):
    """runs: one list of share lines per capture. Per scene and metric: values, mean, (max - min) / mean, and the
    tolerance 3 x that spread. Returns (lines, {scene.metric: tolerance})."""
    per = {}
    for r in runs:
        for o in r:
            if o.get("metric") in METRICS:
                per.setdefault(f"{o['scene']}.{o['metric']}", []).append(o["value"])
    if not per:
        raise SharesError("no share line in the runs")
    lines, tol = [], {}
    for key in sorted(per):
        v = per[key]
        mean = sum(v) / len(v)
        sp = (max(v) - min(v)) / mean
        tol[key] = 3.0 * sp
        lines.append(f"{key}: {' '.join(f'{x:.4f}' for x in v)}  mean {mean:.4f}  spread {sp:.4f}  tolerance {3.0 * sp:.4f}")
    return lines, tol


def _build(path, passes, gap=1000, first_gap=None, first=0):
    """A synthetic export. Every pass: boundary kernel 0..100, a count kernel 100..300 and a copy kernel
    250..400 (overlaps the count kernel; the copy is not a partition kernel), a sort kernel nested inside the
    copy's window at 300..350, then `gap` idle ns; pass period 400 + gap (first_gap for the first `first`
    passes). One more boundary kernel closes the last pass."""
    con = sqlite3.connect(path)
    con.execute("CREATE TABLE StringIds (id INTEGER PRIMARY KEY, value TEXT)")
    con.execute("CREATE TABLE CUPTI_ACTIVITY_KIND_KERNEL (start INTEGER, end INTEGER, shortName INTEGER, demangledName INTEGER)")
    names = [BOUNDARY, "k_queue_build_count", "k_copy", "k_sort_scan"]
    con.executemany("INSERT INTO StringIds VALUES (?, ?)", list(enumerate(names)))
    t = 0
    for i in range(passes):
        for s, e, n in [(0, 100, 0), (100, 300, 1), (250, 400, 2), (300, 350, 3)]:
            con.execute("INSERT INTO CUPTI_ACTIVITY_KIND_KERNEL VALUES (?, ?, ?, ?)", (t + s, t + e, n, n))
        t += 400 + (first_gap if first_gap is not None and i < first else gap)
    con.execute("INSERT INTO CUPTI_ACTIVITY_KIND_KERNEL VALUES (?, ?, 0, 0)", (t, t + 100))
    con.commit()
    con.close()


def self_test():
    bad = 0

    def check(name, ok, detail=""):
        nonlocal bad
        print(f"{'ok  ' if ok else 'FAIL'} {name}" + ("" if ok else f": {detail}"))
        bad += not ok

    def refuses(name, fn, text):
        try:
            fn()
            check(name, False, "no error")
        except SharesError as e:
            check(name, text in str(e), str(e))

    # Union of intervals: overlapping, nested, disjoint, touching.
    check("union overlapping", union_length([(0, 10), (5, 20)]) == 20)
    check("union nested", union_length([(0, 100), (10, 20), (30, 40)]) == 100)
    check("union disjoint", union_length([(0, 10), (20, 30)]) == 20)
    check("union touching", union_length([(0, 10), (10, 25)]) == 25)
    check("union empty", union_length([]) == 0)
    # Synthetic export: union 0..400 = 400 busy of period 1400 -> idle 1000/1400; count 200 + sort 50 = 250/1400.
    with tempfile.TemporaryDirectory() as tmp:
        db = os.path.join(tmp, "a.sqlite")
        _build(db, BURN_IN + MIN_PASSES + 5, first_gap=50000, first=BURN_IN)  # a slow burn-in is not measured
        r = shares(load_kernels(db))
        check("idle share", abs(r["idle_share"] - 1000 / 1400) < 1e-12, f"{r['idle_share']}")
        check("pq share (copy and boundary excluded, overlap once)", abs(r["pq_share"] - 250 / 1400) < 1e-12, f"{r['pq_share']}")
        check("pass count", r["passes"] == MIN_PASSES + 5, f"{r['passes']}")
        lines = history_lines({"b1m": r}, "abc", "m", date="2026-01-01T00:00:00Z")
        check("history lines", [(o["metric"], o["unit"], o["n"]) for o in lines]
              == [("idle_share", "fraction", MIN_PASSES + 5), ("pq_share", "fraction", MIN_PASSES + 5)]
              and set(lines[0]) == {"sha", "machine", "driver", "cuda", "scene", "metric", "value", "unit", "n", "spread", "date"})
        short = os.path.join(tmp, "short.sqlite")
        _build(short, BURN_IN + 5)
        refuses("refuses 5 passes", lambda: shares(load_kernels(short)), "complete passes")
        empty = os.path.join(tmp, "empty.sqlite")
        con = sqlite3.connect(empty)
        con.execute("CREATE TABLE StringIds (id INTEGER PRIMARY KEY, value TEXT)")
        con.execute("CREATE TABLE CUPTI_ACTIVITY_KIND_KERNEL (start INTEGER, end INTEGER, shortName INTEGER, demangledName INTEGER)")
        con.commit()
        con.close()
        refuses("refuses no kernel rows", lambda: load_kernels(empty), "no kernel rows")
        nob = os.path.join(tmp, "noboundary.sqlite")
        _build(nob, BURN_IN + MIN_PASSES + 2)
        con = sqlite3.connect(nob)
        con.execute("UPDATE StringIds SET value = 'k_other' WHERE id = 0")
        con.commit()
        con.close()
        refuses("refuses no boundary kernel", lambda: shares(load_kernels(nob)), BOUNDARY)
    # Fixture cases: comparison against history, decision table.
    with open(CASES, encoding="utf-8") as f:
        cases = json.load(f)
    for c in cases:
        with tempfile.TemporaryDirectory() as tmp:
            rec = os.path.join(tmp, "recorded.jsonl")
            try:
                lines, failed = compare(c["measured"], c["history"], c["tolerance"])
                recorded = record(c["measured"], rec) if not failed else 0
            except SharesError as e:
                lines, failed, recorded = [str(e)], None, 0
        text = "\n".join(lines)
        ok = (failed == c["fail"] and all(s in text for s in c["text"])
              and not any(s in text for s in c.get("absent", [])) and (recorded > 0) == (c["fail"] is False))
        print(f"{'ok  ' if ok else 'FAIL'} {c['name']}" + ("" if ok else f": expected fail={c['fail']} {c['text']}, got fail={failed}\n{text}"))
        bad += not ok

    # The tolerance formula: 3 x (max - min) / mean per scene and metric.
    def row(v):
        return [{"scene": "b1m", "metric": "idle_share", "value": v}]
    lines, tol = spread_table([row(0.10), row(0.11), row(0.09), row(0.10), row(0.10)])
    check("spread_table", abs(tol["b1m.idle_share"] - 0.6) < 1e-9, f"{tol}")
    print(f"perf_shares self-test: {'FAIL' if bad else 'ok'}")
    return 1 if bad else 0


def main(argv):
    try:
        if argv and argv[0] == "--self-test":
            return self_test()
        if argv and argv[0] == "lines":
            ap = argparse.ArgumentParser(prog="perf_shares.py lines")
            ap.add_argument("exports", nargs="+", help="SCENE=EXPORT.sqlite")
            ap.add_argument("--sha", required=True)
            ap.add_argument("--machine", required=True)
            ap.add_argument("--out", required=True)
            a = ap.parse_args(argv[1:])
            results = {}
            for e in a.exports:
                scene, _, path = e.partition("=")
                if not path:
                    raise SharesError(f"{e}: expected SCENE=EXPORT.sqlite")
                results[scene] = shares(load_kernels(path))
            with open(a.out, "w", encoding="utf-8", newline="") as f:
                for o in history_lines(results, a.sha, a.machine):
                    f.write(json.dumps(o, separators=(",", ":")) + "\n")
            for scene, r in sorted(results.items()):
                print(f"{scene} idle_share={r['idle_share']:.4f} pq_share={r['pq_share']:.4f} passes={r['passes']}")
            return 0
        if argv and argv[0] == "compare":
            ap = argparse.ArgumentParser(prog="perf_shares.py compare")
            ap.add_argument("measured")
            ap.add_argument("--record", help="append the measured lines to this history file when every share passes")
            ap.add_argument("--budgets", default=os.path.join(ROOT, "tools", "budgets.toml"))
            ap.add_argument("--history", default=os.path.join(ROOT, "bench", "history", "*.jsonl"))
            a = ap.parse_args(argv[1:])
            measured = perf.read_lines(a.measured)
            lines, failed = compare(measured, read_history(a.history), tolerances(a.budgets))
            if a.record and not failed:
                lines.append(f"recorded {record(measured, a.record)} share lines in {os.path.basename(a.record)}")
            print("\n".join(lines))
            return 1 if failed else 0
        if argv and argv[0] == "spread":
            ap = argparse.ArgumentParser(prog="perf_shares.py spread")
            ap.add_argument("runs", nargs="+")
            a = ap.parse_args(argv[1:])
            lines, _ = spread_table([perf.read_lines(p) for p in a.runs])
            print("\n".join(lines))
            return 0
        ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
        ap.add_argument("export")
        ap.add_argument("--scene", default="")
        a = ap.parse_args(argv)
        r = shares(load_kernels(a.export))
        print(f"{a.scene or os.path.basename(a.export)} idle_share={r['idle_share']:.4f} "
              f"pq_share={r['pq_share']:.4f} passes={r['passes']}")
        return 0
    except perf.PerfError as e:
        print(f"REFUSE perf_shares: {e}")
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
