# SPDX-License-Identifier: Apache-2.0
"""Offline HTML report: tier durations, build times and the M1a close-out sections (measured gates against
their lines, kernel resources over commits, the showcase gallery with CPU and GPU side by side).

Run: <python> -I tools/report.py [--milestone M1b] [--selftest]
Ref: docs/decisions/0009-build-test-process.md d22 (report contents, history line), d13 (budgets).
Reads bench/history/*.jsonl, out/check/*.json, out/viewport/, out/gallery/<milestone>/ (tools/gallery.py;
default the newest milestone with a gallery directory) and the git
history of src/kernel/budget.toml; writes out/report/index.html (inline SVG and PNG, no network).
A section whose history lines do not exist yet says which metric names it waits for; it never draws an empty plot.
History metric names the M1a sections read (scene, unit): b19.idle, b19.partition (b1m/b5m, %: B-19's two rows);
b14.rate (b1m, /s); m1.stack, m1.spill_store, m1.spill_load (m1_probe, B); b18.cold, b18.warm (any, ms);
first_frame.v7a.* (127); pass_ms, kernel_ms_per_sample, rays_closest, rays_shadow, steps_per_pass (bench.ps1).
"""
import base64
import glob
import html
import json
import os
import re
import struct
import subprocess
import sys
import zlib

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TIER_BUDGET = {"fast": "B-08", "gate": "B-09", "full": "B-10", "ci": "B-11"}
BUILD_METRICS = ["build.clean", "build.incr_header", "build.incr_stage", "build.host_cpp"]
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


# 0009 B-14: the viewport's rate target and floor, drawn on every rate plot.
VIEWPORT_LINES = [("30/s target (B-1M)", 30.0), ("24/s floor", 24.0)]


def viewport_series(root):
    """Presented updates per second over the orbit, per scene, from the runs of tools/measure_viewport.ps1
    (out/viewport/rate_<scene>.json: a 0.5 s sliding window, one point every 0.25 s)."""
    s = {}
    for p in sorted(glob.glob(os.path.join(root, "out", "viewport", "rate_*.json"))):
        with open(p, encoding="utf-8-sig") as f:
            run = json.load(f)
        pts = (run.get("stats") or {}).get("series")
        if pts:
            s[run["scene"]] = [tuple(x) for x in pts]
    return s


# 0007 d35 / decision 23: Astroray's V7a is at most this multiple of Cycles' on every scene and switch.
FIRST_FRAME_GATE = 1.5
FF_RE = re.compile(r"^first_frame\.(v7a|v7a_read|v7a_event|v7b)\.(cycles|astroray)\.switch(\d)$")


def first_frame_table(history):
    """{(scene, switch): {"cycles": ms, "astroray": ms}} for V7a (the headline), from the history lines of
    tools/measure_ttff.ps1; the latest line of each metric wins (lines are in date order)."""
    last = {}
    for h in history:
        m = FF_RE.match(h.get("metric", ""))
        if m and m.group(1) == "v7a":
            last[(h["scene"], int(m.group(3)), m.group(2))] = float(h["value"])
    out = {}
    for (scene, sw, eng), v in last.items():
        out.setdefault((scene, sw), {})[eng] = v
    return {k: v for k, v in sorted(out.items()) if "cycles" in v and "astroray" in v}


