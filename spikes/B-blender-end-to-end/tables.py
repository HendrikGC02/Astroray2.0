"""Renders out/*.json into Markdown tables (out/tables.md). Plain Python, json only."""
import json, os, glob, sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")
lines = []


def P(s=""):
    lines.append(s)


def load(name):
    p = os.path.join(OUT, name)
    return json.load(open(p)) if os.path.exists(p) else None


def f(x, n=2):
    return "-" if x is None else (f"{x:.{n}f}" if isinstance(x, (int, float)) else str(x))


def present_table(tag, title):
    j = load(f"gui_present_{tag}.json")
    if not j:
        return
    P(f"### {title}")
    P(f"Region obtained: {j.get('region')}, window {j.get('window')}, errors: {len(j.get('errors', []))}, quit: {j.get('quit_reason')}")
    P()
    P("| config | wrap ms | tex/upload ms (med) | draw ms | view_draw ms min / med / p99 | redraw interval ms (med) | view_draw calls/s | new frames shown/s | producer fps | worst producer gap ms | readback ok |")
    P("|---|---|---|---|---|---|---|---|---|---|---|")
    for c in j.get("configs", []):
        t = c.get("view_draw_total_ms", {})
        P(f"| {c['name']} | {f(c['wrap_ms'].get('median'), 3)} | {f(c['texture_ms'].get('median'))} | {f(c['draw_ms'].get('median'), 3)} | "
          f"{f(t.get('min'), 1)} / {f(t.get('median'), 1)} / {f(t.get('p99'), 1)} | {f(c['interval_ms'].get('median'))} | "
          f"{f(c.get('view_draw_calls_per_s'), 1)} | {f(c.get('distinct_source_frames_shown_per_s'), 1)} | {f(c.get('producer_frames_per_s'), 1)} | "
          f"{f(c['producer'].get('gap_ms_max'), 1)} | {c.get('verify_ok')} |")
    P()


def latency_table():
    j = load("gui_present_latency.json")
    if not j:
        return
    P("### Latency: native 'frame ready' -> view_draw")
    P(f"Region {j.get('region')}, errors {len(j.get('errors', []))}, quit {j.get('quit_reason')}")
    P()
    P("| config | frames shown/s | producer fps | swap->draw start ms min / med / p95 / max | swap->draw end (CPU) ms med / p95 | view_draw ms med |")
    P("|---|---|---|---|---|---|")
    for c in j.get("configs", []):
        a = c.get("latency_swap_to_draw_start_ms", {})
        b = c.get("latency_swap_to_draw_end_cpu_ms", {})
        P(f"| {c['name']} | {f(c.get('distinct_source_frames_shown_per_s'), 1)} | {f(c.get('producer_frames_per_s'), 1)} | "
          f"{f(a.get('min'), 1)} / {f(a.get('median'), 1)} / {f(a.get('p95'), 1)} / {f(a.get('max'), 1)} | {f(b.get('median'), 1)} / {f(b.get('p95'), 1)} | {f(c['view_draw_total_ms'].get('median'), 2)} |")
    P()


def sweep_table(tag=""):
    j = load(f"gui_present_sweep{tag}.json")
    if not j:
        return
    P(f"### Host-to-GPU upload cost vs size (back-to-back, 12 repeats) [run {tag or '1'}]")
    P("| MB | GPUStorageBuf.update ms min / med | GB/s at min | GPUTexture(data=) RGBA32F ms min / med | GB/s at min |")
    P("|---|---|---|---|---|")
    for r in j.get("sweep", []):
        P(f"| {r['MB']} | {f(r['ssbo_update_ms_min'])} / {f(r['ssbo_update_ms_med'])} | {f(r['ssbo_GBps_at_min'], 1)} | {f(r['tex_create_ms_min'])} / {f(r['tex_create_ms_med'])} | {f(r['tex_GBps_at_min'], 1)} |")
    P()


