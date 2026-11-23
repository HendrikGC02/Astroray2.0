# SPDX-License-Identifier: Apache-2.0
"""B-31 sync timing on S1 (b1m) and S2 (b5m): the four cells of the row, in B-31's units (task 161).

Two modes, both started by tools/sync_timing.ps1:
  measure <blend> <scene> <out.json>   inside Blender (Invoke-ArBlender -Script): one fresh process per scene and
                                       run, so the cold figure is the first call in the process, the warm one the
                                       median of 10 further calls (094's method).
  report <dir> [--history] [--profile] plain Python: the table over every <dir>/<scene>_r*.json (median over runs),
                                       out/check/sync_timing.log, and with --history one bench/history line per cell.
The sync package is loaded without the engine DLL: sync.mesh.fill and fill_positions read into NumPy arrays (the
engine staging is a memmove target of the same call), instances.build runs with a kept InstanceCache and KeyMap as
the viewport keeps them, and sync_transforms sends to a recording edit.
Ref: 094 and 129 (PR 92, PR 101): cold = first pass, warm = median of 10; docs/decisions/0009 d13 row B-31.
"""
import cProfile
import datetime
import importlib.util
import io
import json
import os
import pstats
import re
import statistics
import subprocess
import sys
import time
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WARM = 10
SCENES = ("b1m", "b5m")
# metric suffix, label, unit, index of the limit in B-31's text
CELLS = (("mesh_full", "full mesh read", "ms/1M tri", 0), ("positions", "positions only", "ms/1M tri", 1),
         ("instances", "instance loop", "ms/10k inst", 2), ("move", "plain move", "ms", 3))


def limits():
    """B-31's four limits from budgets.toml's text, in order (the row stays the one source)."""
    with open(os.path.join(ROOT, "tools", "budgets.toml"), "rb") as f:
        text = next(b for b in tomllib.load(f)["budget"] if b["id"] == "B-31")["text"]
    found = [float(v) for v in re.findall(r"≤\s*([\d.]+)", text)]
    if len(found) != 4:
        raise RuntimeError(f"B-31's text has {len(found)} limits, the harness reads 4: {text}")
    return found


# --- inside Blender ---------------------------------------------------------------------------------------------

def load_sync():
    path = os.path.join(ROOT, "blender", "astroray", "sync")
    spec = importlib.util.spec_from_file_location(
        "ar_sync", os.path.join(path, "__init__.py"), submodule_search_locations=[path])
    mod = importlib.util.module_from_spec(spec)
    sys.modules["ar_sync"] = mod
    spec.loader.exec_module(mod)
    return mod


def timed(fn):
    t = time.perf_counter()
    fn()
    return (time.perf_counter() - t) * 1e3


def cold_warm(fn):
    cold = timed(fn)
    return cold, [timed(fn) for _ in range(WARM)]


def measure(blend, scene, out_path):
    import bpy
    load_sync()
    from ar_sync import instances, keys, mesh
    bpy.ops.wm.open_mainfile(filepath=blend, load_ui=False)
    dg = bpy.context.evaluated_depsgraph_get()
    # one evaluated mesh per original data, as the viewport reads geometry
    seen, mes = set(), []
    for ob in dg.objects:
        if ob.type == "MESH" and ob.original.data.session_uid not in seen:
            seen.add(ob.original.data.session_uid)
            mes.append((ob.original.data.session_uid, ob.data))
    lays = [mesh.layout(me) for _, me in mes]
    tri_m = sum(lay.C - 2 * lay.F for lay in lays) / 1e6
    outs = [mesh.alloc(lay) for lay in lays]
    res = {"scene": scene, "meshes": len(mes), "mtri": tri_m, "cells": {}}
    checks = mesh.LayoutChecks()

    def sweep(positions_only):
        def run():
            for (uid, me), lay, out in zip(mes, lays, outs):
                chk = checks.needed(uid, lay)
                if positions_only:
                    mesh.fill_positions(me, out.positions, out.corner_normals, chk)
                else:
                    mesh.fill(me, out, chk)
                    checks.passed(uid, lay)
        return run
    # Full read first, as the first sync of a scene is; positions-only follows on checked keys, as an edit does.
    c, w = cold_warm(sweep(False))
    res["cells"]["mesh_full"] = (c / tri_m, [x / tri_m for x in w])
    c, w = cold_warm(sweep(True))
    res["cells"]["positions"] = (c / tri_m, [x / tri_m for x in w])

    km, icache = keys.KeyMap(), instances.InstanceCache()
    lg = instances.lightgroup_indices(bpy.context.view_layer)
    holder = {}

    def build():
        holder["t"] = instances.build(dg, km, lightgroups=lg, cache=icache)
    c, w = cold_warm(build)
    table = holder["t"]
    res["instances"] = table.n
    k = 1e4 / table.n
    res["cells"]["instances"] = (c * k, [x * k for x in w])
    if "--profile" in sys.argv:
        res["profile"] = profile_split(instances, keys, dg, km, lg, icache) + profile_build(
            instances, dg, km, lg, icache)

    uid = next((u for key, i in table.index_of.items() if len(key) == 1
                for u in key if table.by_object.get(u) == [i]), None)
    if uid is None:
        raise RuntimeError(f"{scene}: no plain own-row object to move")
    ob = next(o for o in bpy.data.objects if o.session_uid == uid)

    class Rec:
        def call(self, *a):
            pass
    times = []

    def handler(_scene, g):
        ev = {u.id.original.session_uid: u.id for u in g.updates
              if u.id.original.id_type == "OBJECT" and u.id.original.session_uid == uid}
        if ev:
            t = time.perf_counter()
            ok = instances.sync_transforms(Rec(), None, table, g, ev)
            times.append((time.perf_counter() - t) * 1e3)
            if not ok:
                raise RuntimeError("sync_transforms refused a plain move")
    bpy.app.handlers.depsgraph_update_post.append(handler)
    for i in range(WARM + 1):
        ob.location.x += 0.01 * (i + 1)
        bpy.context.evaluated_depsgraph_get().update()
    bpy.app.handlers.depsgraph_update_post.remove(handler)
    if len(times) != WARM + 1:
        raise RuntimeError(f"{scene}: {len(times)} plain moves timed, expected {WARM + 1}")
    res["cells"]["move"] = (times[0], times[1:])
    with open(out_path, "w") as f:
        json.dump(res, f)
    print(f"sync_timing: {scene} {len(mes)} meshes {tri_m:.2f} Mtri {table.n} instances -> {out_path}")