def first_frame_bars(table, width=640, height=300):
    """Bars of Astroray/Cycles V7a per scene and switch, with the 1.5x gate and the 1.0x goal as lines."""
    l, r, t, b = 60, 150, 20, 56
    ratios = [v["astroray"] / v["cycles"] for v in table.values()]
    y1 = max(ratios + [FIRST_FRAME_GATE]) * 1.15
    n = len(table)
    slot = (width - l - r) / max(n, 1)

    def py(y):
        return height - b - y / y1 * (height - t - b)

    o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" role="img" '
         f'aria-label="first frame, Astroray over Cycles" font-family="sans-serif" font-size="11">',
         f'<rect x="{l}" y="{t}" width="{width - l - r}" height="{height - t - b}" fill="none" stroke="#999"/>']
    for k in range(5):
        v = y1 * k / 4
        o.append(f'<text x="{l - 4}" y="{py(v) + 4:.1f}" text-anchor="end">{v:.2f}</text>')
    o.append(f'<text x="12" y="{t + 10}" transform="rotate(-90 12 {t + 10})" text-anchor="end">Astroray / Cycles, V7a</text>')
    for (scene, sw), v in table.items():
        i = list(table).index((scene, sw))
        ratio = v["astroray"] / v["cycles"]
        x = l + slot * i + slot * 0.2
        colour = "#2ca02c" if ratio <= FIRST_FRAME_GATE else "#d62728"
        o.append(f'<rect data-bar="{html.escape(scene)} switch {sw}" x="{x:.1f}" y="{py(ratio):.1f}" width="{slot * 0.6:.1f}" '
                 f'height="{py(0) - py(ratio):.1f}" fill="{colour}"/>')
        o.append(f'<text x="{x + slot * 0.3:.1f}" y="{py(ratio) - 3:.1f}" text-anchor="middle">{ratio:.2f}x</text>')
        o.append(f'<text x="{x + slot * 0.3:.1f}" y="{height - b + 14}" text-anchor="middle">{html.escape(scene)}</text>')
        o.append(f'<text x="{x + slot * 0.3:.1f}" y="{height - b + 27}" text-anchor="middle">switch {sw}</text>')
        o.append(f'<text x="{x + slot * 0.3:.1f}" y="{height - b + 40}" text-anchor="middle" fill="#555">'
                 f'{v["astroray"]:.0f} / {v["cycles"]:.0f} ms</text>')
    for lab, y, c in ((f"{FIRST_FRAME_GATE}x gate", FIRST_FRAME_GATE, "#d62728"), ("1.0x goal", 1.0, "#555")):
        o.append(f'<line x1="{l}" x2="{width - r}" y1="{py(y):.1f}" y2="{py(y):.1f}" stroke="{c}" stroke-dasharray="5 3"/>')
        o.append(f'<text x="{width - r + 4}" y="{py(y) + 4:.1f}" fill="{c}">{lab}</text>')
    o.append("</svg>")
    return "\n".join(o)


def load_ttff_history(root):
    """The history lines of the latest tools/measure_ttff.ps1 run that the lead has not committed yet."""
    paths = sorted(glob.glob(os.path.join(root, "out", "bench", "history-ttff-*.jsonl")))
    lines = []
    for p in paths[-1:]:
        with open(p, encoding="utf-8-sig") as f:
            lines += [json.loads(ln) for ln in f if ln.strip()]
    return lines


# The lines the M1a sections draw their measurements against. Ref: 0009 B-19 (each <= 10 % of pass time), B-18
# (<= 260 ms warm / <= 490 ms cold), 0002 D10 (VM stack 512 B, other 128 B; VM spill allowance 128 B stores, 192 B loads).
B19_LINES = [("10 % (B-19)", 10.0)]
B18_LINES = [("490 ms cold (B-18)", 490.0), ("260 ms warm (B-18)", 260.0)]
M1_LINES = [("512 B VM stack (D10)", 512.0), ("192 B spill loads (D10)", 192.0), ("128 B spill stores (D10)", 128.0)]
STACK_LINES = [("512 B VM stack (D10)", 512.0), ("128 B other stack (D10)", 128.0)]
SPILL_LINES = [("128 B VM spill stores (D10)", 128.0)]


def awaiting(what):
    return f'<p class="awaiting">awaiting history lines: {html.escape(what)}; the lead commits them from the measurement run</p>'


def metric_series(history, metrics):
    """{"scene metric [unit]": [(i, value)]} for the exact metric names `metrics`, in line order."""
    rows = {}
    for h in history:
        if h["metric"] in metrics:
            rows.setdefault(f'{h["scene"]} {h["metric"]} [{h["unit"]}]', []).append(float(h["value"]))
    return {k: list(enumerate(v)) for k, v in sorted(rows.items())}


def metric_section(title, history, metrics, hlines=(), ylabel="", per_metric=False):
    """A plot of the named history metrics (one plot per metric when their units differ) with the budget's
    lines; the names that have no line yet are listed under it."""
    plots = []
    for group in ([[m] for m in metrics] if per_metric else [list(metrics)]):
        ser = metric_series(history, group)
        if ser:
            plots.append(svg_plot(f"{title}: {', '.join(group)}", ser, hlines, ylabel))
    have = {h["metric"] for h in history}
    missing = [m for m in metrics if m not in have]
    body = "\n".join(plots)
    if missing:
        body += awaiting(", ".join(missing))
    return section(title, body)


def kernel_rows(root, limit=12):
    """Per-kernel resource rows of src/kernel/budget.toml at the last `limit` commits that changed it, oldest first:
    [(short sha, {kernel: row})]. The file is the recorded baseline of 0002 D10 (a change edits its row in the diff)."""
    import tomllib
    try:
        shas = subprocess.run(["git", "log", f"-n{limit}", "--format=%h", "--", "src/kernel/budget.toml"], cwd=root,
                              capture_output=True, text=True, check=True).stdout.split()
        out = []
        for sha in reversed(shas):
            text = subprocess.run(["git", "show", f"{sha}:src/kernel/budget.toml"], cwd=root, capture_output=True,
                                  text=True, check=True).stdout
            out.append((sha, tomllib.loads(text).get("kernel", {})))
    except (OSError, subprocess.CalledProcessError):
        return []
    return out