def sync_table(which):
    j = load(f"sync_{which}.json")
    if not j:
        return
    P(f"### Full sync, {which} (background Blender, native copy-in)")
    P(f"open_mainfile {f(j.get('open_mainfile_ms'), 0)} ms; evaluated_depsgraph_get {f(j.get('depsgraph_first_get_ms'))} ms; "
      f"depsgraph.update() {f(j.get('depsgraph_update_ms_cold'))} ms; tag-all-objects + view_layer.update() runs {j.get('depsgraph_retag_all_eval_ms', {}).get('all')}")
    P()
    for prefix, label in (("", "loop_triangles strategy (naive)"), ("fast_", "fast attribute strategy + native fan triangulation")):
        c = j.get(prefix + "full_sync_cold", {})
        wmin = j.get(prefix + "full_sync_warm_min", {})
        wmed = j.get(prefix + "full_sync_warm_median", {})
        if not c:
            continue
        P(f"#### {which}: {label}; native totals {j.get(prefix + 'native_scene_totals') or j.get('native_scene_totals')}")
        P(f"instances {c.get('n_instances')}, unique meshes {c.get('n_unique_meshes')}")
        P()
        P("| phase | 1st sync in this process ms | warm min ms | warm median ms |")
        P("|---|---|---|---|")
        keys = ["iterate_instances"] + sorted(k for k in c if k.startswith("extract_") and k != "extract_total") + ["extract_total", "native_copy_ms", "native_call_total_ms", "set_objects", "total"]
        for k in keys:
            P(f"| {k} | {f(c.get(k))} | {f(wmin.get(k))} | {f(wmed.get(k))} |")
        P()
    qs = []
    for strat in ("naive", "fast"):
        rows = []
        for fn in sorted(glob.glob(os.path.join(OUT, f"sync_{which}_quick_{strat}_*.json"))):
            q = json.load(open(fn))
            c = q.get(("fast_" if strat == "fast" else "") + "full_sync_cold", {})
            g = q.get("gas_cold", {})
            rows.append((c.get("total"), c.get("iterate_instances"), c.get("extract_total"), c.get("native_copy_ms"), g.get("total_ms"), q.get("open_mainfile_ms")))
        if rows:
            P(f"Cold full sync, fresh process, strategy {strat} (3 runs; total / iterate / extract / native copy / GAS+TLAS total ms):")
            for r in rows:
                P("- " + " / ".join(f(x, 1) for x in r))
            P()
    g = j.get("gas_cold")
    if g:
        P("OptiX GAS+TLAS from the synced data (cold = first GAS build in the process):")
        P("| step | cold | warm min | warm median |")
        P("|---|---|---|---|")
        gm = j.get("gas_warm_min", {}); gd = j.get("gas_warm_median", {})
        for k in ["h2d_ms", "gas_build_ms", "compaction_ms", "tlas_build_ms", "total_ms", "gas_bytes_raw", "gas_bytes_compacted", "tlas_bytes", "n_gas", "n_instances"]:
            P(f"| {k} | {f(g.get(k), 1)} | {f(gm.get(k), 1)} | {f(gd.get(k), 1)} |")
        P()
    fp = load(f"fastpaths_{which}.json")
    if fp:
        P(f"foreach_get fast-path probe on {fp['mesh']} ({fp['verts']} verts, {fp['loops']} corners, {fp['faces']} faces, {fp['tris']} tris); attributes {fp['attributes']}")
        P("| read | min ms | median ms |")
        P("|---|---|---|")
        for k, v in fp["ms"].items():
            P(f"| {k} | " + (f"{f(v['min'], 3)} | {f(v['median'], 3)} |" if isinstance(v, dict) else f"{v} | |"))
        P()
    m = j.get("micro", {})
    if m:
        P(f"#### Micro-benchmarks, {which} ({m.get('n_object_instances')} object instances)")
        P("| loop variant | total ms (min) | ns / instance |")
        P("|---|---|---|")
        for k, v in m.items():
            if isinstance(v, dict) and "ns_per_instance_min" in v:
                P(f"| {k} | {f(v['total_ms_min'])} | {f(v['ns_per_instance_min'], 0)} |")
            elif isinstance(v, dict) and "error" in v:
                P(f"| {k} | error | {v['error']} |")
        P()
        fg = m.get("foreach_get_ms", {})
        P(f"foreach_get on mesh {m.get('fg_mesh')}")
        P("| call | min ms | median ms |")
        P("|---|---|---|")
        for k, v in fg.items():
            P(f"| {k} | {f(v['min'], 3)} | {f(v['median'], 3)} |")
        P()
        for k in ("scene.objects+asarray(matrix_world)_ms", "ob.data x300_ms", "ob.to_mesh()+clear x300_ms", "per_call_ob.to_mesh_us"):
            if k in m:
                P(f"- {k}: {f(m[k], 3)}")
        P()


