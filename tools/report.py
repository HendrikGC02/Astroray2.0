# SPDX-License-Identifier: Apache-2.0
"""Offline HTML report: tier durations, build times, empty sections for later plots.

Run: <python> -I tools/report.py [--selftest]
Ref: docs/decisions/0009-build-test-process.md d22 (report contents, history line), d13 (budgets).
Reads bench/history/*.jsonl and out/check/*.json; writes out/report/index.html (inline SVG, no network).
"""
import base64
import glob
import html
import json
import os
import re
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TIER_BUDGET = {"fast": "B-08", "gate": "B-09", "full": "B-10", "ci": "B-11"}
BUILD_METRICS = ["build.clean", "build.incr_header", "build.incr_stage", "build.host_cpp"]
PLACEHOLDERS = [("throughput", "throughput"), ("work counters", "work"), ("first frame vs Cycles", "first_frame"),
                ("viewport rate", "viewport"), ("kernel resources", "kernel"), ("heat maps", "heat"),
                ("renders", "render")]
COLORS = ["#1f77b4", "#d62728", "#2ca02c", "#9467bd", "#ff7f0e", "#17becf", "#8c564b"]


def embed_png(path):
    """Inline <img> for a PNG, for sections that later show renders and heat maps."""
    with open(path, "rb") as f:
        b = base64.b64encode(f.read()).decode()
    return f'<img src="data:image/png;base64,{b}" alt="{html.escape(os.path.basename(path))}">'


def load_history(root):
    lines = []
    for p in sorted(glob.glob(os.path.join(root, "bench", "history", "*.jsonl"))):
        with open(p, encoding="utf-8") as f:
            for n, ln in enumerate(f, 1):
                if ln.strip():
                    try:
                        lines.append(json.loads(ln))
                    except ValueError as e:
                        raise SystemExit(f"{p}:{n}: not JSON ({e}); run tools/lint.py --section history")
    return lines


def load_checks(root):
    out = []
    for p in sorted(glob.glob(os.path.join(root, "out", "check", "*.json"))):
        with open(p, encoding="utf-8-sig") as f:
            out.append(json.load(f))
    return out


def budget_seconds(text):
    """First limit in a budgets.toml text cell, in seconds (`<= 3 min` -> 180); None if it has none."""
    m = re.search(r"(\d+(?:\.\d+)?)\s*(min|s)\b", text)
    if not m:
        return None
    return float(m.group(1)) * (60 if m.group(2) == "min" else 1)


def load_budgets(root):
    import tomllib
    with open(os.path.join(root, "tools", "budgets.toml"), "rb") as f:
        return {b["id"]: budget_seconds(b["text"]) for b in tomllib.load(f)["budget"]}


def svg_plot(title, series, hlines=(), ylabel="", width=640, height=300):
    """series: {name: [(x, y), ...]}; one <polyline> per series; hlines: [(label, y)]."""
    l, r, t, b = 60, 150, 20, 36
    pts = [p for s in series.values() for p in s]
    xs = np.array([p[0] for p in pts], float)
    ys = np.array([p[1] for p in pts] + [y for _, y in hlines], float)
    x0, x1 = (xs.min(), xs.max()) if len(xs) else (0.0, 1.0)
    if x1 == x0:
        x0, x1 = x0 - 0.5, x1 + 0.5
    y1 = ys.max() * 1.1 if len(ys) and ys.max() > 0 else 1.0

    def px(x):
        return l + (x - x0) / (x1 - x0) * (width - l - r)

    def py(y):
        return height - b - y / y1 * (height - t - b)

    o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" role="img" '
         f'aria-label="{html.escape(title)}" font-family="sans-serif" font-size="11">',
         f'<rect x="{l}" y="{t}" width="{width - l - r}" height="{height - t - b}" fill="none" stroke="#999"/>']
    for k in range(5):
        v = y1 * k / 4
        o.append(f'<text x="{l - 4}" y="{py(v) + 4:.1f}" text-anchor="end">{v:.3g}</text>')
    o.append(f'<text x="12" y="{t + 10}" transform="rotate(-90 12 {t + 10})" text-anchor="end">{html.escape(ylabel)}</text>')
    for lab, y in hlines:
        o.append(f'<line x1="{l}" x2="{width - r}" y1="{py(y):.1f}" y2="{py(y):.1f}" stroke="#d62728" stroke-dasharray="5 3"/>')
        o.append(f'<text x="{width - r + 4}" y="{py(y) + 4:.1f}" fill="#d62728">{html.escape(lab)}</text>')
    for i, (name, s) in enumerate(series.items()):
        c = COLORS[i % len(COLORS)]
        p = " ".join(f"{px(x):.1f},{py(y):.1f}" for x, y in s)
        o.append(f'<polyline data-series="{html.escape(name)}" points="{p}" fill="none" stroke="{c}" stroke-width="2"/>')
        for x, y in s:
            o.append(f'<circle cx="{px(x):.1f}" cy="{py(y):.1f}" r="3" fill="{c}"/>')
        o.append(f'<text x="{width - r + 4}" y="{t + 12 + 14 * (i + len(hlines))}" fill="{c}">{html.escape(name)}</text>')
    o.append("</svg>")
    return "\n".join(o)