def profile_split(instances, keys, dg, km, lg, icache):
    """Where a warm build's time goes, as 129 split it: the loop's work cumulatively (each line adds one kind of
    per-instance work to the one above), then the passes after the loop, each in ms per 10k instances."""
    geometry = instances.GEOMETRY_TYPES
    real, key = icache.real, km.key
    n = sum(1 for _ in dg.object_instances)

    def l0():
        for _ in dg.object_instances:
            pass

    def l1():
        for inst in dg.object_instances:
            if inst.object.type not in geometry:
                continue

    def l2():
        for inst in dg.object_instances:
            ob = inst.object
            if ob.type not in geometry:
                continue
            ob.original.session_uid

    def l3():
        for inst in dg.object_instances:
            ob = inst.object
            if ob.type not in geometry:
                continue
            uid = ob.original.session_uid
            keys.object_identity(inst, keys.instance_path(inst), uid)

    def l4():
        for inst in dg.object_instances:
            ob = inst.object
            if ob.type not in geometry:
                continue
            uid = ob.original.session_uid
            path = keys.instance_path(inst)
            keys.object_identity(inst, path, uid)
            keys.geometry_identity(inst, dg, path, real, uid)

    def l5():
        for inst in dg.object_instances:
            ob = inst.object
            if ob.type not in geometry:
                continue
            uid = ob.original.session_uid
            path = keys.instance_path(inst)
            ident = keys.object_identity(inst, path, uid)
            key(keys.geometry_identity(inst, dg, path, real, uid))
            key(ident)
    import numpy as np
    obs = {}
    for inst in dg.object_instances:
        if inst.object.type in geometry:
            o = inst.object.original
            obs[o.session_uid] = o
    uids = np.asarray(list(obs), np.int32)
    m16, rid = np.empty(n * 16, np.float32), np.empty(n, np.int32)

    def fg():
        dg.object_instances.foreach_get("matrix_world", m16)
        dg.object_instances.foreach_get("random_id", rid)
    steps = [("iterate only", l0), ("+ object, type", l1), ("+ original, session_uid", l2),
             ("+ instance_path, object_identity", l3), ("+ geometry_identity", l4), ("+ keymap.key x2 (loop total)", l5),
             ("after: foreach_get matrix, random_id", fg),
             ("after: _object_scalars", lambda: instances._object_scalars(uids)),
             ("whole build", lambda: instances.build(dg, km, lightgroups=lg, cache=icache))]
    out = ["split of a warm build, median of 10, ms per 10k instances (cumulative for the loop lines):"]
    for name, fn in steps:
        v = statistics.median(timed(fn) for _ in range(WARM)) * 1e4 / n
        out.append(f"  {name:<40} {v:7.2f}")
    return "\n".join(out) + "\n"