def edits_table(which):
    j = load(f"gui_edits_{which}.json")
    if not j:
        return
    fv = j.get("first_view_update", {})
    P(f"### Incremental edits and first view_update in the GUI, {which}")
    P(f"errors {len(j.get('errors', []))}, region {j.get('region')}; script start -> first view_update {f(fv.get('script_start_to_first_view_update_s'), 2)} s (incl. ~0.5 s timer delay); "
      f"full sync inside view_update: {f(fv.get('view_update_total_ms'), 0)} ms")
    P()
    P("| edit | view_update called | Blender edit -> view_update entry ms | depsgraph.updates (n) | parse ms | our resync ms | what Blender reported | resync actions (ms) |")
    P("|---|---|---|---|---|---|---|---|")
    for e in j.get("edits", []):
        ups = "; ".join(f"{u['type']}:{u['name']}[{'G' if u['geometry'] else ''}{'S' if u['shading'] else ''}{'T' if u['transform'] else ''}]" for u in e.get("updates", [])[:6])
        acts = ", ".join(f"{k}={f(v, 2)}" for k, v in e.get("resync_actions_ms", {}).items())
        P(f"| {e['edit']} | {e.get('view_update_called')} | {f(e.get('blender_edit_to_view_update_ms'), 1)} | {e.get('n_updates')} | {f(e.get('parse_depsgraph_updates_ms'), 3)} | {f(e.get('resync_total_ms'), 2)} | {ups} | {acts} |")
    P()


def cycles_log(which):
    import re
    p = os.path.join(OUT, f"cycles_{which}.log")
    if not os.path.exists(p):
        return []
    txt = open(p, encoding="utf-8", errors="replace").read()
    tot = [float(x) for x in re.findall(r"Total render time: ([0-9.]+)", txt)]
    nos = [float(x) for x in re.findall(r"Render time \(without synchronization\): ([0-9.]+)", txt)]
    syn = [float(x) for x in re.findall(r"Total time spent synchronizing data: ([0-9.]+)", txt)]
    return list(zip(tot, nos, syn))


def cycles_table(which):
    j = load(f"cycles_{which}.json")
    if not j:
        return
    lg = cycles_log(which)
    if lg:
        P(f"Cycles own log ({which}): render # / Total render time s / render without sync s / pre-first-sample = difference s / Blender->Cycles data sync s")
        for i, (a, b, c) in enumerate(lg):
            P(f"- {i + 1}: {a:.3f} / {b:.3f} / {a - b:.3f} / {c:.3f}")
        P()
    P(f"### Cycles OptiX 1080p, {which}")
    P(f"devices used: {j.get('devices_after')}")
    P("| render | samples | render op ms |")
    P("|---|---|---|")
    for i, r in enumerate(j.get("renders", [])):
        P(f"| {i + 1} | {r['samples']} | {f(r['render_op_ms'], 0)} |")
    P()


if __name__ == "__main__":
    present_table("matrix", "Present matrix (small window; texture sizes as stated)")
    present_table("stall", "Producer free-running vs viewport redraw")
    present_table("paced", "Paced producer: can the pipeline hold N updates/s")
    latency_table()
    sweep_table()
    sweep_table("_r2")
    sweep_table("_r3")
    present_table("matrix_fullregion", "Present matrix, viewport maximised (real region)")
    for w in ("S1", "S2"):
        sync_table(w)
        edits_table(w)
        cycles_table(w)
    open(os.path.join(OUT, "tables.md"), "w", encoding="utf-8").write("\n".join(lines))
    print("\n".join(lines))