def tier_series(history, checks):
    """Per tier, durations in run order: history metric `tier.<tier>`, then the latest out/check run."""
    s = {}
    for h in history:
        if h["metric"].startswith("tier."):
            s.setdefault(h["metric"][5:], []).append(float(h["value"]))
    for c in checks:
        if c["tier"] not in TIER_BUDGET:  # build.json and other non-tier summaries
            continue
        s.setdefault(c["tier"], []).append(float(c["seconds"]))
    return {k: list(enumerate(v)) for k, v in sorted(s.items())}


def section(title, body):
    return f"<section><h2>{html.escape(title)}</h2>\n{body}\n</section>"


def build_html(history, checks, budgets):
    parts = []
    ts = tier_series(history, checks)
    if ts:
        hl = [(f"{TIER_BUDGET[t]} {t}", budgets[TIER_BUDGET[t]]) for t in ts
              if t in TIER_BUDGET and budgets.get(TIER_BUDGET[t])]
        parts.append(section("Tier durations", svg_plot("Tier durations per check run", ts, hl, "seconds")))
    else:
        parts.append(section("Tier durations", "<p>no data yet</p>"))
    bs = {}
    for h in history:
        if h["metric"] in BUILD_METRICS:
            bs.setdefault(h["metric"], []).append(float(h["value"]))
    if bs:
        parts.append(section("Build times per edit class",
                             svg_plot("Build times", {k: list(enumerate(v)) for k, v in sorted(bs.items())}, (), "seconds")))
    else:
        parts.append(section("Build times per edit class", "<p>no data yet</p>"))
    for title, prefix in PLACEHOLDERS:
        rows = {}
        for h in history:
            if h["metric"].startswith(prefix):
                rows.setdefault(f'{h["scene"]} {h["metric"]} [{h["unit"]}]', []).append(float(h["value"]))
        body = (svg_plot(title, {k: list(enumerate(v)) for k, v in rows.items()}) if rows else "<p>no data yet</p>")
        parts.append(section(title, body))
    return ("<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>Astroray 2.0 report</title>"
            "<style>body{font-family:sans-serif;margin:1em 2em}section{margin-bottom:1.5em}</style></head><body>\n"
            "<h1>Astroray 2.0 report</h1>\n" + "\n".join(parts) + "\n</body></html>\n")


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def selftest():
    line = {"sha": "abc", "machine": "m", "driver": "d", "cuda": "c", "scene": "B-1M", "unit": "s",
            "n": 1, "spread": 0, "date": "2026-01-01T00:00:00Z"}
    hist = [dict(line, metric=m, value=v) for m, v in
            [("tier.fast", 4.0), ("tier.fast", 5.0), ("tier.gate", 100.0), ("tier.gate", 90.0),
             ("build.clean", 60.0), ("build.clean", 61.0), ("build.incr_header", 9.0), ("throughput.mpaths", 3.0)]]
    text = build_html(hist, [], {"B-08": 10.0, "B-09": 180.0})
    write(os.path.join(ROOT, "out", "report", "selftest.html"), text)
    want = 2 + 2 + 1  # tiers fast and gate; build.clean and build.incr_header; one throughput series
    got = text.count("<polyline")
    if got != want:
        print(f"selftest FAIL: {got} polylines, expected {want}")
        return 1
    if text.count("no data yet") != len(PLACEHOLDERS) - 1:
        print("selftest FAIL: placeholder count")
        return 1
    print("selftest ok")
    return 0


def main():
    if "--selftest" in sys.argv[1:]:
        return selftest()
    out = os.path.join(ROOT, "out", "report", "index.html")
    write(out, build_html(load_history(ROOT), load_checks(ROOT), load_budgets(ROOT)))
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
