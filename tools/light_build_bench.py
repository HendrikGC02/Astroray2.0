# SPDX-License-Identifier: Apache-2.0
"""B-23 capture: the light-structure build at 10k lamps, 100k and 1M emissive triangles.

Usage: <python> -I tools/light_build_bench.py [--runs 9] [--discard 2] [--history <file.jsonl>]
Builds the three recordings from scenes/b23_lights/build.py into out/tmp/191/, replays each one through
`astroray_cli render --device cpu --spp 1 --size 64x64` (RUNS processes per size, each in its own working
directory, since the CLI logs to ./astroray_cli.log) and reads the engine's `light table: ... built in`
line (Scene::build_lights: gather, peaks and scale, importance, alias tables and tree, of which tree).
The first DISCARD processes of a size are dropped; the rest give the median, minimum and maximum of the
total, taken per 10k lights (total ms times 10,000 over the light count). One build per process: the CLI
cannot rebuild the table twice, so each build is a first build in a fresh process (the real first frame),
which is what B-23 counts; tests/cpu light_tree_kernel B-23 is the in-process median.
It does not take the machine lock: run it inside one (for example
`pwsh -NoProfile -Command ". tools/lib/lock.ps1; Enter-ArMachineLock -Job b23 | Out-Null; & <python> -I tools/light_build_bench.py"`)
and on an idle machine; it prints the processes of a build or GPU kind it sees before and after.
--history writes two lines per size (light_build_ms per 10k lights, light_build_total_ms) in the form of
bench/history (0009 d22) with perf.ps1's neutral machine id; the lead commits them.
Ref: docs/decisions/0009-build-test-process.md d13 row B-23, d22; src/host/scene/scene.cpp build_lights."""
import argparse
import datetime
import json
import os
import re
import statistics
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TMP = os.path.join(ROOT, "out", "tmp", "191")
CLI = os.path.join(ROOT, "out", "bin", "astroray_cli.exe")
MACHINE = "rtx5070ti-8c16t"  # perf.ps1's neutral id (region_bias.py writes the same); never COMPUTERNAME
SIZES = [("b23_10k_lamps", ["--lamps", "10000"]),
         ("b23_100k_triangles", ["--triangles", "100000"]),
         ("b23_1m_triangles", ["--triangles", "1000000"])]
LINE = re.compile(r"light table: (\d+) lights \((\d+) emissive triangles\) built in ([0-9.]+) ms \(gather ([0-9.]+), "
                  r"peaks and scale ([0-9.]+), importance, alias tables and tree ([0-9.]+), of which tree ([0-9.]+) "
                  r"for (\d+) nodes\) on (\d+) threads")
BUSY = ("cl", "link", "ninja", "cmake", "nvcc", "blender", "astroray_cli", "astroray_tests_cpu", "astroray_tests_gpu",
        "msbuild", "clang", "rustc", "cargo", "nsys")


def busy_processes():
    out = subprocess.run(["tasklist", "/fo", "csv", "/nh"], capture_output=True, text=True).stdout
    names = [row.split('","')[0].strip('"').lower().removesuffix(".exe") for row in out.splitlines() if row]
    return sorted({n for n in names if n in BUSY or n.startswith("astroray")})


def build_recording(name, args):
    rec = os.path.join(TMP, name + ".arrec")
    r = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "scenes", "b23_lights", "build.py"), "--out", rec, *args],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"FAIL light_build_bench: building {name}: {r.stdout[-300:]}{r.stderr[-300:]}")
    return rec


def one_build(rec, name, i):
    cwd = os.path.join(TMP, "run", f"{name}_{i}")
    os.makedirs(cwd, exist_ok=True)
    log = os.path.join(cwd, "astroray_cli.log")
    if os.path.exists(log):
        os.remove(log)
    r = subprocess.run([CLI, "render", rec, "-o", os.path.join(cwd, "film.fits"), "--device", "cpu", "--spp", "1",
                        "--size", "64x64"], cwd=cwd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"FAIL light_build_bench: render {name} run {i} exited {r.returncode}: {r.stdout[-400:]}")
    with open(log, encoding="utf-8", errors="replace") as f:
        found = [m for m in map(LINE.search, f) if m]
    if len(found) != 1:
        sys.exit(f"FAIL light_build_bench: {name} run {i}: {len(found)} `light table` lines in {log}, expected 1")
    g = found[0].groups()
    keys = ("lights", "triangles", "total", "gather", "peaks", "importance", "tree", "nodes", "threads")
    return {k: (int(v) if k in ("lights", "triangles", "nodes", "threads") else float(v)) for k, v in zip(keys, g)}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--runs", type=int, default=9, help="processes per size, the first --discard dropped")
    ap.add_argument("--discard", type=int, default=2)
    ap.add_argument("--history", help="write the history lines to this file")
    a = ap.parse_args()
    if not os.path.exists(CLI):
        sys.exit(f"FAIL light_build_bench: {CLI} is missing; run tools/build.ps1 first")
    os.makedirs(TMP, exist_ok=True)
    t0 = time.time()
    before = busy_processes()
    print("processes of a build or GPU kind before:", ", ".join(before) or "none")
    sha = subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True, text=True, cwd=ROOT).stdout.strip()
    date = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    lines = []
    print(f"{'scene':20} {'lights':>8} {'median ms':>10} {'per10k':>8} {'min':>8} {'max':>8} | phases at the median run (ms):"
          " gather, peaks+scale, importance+alias+tree, tree")
    for name, args in SIZES:
        rec = build_recording(name, args)
        runs = [one_build(rec, name, i) for i in range(a.runs)][a.discard:]
        n = len(runs)
        per10k = sorted(((r["total"] * 1e4 / r["lights"], r) for r in runs), key=lambda p: p[0])
        med_per10k, med = per10k[n // 2][0], per10k[n // 2][1]
        lo, hi = per10k[0][0], per10k[-1][0]
        totals = [r["total"] for r in runs]
        print(f"{name:20} {med['lights']:>8} {med['total']:>10.1f} {med_per10k:>8.1f} {lo:>8.1f} {hi:>8.1f} | "
              f"{med['gather']:.1f}, {med['peaks']:.1f}, {med['importance']:.1f}, {med['tree']:.1f}  "
              f"({n} builds, {med['threads']} threads, {med['nodes']} nodes)")
        base = {"sha": sha, "machine": MACHINE, "driver": "unknown", "cuda": "none", "scene": name, "n": n, "date": date}
        lines.append({**base, "metric": "light_build_ms", "value": round(med_per10k, 1), "unit": "ms per 10k lights",
                      "spread": round(hi - lo, 1)})
        lines.append({**base, "metric": "light_build_total_ms", "value": round(statistics.median(totals), 1), "unit": "ms",
                      "spread": round(max(totals) - min(totals), 1)})
    print("processes of a build or GPU kind after:", ", ".join(busy_processes()) or "none")
    print(f"capture wall time {time.time() - t0:.0f} s")
    if a.history:
        with open(a.history, "w", encoding="utf-8", newline="\n") as f:
            for l in lines:
                f.write(json.dumps(l, separators=(",", ":")) + "\n")
        print("history lines written to", a.history)


if __name__ == "__main__":
    main()