def kernel_section(rows):
    if not rows:
        return section("Kernel resources", awaiting("none: no git history of src/kernel/budget.toml"))
    names = sorted({k for _, r in rows for k in r})
    labels = ", ".join(sha for sha, _ in rows)
    parts = []
    for field, title, lines, ylabel in (("regs", "registers", (), "registers"), ("stack", "stack", STACK_LINES, "bytes"),
                                        ("spill_store", "spill stores", SPILL_LINES, "bytes")):
        ser = {k: [(i, float(r[k][field])) for i, (_, r) in enumerate(rows) if k in r and field in r[k]] for k in names}
        ser = {k: v for k, v in ser.items() if v}
        parts.append(svg_plot(f"{title} per kernel over commits", ser, lines, ylabel))
    return section("Kernel resources", "\n".join(parts) + f"<p>x axis: commits {html.escape(labels)} (oldest first)</p>")


def read_gallery(root, milestone="M1a"):
    """The gallery's sidecars: {name: sidecar dict} with the thumbnail path added, from out/gallery/<milestone>/."""
    d = os.path.join(root, "out", "gallery", milestone)
    out = {}
    for p in sorted(glob.glob(os.path.join(d, "*.json"))):
        with open(p, encoding="utf-8") as f:
            side = json.load(f)
        name = os.path.splitext(os.path.basename(p))[0]
        thumb = os.path.join(d, "thumbs", name + ".png")
        if os.path.exists(thumb):
            side["thumb"] = thumb
            side["dir"] = d
            out[name] = side
    return out


VIRIDIS_H = os.path.join(ROOT, "src", "kernel", "film", "viridis.h")
_viridis = []


def viridis_table():
    """The engine's 256 viridis entries as (256, 3) uint8, parsed from src/kernel/film/viridis.h at run time so the
    report never carries a second copy of the table. Rounded like present.h's viridis_rgba8 (value * 255 + 0.5)."""
    if not _viridis:
        with open(VIRIDIS_H, encoding="utf-8") as f:
            text = f.read()
        body = text[text.index("kTable["):]
        vals = [float(v) for v in re.findall(r"(\d\.\d+)f", body[body.index("{"):body.index("};")])]
        if len(vals) != 256 * 3:
            raise RuntimeError(f"{VIRIDIS_H}: {len(vals)} table values, expected 768")
        _viridis.append(np.floor(np.array(vals, np.float32).reshape(256, 3) * 255.0 + 0.5).astype(np.uint8))
    return _viridis[0]


def heat_from_raw(values, lo, hi):
    """(h, w, 3) uint8 viridis image of a raw statistic over [lo, hi], nearest of 256 entries like the engine's
    viridis_rgba8 (0008 d12). hi <= lo has no scale: every pixel takes the first entry (no division by zero)."""
    if hi > lo:
        t = np.clip((values.astype(np.float64) - lo) / (hi - lo), 0.0, 1.0)
    else:
        t = np.zeros(values.shape)
    idx = np.floor(np.nan_to_num(t) * 255.0 + 0.5).astype(np.int64)
    return viridis_table()[idx]


def num(v):
    return f"{v:.4g}"


def png_html(rgb, name):
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, name + ".png")
        write_png(p, rgb)
        return embed_png(p)


def legend_html(lo, hi, ticks=5):
    """A viridis strip over [lo, hi] with tick labels as page text (the PNG writer has no font)."""
    strip = viridis_table()[np.arange(256)][None].repeat(14, axis=0)
    img = png_html(strip, "legend").replace("<img ", '<img style="width:100%;height:14px" ')
    labels = "".join(f"<span>{num(lo + (hi - lo) * i / (ticks - 1))}</span>" for i in range(ticks))
    return f'{img}<div style="display:flex;justify-content:space-between">{labels}</div>'