def profile_build(instances, dg, km, lg, icache):
    """cProfile of WARM warm builds: the top functions by own time, as text."""
    pr = cProfile.Profile()
    pr.enable()
    for _ in range(WARM):
        instances.build(dg, km, lightgroups=lg, cache=icache)
    pr.disable()
    s = io.StringIO()
    pstats.Stats(pr, stream=s).sort_stats("tottime").print_stats(14)
    return s.getvalue()


# --- plain Python -----------------------------------------------------------------------------------------------

def fmt(v):
    return f"{v:.3g}" if v >= 1 else f"{v:.3f}"


def report(d, history, profile):
    lim = limits()
    runs = {s: sorted(f for f in os.listdir(d) if re.fullmatch(rf"{s}_r\d+\.json", f)) for s in SCENES}
    data = {s: [json.load(open(os.path.join(d, f))) for f in fs] for s, fs in runs.items() if fs}
    if not data:
        raise RuntimeError(f"no result for {SCENES} in {d}")
    rows = [f"B-31 sync timing, median over runs (cold = first call, warm = median of {WARM}); limits "
            f"{lim[0]:g} / {lim[1]:g} ms per 1M tri, {lim[2]:g} ms per 10k inst, {lim[3]:g} ms",
            f"{'scene':<5} {'cell':<15} {'unit':<11} {'cold':>8} {'warm':>8} {'limit':>6}  cold/warm  runs [range of runs]"]
    hist = []
    for s, rs in data.items():
        for name, label, unit, i in CELLS:
            cold = [r["cells"][name][0] for r in rs]
            warm = [statistics.median(r["cells"][name][1]) for r in rs]
            mc, mw = statistics.median(cold), statistics.median(warm)
            single = name == "instances" and rs[0]["instances"] < 100  # B-31's per-10k limit is S2's
            if single:  # shown as plain ms for the one instance
                f = rs[0]["instances"] / 1e4
                cold, warm, mc, mw = [v * f for v in cold], [v * f for v in warm], mc * f, mw * f
            limit = None if single else lim[i]
            verdict = "n/a" if single else "/".join("PASS" if v <= limit else "MISS" for v in (mc, mw))
            u = "ms (1 inst)" if single else unit
            rows.append(f"{s:<5} {label:<15} {u[:11]:<11} {fmt(mc):>8} {fmt(mw):>8} {'-' if single else f'{limit:g}':>6}  "
                        f"{verdict:<9}  {len(rs)} [cold {fmt(min(cold))}-{fmt(max(cold))}, warm {fmt(min(warm))}-{fmt(max(warm))}]")
            hu = "ms" if single else unit
            hist += [(s, f"b31.{name}_cold", mc, hu, len(cold), max(cold) - min(cold)),
                     (s, f"b31.{name}_warm", mw, hu, len(warm), max(warm) - min(warm))]
    log = os.path.join(ROOT, "out", "check", "sync_timing.log")
    os.makedirs(os.path.dirname(log), exist_ok=True)
    full = list(rows)
    for s, rs in data.items():
        for r in rs:
            full.append(f"{s} run: " + json.dumps(r["cells"]))
            if r.get("profile"):
                full.append(r["profile"])
    with open(log, "w") as f:
        f.write("\n".join(full) + "\n")
    print("\n".join(rows[:20]))
    if profile:
        for r in data.get("b5m", [])[-1:]:
            print(r.get("profile", "no profile in the run"))
    if history:
        def git(*a):
            return subprocess.run(["git", "-C", ROOT, *a], capture_output=True, text=True, check=True).stdout.strip()
        with open(os.path.join(ROOT, "bench", "history", "2026.jsonl")) as f:
            last = json.loads(f.read().splitlines()[-1])
        now = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        out = os.path.join(ROOT, "out", "bench", f"history-{now[:10].replace('-', '')}.jsonl")
        os.makedirs(os.path.dirname(out), exist_ok=True)
        sha = git("rev-parse", "HEAD")
        with open(out, "a", newline="\n") as f:
            for s, metric, v, u, n, spread in hist:
                f.write(json.dumps({"sha": sha, "machine": last["machine"], "driver": last["driver"],
                                    "cuda": last["cuda"], "scene": s, "metric": metric, "value": round(v, 4),
                                    "unit": u, "n": n, "spread": round(spread, 4), "date": now}) + "\n")
        print(f"history lines appended to {out}")


def main():
    if "--" in sys.argv:  # inside Blender: its own arguments precede the `--`
        a = sys.argv[sys.argv.index("--") + 1:]
    else:
        a = sys.argv[1:]
    a = [x for x in a if not x.startswith("--")]
    if a[:1] == ["measure"] and len(a) == 4:
        measure(*a[1:])
    elif a[:1] == ["report"] and len(a) == 2:
        report(a[1], "--history" in sys.argv, "--profile" in sys.argv)
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