def raw_heat_cell(side, name, kind, image_range):
    """One raw statistic map with its legend and caption; the range is the record's (0008 d12: sample count
    [0, max spp], bounces [0, max depth]) or, for the second bounce map, [0, the image's maximum] (a report
    presentation chosen here, not a record default)."""
    entry = ((side or {}).get("heat") or {}).get(kind)
    if entry is None:
        return f'<td class="awaiting">missing: {html.escape(name)}</td>'
    path = os.path.join(side["dir"], entry["file"])
    if not os.path.exists(path):
        return f'<td class="awaiting">missing: {html.escape(entry["file"])}</td>'
    values = np.load(path)
    heat = side["heat"]
    record = ("max spp", float(heat["max_spp"])) if kind == "samples" else ("max depth", float(heat["max_bounces"]))
    hi = entry["max"] if image_range else record[1]
    step = max(1, -(-values.shape[1] // 640))
    img = heat_from_raw(values[::step, ::step], 0.0, hi)   # nearest: a heat map's pixels are values, never blended
    which = "range [0, image max] (report choice, not the record's)" if image_range else f"record range [0, {record[0]}]"
    const = ", constant" if entry["min"] == entry["max"] else ""
    cap = (f"{side['device']} {kind[:-1]} count, {which} = [0, {num(hi)}]: "
           f"min {num(entry['min'])}, mean {num(entry['mean'])}, max {num(entry['max'])}{const}")
    return f"<td>{png_html(img, name)}{legend_html(0.0, hi)}<small>{html.escape(cap)}</small></td>"


def newest_milestone(root):
    """The milestone with a gallery directory that sorts last by number then letter (M1a < M1b < M2); M1a if none."""
    names = [os.path.basename(p) for p in glob.glob(os.path.join(root, "out", "gallery", "M*")) if os.path.isdir(p)]
    key = lambda n: tuple((int(x) if x.isdigit() else x) for x in re.findall(r"\d+|[a-z]+", n[1:]))
    return max(names, key=key) if names else "M1a"


def gallery_sections(gallery, milestone="M1a"):
    """Side-by-side CPU, GPU and relative-difference thumbnails per scene, then the heat maps, with the sidecar's
    spp, time and settings as captions."""
    if not gallery:
        return [section("Renders, CPU and GPU", awaiting(f"none: run tools/gallery.py --milestone {milestone}")),
                section("Heat maps", awaiting(f"none: run tools/gallery.py --milestone {milestone}"))]

    def cell(name, caption):
        g = gallery.get(name)
        if g is None:
            return f'<td class="awaiting">missing: {html.escape(name)}</td>'
        return f"<td>{embed_png(g['thumb'])}<br><small>{html.escape(caption(g))}</small></td>"

    def dev_caption(g):
        st = g["settings"]
        t = f"{g['time']:.1f} s, " if g.get("time") is not None else ""
        return f"{g['device']}, {g['spp']} spp, {t}{st['width']} x {st['height']}, {st['view_transform']}; {g['build']}"

    def diff_caption(g):
        return (f"|GPU - CPU| / mean, black 0 %, yellow 100 %: median {g['median_rel']:.1%}, 95th {g['p95_rel']:.1%}, "
                f"GPU/CPU mean {g['mean_gpu_over_cpu']:.4f} (informational)")

    scenes = sorted({n.rsplit("_", 1)[0] for n in gallery if n.endswith(("_cpu", "_cuda", "_diff")) and "_heat_" not in n})
    rows = [f'<tr><th colspan="3">{html.escape(sc)}</th></tr><tr>{cell(sc + "_cpu", dev_caption)}'
            f'{cell(sc + "_cuda", dev_caption)}{cell(sc + "_diff", diff_caption)}</tr>' for sc in scenes]
    def denoise_caption(g):
        t = f"{g['device']}, {g['spp']} spp, use_denoising {g['denoise']}, {g['time']:.1f} s"
        pr = g.get("pair")
        if pr is None:
            return t
        return (f"{t}; flat wall patch: high-frequency energy {pr['hf_energy_off']:.3g} off, {pr['hf_energy_on']:.3g} on "
                f"({pr['hf_ratio_on_over_off']:.3f}x), mean {pr['patch_mean_off']:.4f} off, {pr['patch_mean_on']:.4f} on; "
                f"mean |on - off| {pr['mean_abs_diff']:.4f}")

    def panel_caption(g):
        names = "; ".join(f"{x['subject']}: {x['detail']}" for x in g["unsupported_entries"])
        return f"Diagnostics panel after the job added node '{g['added_node']}' ({len(g['unsupported_entries'])} NODE_UNSUPPORTED: {names})"

    for sc in sorted({n[:-len("_denoise_on")] for n in gallery if n.endswith("_denoise_on")}):
        rows.append(f'<tr><th colspan="3">{html.escape(sc)}: OIDN off and on in F12</th></tr><tr>'
                    f'{cell(sc + "_denoise_off", denoise_caption)}{cell(sc + "_denoise_on", denoise_caption)}</tr>')
    for sc in sorted({n[:-len("_panel")] for n in gallery if n.endswith("_panel")}):
        rows.append(f'<tr><th colspan="3">{html.escape(sc)}: the Diagnostics panel</th></tr><tr>{cell(sc + "_panel", panel_caption)}</tr>')
    heat = []
    raw_scenes = {g["scene"] for g in gallery.values() if g.get("heat")}
    for sc in sorted(raw_scenes | {n.split("_heat_")[0] for n in gallery if "_heat_" in n}):
        if sc in raw_scenes:   # a gallery rendered before the raw passes existed has the screenshots only
            sides = {d: gallery.get(f"{sc}_{d}") for d in ("cpu", "cuda")}
            for kind, image_range in (("samples", False), ("bounces", False), ("bounces", True)):
                title = f"{kind[:-1]} count, " + ("range [0, image max]" if image_range else "record range")
                heat.append(f'<tr><th colspan="2">{html.escape(sc)}: {title} (raw pass)</th></tr><tr>'
                            + "".join(raw_heat_cell(sides[d], f"{sc}_{d}_{kind}.npy", kind, image_range)
                                      for d in ("cpu", "cuda")) + "</tr>")
        for kind in ("samples", "bounces"):
            heat.append(f'<tr><th colspan="2">{html.escape(sc)}: {kind[:-1]} count (viewport screenshots)</th></tr><tr>'
                        f'{cell(f"{sc}_heat_{kind}_cpu", dev_caption)}{cell(f"{sc}_heat_{kind}_cuda", dev_caption)}</tr>')
    heat_body = f"<table>{''.join(heat)}</table>" if heat else awaiting("none: heat maps not rendered")
    return [section("Renders, CPU and GPU", f"<table>{''.join(rows)}</table>"), section("Heat maps", heat_body)]


def build_html(history, checks, budgets, viewport=None, kernels=None, gallery=None, milestone="M1a"):
    parts = []
    ts = tier_series(history, checks)
    if ts:
        hl = [(f"{TIER_BUDGET[t]} {t}", budgets[TIER_BUDGET[t]]) for t in ts
              if t in TIER_BUDGET and budgets.get(TIER_BUDGET[t])]
        parts.append(section("Tier durations", svg_plot("Tier durations per check run", ts, hl, "seconds")))
    else:
        parts.append(section("Tier durations", awaiting("tier.fast, tier.gate, tier.full or out/check runs")))
    bs = {}
    for h in history:
        if h["metric"] in BUILD_METRICS:
            bs.setdefault(h["metric"], []).append(float(h["value"]))
    if bs:
        parts.append(section("Build times per edit class",
                             svg_plot("Build times", {k: list(enumerate(v)) for k, v in sorted(bs.items())}, (), "seconds")))
    else:
        parts.append(section("Build times per edit class", awaiting(", ".join(BUILD_METRICS))))
    parts.append(metric_section("Throughput", history, ["pass_ms", "kernel_ms_per_sample"], per_metric=True))
    parts.append(metric_section("Work counters", history, ["rays_closest", "rays_shadow", "steps_per_pass"],
                                per_metric=True))
    if viewport:
        parts.append(section("Viewport rate (B-14)", svg_plot("Presented updates per second during the orbit",
                                                              viewport, VIEWPORT_LINES, "updates/s")))
    else:
        parts.append(metric_section("Viewport rate (B-14)", history, ["b14.rate"], VIEWPORT_LINES, "updates/s"))
    table = first_frame_table(history)
    parts.append(section("First frame vs Cycles (V7a)", first_frame_bars(table)) if table else
                 section("First frame vs Cycles (V7a)", awaiting("first_frame.v7a.cycles.switchN and .astroray.switchN")))
    parts.append(metric_section("B-19: GPU idle and partition shares", history, ["b19.idle", "b19.partition"],
                                B19_LINES, "% of pass"))
    parts.append(metric_section("M-1 probe: spills and stack", history, ["m1.stack", "m1.spill_store", "m1.spill_load"],
                                M1_LINES, "bytes"))
    parts.append(metric_section("OptiX cold compile (B-18)", history, ["b18.cold", "b18.warm"], B18_LINES, "ms"))
    parts.append(kernel_section(kernels if kernels is not None else []))
    parts += gallery_sections(gallery, milestone)
    return ("<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>Astroray 2.0 report</title>"
            "<style>body{font-family:sans-serif;margin:1em 2em}section{margin-bottom:1.5em}"
            "img{max-width:100%}td{vertical-align:top;padding:2px}th{text-align:left;padding-top:10px}"
            ".awaiting{color:#a60}</style></head><body>\n"
            "<h1>Astroray 2.0 report</h1>\n" + "\n".join(parts) + "\n</body></html>\n")


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


# 3x5 digits, five rows of three bits each, for the axis numbers of the PNG plot (no font is available).
_DIGITS = {"0": "111101101101111", "1": "010110010010111", "2": "111001111100111", "3": "111001111001111",
           "4": "101101111001001", "5": "111100111001111", "6": "111100111101111", "7": "111001001001001",
           "8": "111101111101111", "9": "111101111001111"}


def write_png(path, rgb):
    """An 8-bit RGB PNG from a (h, w, 3) uint8 array."""
    h, w, _ = rgb.shape
    raw = b"".join(b"\x00" + rgb[y].tobytes() for y in range(h))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def draw_digits(img, s, x, y, scale=2):
    for ch in s:
        bits = _DIGITS.get(ch)
        if bits:
            for i, bit in enumerate(bits):
                if bit == "1":
                    img[y + (i // 3) * scale:y + (i // 3 + 1) * scale, x + (i % 3) * scale:x + (i % 3 + 1) * scale] = 0
        x += 4 * scale


def ttff_png(path, table, scene, width=720, height=420):
    """Evidence for one scene: V7a in ms as bars, Cycles (blue) and Astroray (red when above the 1.5x line, green
    when not) side by side per switch, the 1.5x-Cycles line dashed over each Astroray bar, the ms printed above."""
    l, r, t, b = 60, 20, 24, 30
    img = np.full((height, width, 3), 255, np.uint8)
    cols = {sw: v for (sc, sw), v in table.items() if sc == scene}
    ymax = max(max(v["cycles"], v["astroray"], FIRST_FRAME_GATE * v["cycles"]) for v in cols.values()) * 1.15
    ytick = 10 ** int(np.floor(np.log10(ymax / 4)))
    ytick *= max(1, int(round((ymax / 4) / ytick)))

    def py(y):
        return int(height - b - y / ymax * (height - t - b))

    for v in range(0, int(ymax) + 1, int(ytick)):
        img[py(v), l:width - r] = 225
        draw_digits(img, str(v), 4, py(v) - 5)
    img[t:height - b, l] = 100
    img[height - b, l:width - r] = 100
    slot = (width - l - r) // max(len(cols), 1)
    for i, (sw, v) in enumerate(sorted(cols.items())):
        x = l + i * slot + slot // 8
        w = slot // 4
        ratio = v["astroray"] / v["cycles"]
        for j, (eng, colour) in enumerate((("cycles", [31, 119, 180]),
                                           ("astroray", [44, 160, 44] if ratio <= FIRST_FRAME_GATE else [214, 39, 40]))):
            x0 = x + j * (w + 8)
            img[py(v[eng]):py(0), x0:x0 + w] = colour
            draw_digits(img, str(int(round(v[eng]))), x0, py(v[eng]) - 12)
        gy = py(FIRST_FRAME_GATE * v["cycles"])
        for xx in range(x - 4, x + 2 * w + 12):
            if (xx // 5) % 2 == 0:
                img[gy, xx] = [214, 39, 40]
        draw_digits(img, str(sw), x + w, height - b + 8)   # the switch number under its pair of bars
    write_png(path, img)


def plot_png(path, series, hlines, ymax=80.0, width=960, height=420):
    """The rate plot as a raster, y in updates/s, x in seconds of the orbit; one colour per series (in the
    order of the dict, the colours of COLORS), the hlines dashed red, a grid every 10/s and every second."""
    l, r, t, b = 52, 12, 12, 30
    img = np.full((height, width, 3), 255, np.uint8)
    xmax = max((p[0] for s in series.values() for p in s), default=1.0)

    def px(x):
        return int(l + x / xmax * (width - l - r))

    def py(y):
        return int(height - b - min(y, ymax) / ymax * (height - t - b))

    def rgb(hexcolor):
        return [int(hexcolor[i:i + 2], 16) for i in (1, 3, 5)]

    def text(s, x, y, scale=2):
        draw_digits(img, s, x, y, scale)

    for v in range(0, int(ymax) + 1, 10):
        img[py(v), l:width - r] = 225
        text(str(v), 4, py(v) - 5)
    for sec in range(0, int(xmax) + 1):
        img[t:height - b, px(sec)] = 225
        text(str(sec), px(sec) - 3, height - b + 8)
    img[t:height - b, l] = 100
    img[height - b, l:width - r] = 100
    for _, y in hlines:
        for x in range(l, width - r):
            if (x // 6) % 2 == 0:
                img[py(y), x] = rgb("#d62728")
    for i, s in enumerate(series.values()):
        c = rgb(COLORS[i % len(COLORS)])
        for (x0, y0), (x1, y1) in zip(s, s[1:]):
            n = max(abs(px(x1) - px(x0)), abs(py(y1) - py(y0)), 1)
            for k in range(n + 1):
                xx, yy = px(x0 + (x1 - x0) * k / n), py(y0 + (y1 - y0) * k / n)
                img[yy - 1:yy + 2, xx - 1:xx + 2] = c
        img[t + 4 + 10 * i:t + 12 + 10 * i, width - r - 30:width - r - 6] = c  # legend swatch, in series order
    write_png(path, img)


def selftest():
    line = {"sha": "abc", "machine": "m", "driver": "d", "cuda": "c", "scene": "B-1M", "unit": "s",
            "n": 1, "spread": 0, "date": "2026-01-01T00:00:00Z"}
    hist = [dict(line, metric=m, value=v) for m, v in
            [("tier.fast", 4.0), ("tier.fast", 5.0), ("tier.gate", 100.0), ("tier.gate", 90.0),
             ("build.clean", 60.0), ("build.clean", 61.0), ("build.incr_header", 9.0), ("pass_ms", 3.0)]]
    ff = [dict(line, scene="b5m", unit="ms", metric=f"first_frame.v7a.{e}.switch{s}", value=v)
          for s, e, v in ((1, "cycles", 400.0), (1, "astroray", 500.0), (2, "cycles", 300.0), (2, "astroray", 600.0))]
    text = build_html(hist, [], {"B-08": 10.0, "B-09": 180.0})
    write(os.path.join(ROOT, "out", "report", "selftest.html"), text)
    with_ff = build_html(hist + ff, [], {"B-08": 10.0, "B-09": 180.0})
    bars = re.findall(r'<rect data-bar="[^"]*"[^>]*fill="(#[0-9a-f]{6})"', with_ff)
    if sorted(bars) != ["#2ca02c", "#d62728"]:
        print("selftest FAIL: first-frame bars (2 bars, 1.25x green and 2.00x red)")
        return 1
    want = 2 + 2 + 1  # tiers fast and gate; build.clean and build.incr_header; one throughput series
    got = text.count("<polyline")
    if got != want:
        print(f"selftest FAIL: {got} polylines, expected {want}")
        return 1
    n = text.count('class="awaiting"')
    if n != 10:  # throughput, work, B-14, V7a, B-19, M-1, B-18, kernels, renders, heat maps
        print(f"selftest FAIL: {n} awaiting notes, expected 10")
        return 1
    b19 = [dict(line, scene="b1m", unit="%", metric=m, value=v) for m, v in (("b19.idle", 9.0), ("b19.partition", 18.0))]
    with_b19 = build_html(hist + b19, [], {"B-08": 10.0, "B-09": 180.0})
    if with_b19.count("<polyline") != want + 2 or "10 % (B-19)" not in with_b19 or with_b19.count('class="awaiting"') != 9:
        print("selftest FAIL: B-19 section (two series, the 10 % line, one awaiting note fewer)")
        return 1
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        g = os.path.join(d, "out", "gallery", "M1a")
        os.makedirs(os.path.join(g, "thumbs"))
        px = np.full((4, 4, 3), 200, np.uint8)
        for name, extra in (("s_cpu", {"device": "CPU"}), ("s_cuda", {"device": "CUDA"}),
                            ("s_diff", {"median_rel": 0.1, "p95_rel": 0.4, "mean_gpu_over_cpu": 1.0}),
                            ("s_heat_samples_cpu", {"device": "CPU"}), ("s_heat_samples_cuda", {"device": "CUDA"})):
            write_png(os.path.join(g, "thumbs", name + ".png"), px)
            with open(os.path.join(g, name + ".json"), "w") as f:
                json.dump(dict(extra, spp=8, time=1.5, build="b",
                               settings={"width": 4, "height": 4, "view_transform": "AgX"}), f)
        pair = {"hf_energy_off": 4e-4, "hf_energy_on": 1e-5, "hf_ratio_on_over_off": 0.025, "patch_mean_off": 0.4,
                "patch_mean_on": 0.41, "mean_abs_diff": 0.02}
        for name, extra in (("s_denoise_off", {"denoise": "off"}), ("s_denoise_on", {"denoise": "on", "pair": pair}),
                            ("s_panel", {"added_node": "Wave", "unsupported_entries": [{"subject": "Mat", "detail": "ShaderNodeTexWave"}]})):
            write_png(os.path.join(g, "thumbs", name + ".png"), px)
            with open(os.path.join(g, name + ".json"), "w") as f:
                json.dump(dict(extra, device="CUDA", spp=8, time=1.5, build="b",
                               settings={"width": 4, "height": 4, "view_transform": "AgX"}), f)
        gal = build_html(hist, [], {"B-08": 10.0, "B-09": 180.0}, gallery=read_gallery(d))
        for want in ("s: OIDN off and on in F12", "high-frequency energy 0.0004 off, 1e-05 on", "s: the Diagnostics panel",
                     "1 NODE_UNSUPPORTED: Mat: ShaderNodeTexWave"):
            if want not in gal:
                print(f"selftest FAIL: denoise pair or panel row (page lacks {want!r})")
                return 1
        # s: three images; heat maps: samples on both devices, bounces missing on both
        if gal.count("<img") != 8 or gal.count("missing: s_heat_bounces") != 2:
            print(f"selftest FAIL: gallery cells ({gal.count('<img')} images)")
            return 1
        if "(raw pass)" in gal:
            print("selftest FAIL: raw-pass maps drawn for a gallery without raw passes")
            return 1
    table = viridis_table()
    ramp = np.linspace(0.0, 5.0, 6 * 6).reshape(6, 6)
    img = heat_from_raw(ramp, 0.0, 5.0)
    if table.shape != (256, 3) or table[0].tolist() != [68, 1, 84] or table[255].tolist() != [253, 231, 37] \
            or img[0, 0].tolist() != table[0].tolist() or img[-1, -1].tolist() != table[255].tolist():
        print("selftest FAIL: heat_from_raw (range start must be viridis entry 0, range end entry 255)")
        return 1
    if (heat_from_raw(np.full((3, 3), 7.0), 7.0, 7.0) != table[0]).any():
        print("selftest FAIL: heat_from_raw of a constant map")
        return 1
    with tempfile.TemporaryDirectory() as d:
        g = os.path.join(d, "out", "gallery", "M1b")
        os.makedirs(os.path.join(g, "thumbs"))
        os.makedirs(os.path.join(g, "work"))
        px = np.full((4, 4, 3), 200, np.uint8)
        counts = np.full((6, 6), 8.0, np.float32)   # constant: every pixel took the same number of samples
        bounces = np.linspace(0.0, 5.0, 36, dtype=np.float32).reshape(6, 6)
        for dev in ("cpu", "cuda"):
            np.save(os.path.join(g, "work", f"s_{dev}_samples.npy"), counts)
            if dev == "cpu":   # the CUDA bounce map is absent: named as missing, the page still renders
                np.save(os.path.join(g, "work", f"s_{dev}_bounces.npy"), bounces)
            heat = {"max_spp": 8, "max_bounces": 12,
                    "samples": {"min": 8.0, "mean": 8.0, "max": 8.0, "file": f"work/s_{dev}_samples.npy"},
                    "bounces": {"min": 0.0, "mean": 2.5, "max": 5.0, "file": f"work/s_{dev}_bounces.npy"}}
            names = [f"s_{dev}"] + [f"s_heat_{k}_{dev}" for k in ("samples", "bounces")]
            for name in names:
                write_png(os.path.join(g, "thumbs", name + ".png"), px)
                with open(os.path.join(g, name + ".json"), "w") as f:
                    json.dump(dict({"device": dev.upper(), "scene": "s", "spp": 8, "time": 1.5, "build": "b",
                                    "settings": {"width": 4, "height": 4, "view_transform": "AgX"}},
                                   **({"heat": heat} if name == f"s_{dev}" else {})), f)
        page = build_html(hist, [], {"B-08": 10.0, "B-09": 180.0}, gallery=read_gallery(d, "M1b"), milestone="M1b")
        want = ["<span>12</span>", "<span>6</span>", "<span>1.25</span>", "<span>3.75</span>",
                "min 8, mean 8, max 8, constant", "min 0, mean 2.5, max 5", "[0, max spp]", "[0, max depth]",
                "range [0, image max]", "missing: work/s_cuda_bounces.npy"]
        lost = [w for w in want if w not in page]
        if lost or page.count("viewport screenshots") != 2 or page.count("missing: s_heat") != 0 \
                or page.count("(raw pass)") != 3:
            print(f"selftest FAIL: raw heat maps (page lacks {lost}, {page.count('(raw pass)')} raw headings)")
            return 1
    print("selftest ok")
    return 0


def main():
    if "--selftest" in sys.argv[1:]:
        return selftest()
    if "--viewport-png" in sys.argv[1:]:
        series = viewport_series(ROOT)
        if not series.get("b1m"):
            print("no out/viewport/rate_b1m.json; run tools/measure_viewport.ps1")
            return 1
        png = os.path.join(ROOT, "out", "evidence", "096", "viewport_rate_b1m.png")
        plot_png(png, {"b1m": series["b1m"]}, VIEWPORT_LINES)
        print(f"wrote {png}")
        return 0
    if "--ttff-png" in sys.argv[1:]:
        table = first_frame_table(load_history(ROOT) + load_ttff_history(ROOT))
        if not any(sc == "b5m" for sc, _ in table):
            print("no first-frame history for b5m; run tools/measure_ttff.ps1")
            return 1
        png = os.path.join(ROOT, "out", "evidence", "127", "ttff_s2.png")
        ttff_png(png, table, "b5m")
        print(f"wrote {png}")
        return 0
    argv = sys.argv[1:]
    milestone = argv[argv.index("--milestone") + 1] if "--milestone" in argv else newest_milestone(ROOT)
    out = os.path.join(ROOT, "out", "report", "index.html")
    write(out, build_html(load_history(ROOT) + load_ttff_history(ROOT), load_checks(ROOT), load_budgets(ROOT),
                          viewport_series(ROOT), kernel_rows(ROOT), read_gallery(ROOT, milestone), milestone))
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
