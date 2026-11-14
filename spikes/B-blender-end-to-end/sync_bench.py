"""Q2/Q3: Python RenderEngine-style full sync of S1/S2 into the native module, with breakdowns. Background Blender.
Run: blender -b --factory-startup --python sync_bench.py -- S1|S2 [quick=1] [gas=0]
quick=1: only open + cold depsgraph + one cold full sync (for run-to-run cold repeats)."""
import os, sys, time, json
import numpy as np
import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sbn

argv = sys.argv[sys.argv.index("--") + 1:]
which = argv[0]
kv = dict(a.split("=", 1) for a in argv[1:] if "=" in a)
QUICK = kv.get("quick", "0") == "1"
OUT = {"scene": which, "quick": QUICK, "blender": bpy.app.version_string}
T = time.perf_counter


def ms(t0):
    return (T() - t0) * 1e3


def rep(fn, n=5):
    """run fn n times; returns dict of min/median of its returned-or-measured ms"""
    ts = []
    for _ in range(n):
        t0 = T()
        r = fn()
        ts.append(r if isinstance(r, (int, float)) else ms(t0))
    ts.sort()
    return {"min": ts[0], "median": ts[len(ts) // 2], "all": ts}


# ------------------------------------------------------------------ open + depsgraph
t0 = T()
bpy.ops.wm.open_mainfile(filepath=os.path.join(HERE, "scenes", f"{which}.blend"))
OUT["open_mainfile_ms"] = ms(t0)
scene = bpy.context.scene
view_layer = bpy.context.view_layer

t0 = T()
dg = bpy.context.evaluated_depsgraph_get()
OUT["depsgraph_first_get_ms"] = ms(t0)        # cold: builds the depsgraph relations; evaluation may be lazy
t0 = T()
dg.update()
OUT["depsgraph_update_ms_cold"] = ms(t0)      # forces evaluation of everything still dirty
if not QUICK:
    def tag_all_and_eval():
        for ob in scene.objects:
            ob.update_tag()
        t = T()
        view_layer.update()
        return ms(t)
    t0 = T()
    OUT["depsgraph_retag_all_eval_ms"] = rep(tag_all_and_eval, 3)

# ------------------------------------------------------------------ shared sync code (syncmod.py)
import syncmod as SM
from syncmod import (MAXV, MAXL, MAXT, pos_s, tri_s, tlp_s, tmt_s, nrm_s, uv_s, uv_collection, extract_mesh, STEPS, full_sync)
STRAT_FAST = kv.get("strat", "naive") == "fast"


def summarize(prefix, runs):
    OUT[prefix + "full_sync_cold"] = runs[0]
    if len(runs) > 1:
        OUT[prefix + "full_sync_warm_runs_total_ms"] = [r["total"] for r in runs[1:]]
        keys = [k for k in runs[1] if isinstance(runs[1][k], float)]
        OUT[prefix + "full_sync_warm_min"] = {k: min(r[k] for r in runs[1:]) for k in keys}
        OUT[prefix + "full_sync_warm_median"] = {k: sorted(r[k] for r in runs[1:])[len(runs[1:]) // 2] for k in keys}


def totals(h):
    tot = np.zeros(4, np.int64)
    sbn.scene_totals(h, tot.ctypes.data)
    return {"meshes": int(tot[0]), "tris": int(tot[1]), "objects": int(tot[2]), "instanced_tris": int(tot[3])}


if QUICK:
    runs, scene_h = SM.run_full_syncs(dg, 1, STRAT_FAST)
    summarize("fast_" if STRAT_FAST else "", runs)
else:
    runs, scene_h = SM.run_full_syncs(dg, 5, False)            # naive loop_triangles strategy; run 1 is the cold one
    summarize("", runs)
    OUT["native_scene_totals"] = totals(scene_h)
    runs_f, scene_hf = SM.run_full_syncs(dg, 5, True)           # fast attribute strategy (warm-process; cold from quick runs)
    summarize("fast_", runs_f)
    OUT["fast_native_scene_totals"] = totals(scene_hf)
    sbn.scene_destroy(scene_h)
    scene_h = scene_hf
# ------------------------------------------------------------------ OptiX GAS/TLAS over the synced data
if kv.get("gas", "1") == "1":
    rc = sbn.gas_init()
    OUT["gas_init_rc"] = rc
    if rc == 0:
        outs = []
        for k in range(1 if QUICK else 4):
            o = (sbn.D * 10)()
            r = sbn.gas_build(scene_h, o)
            if r != 0:
                OUT["gas_error"] = (r, (sbn.gas_error() or b"").decode())
                break
            outs.append(list(o))
        if outs:
            names = ["h2d_ms", "gas_build_ms", "compaction_ms", "tlas_build_ms", "total_ms", "gas_bytes_raw", "gas_bytes_compacted", "tlas_bytes", "n_gas", "n_instances"]
            OUT["gas_cold"] = dict(zip(names, outs[0]))
            if len(outs) > 1:
                OUT["gas_warm_min"] = {n: min(o[j] for o in outs[1:]) for j, n in enumerate(names)}
                OUT["gas_warm_median"] = {n: sorted(o[j] for o in outs[1:])[len(outs[1:]) // 2] for j, n in enumerate(names)}

if QUICK:
    json.dump(OUT, open(os.path.join(HERE, "out", f"sync_{which}_quick_{kv.get('strat', 'naive')}_{kv.get('run', '0')}.json"), "w"), indent=1)
    print("SYNC_DONE", which, "quick")
    sys.stdout.flush()
    os._exit(0)

# ------------------------------------------------------------------ micro-benchmarks
micro = {}
OUT["micro"] = micro
insts = list(dg.object_instances)
micro["n_object_instances"] = len(insts)


def loop_bare():
    n = 0
    for inst in dg.object_instances:
        n += 1
    return n


def per_inst(fn, label, n=3):
    cnt = len(insts)

    def run():
        t = T()
        fn()
        return (T() - t) * 1e3
    r = rep(run, n)
    micro[label] = {"total_ms_min": r["min"], "total_ms_median": r["median"], "ns_per_instance_min": r["min"] * 1e6 / cnt}


per_inst(loop_bare, "iterate_bare")


def l_obj():
    for inst in dg.object_instances:
        ob = inst.object
per_inst(l_obj, "iterate+inst.object")


def l_type():
    for inst in dg.object_instances:
        ob = inst.object
        ob.type
per_inst(l_type, "iterate+object.type")


def l_data_uid():
    for inst in dg.object_instances:
        ob = inst.object
        if ob.type == 'MESH':
            ob.data.session_uid
per_inst(l_data_uid, "iterate+object.data.session_uid")


def l_data_orig_uid():
    for inst in dg.object_instances:
        ob = inst.object
        if ob.type == 'MESH':
            ob.data.original.session_uid
per_inst(l_data_orig_uid, "iterate+object.data.original.session_uid (the key used)")


def l_data_ptr():
    for inst in dg.object_instances:
        ob = inst.object
        if ob.type == 'MESH':
            ob.data.as_pointer()
per_inst(l_data_ptr, "iterate+object.data.as_pointer()")


def l_data_name():
    for inst in dg.object_instances:
        ob = inst.object
        if ob.type == 'MESH':
            ob.data.name
per_inst(l_data_name, "iterate+object.data.name")


def l_mw_np():
    for inst in dg.object_instances:
        np.array(inst.matrix_world)
per_inst(l_mw_np, "iterate+np.array(matrix_world)")


def l_mw_asarray_f32():
    for inst in dg.object_instances:
        np.asarray(inst.matrix_world, np.float32)
per_inst(l_mw_asarray_f32, "iterate+np.asarray(matrix_world,f32)")


buf16 = np.empty((len(insts), 16), np.float32)


def l_mw_assign():
    i = 0
    for inst in dg.object_instances:
        buf16[i] = np.asarray(inst.matrix_world, np.float32).reshape(16)
        i += 1
per_inst(l_mw_assign, "iterate+assign row (asarray.reshape)")


def l_mw_flat_list():
    for inst in dg.object_instances:
        mw = inst.matrix_world
        [mw[0][0], mw[0][1], mw[0][2], mw[0][3], mw[1][0], mw[1][1], mw[1][2], mw[1][3], mw[2][0], mw[2][1], mw[2][2], mw[2][3]]
per_inst(l_mw_flat_list, "iterate+12 scalar reads of matrix_world")


def l_mw_memoryview():
    for inst in dg.object_instances:
        np.frombuffer(memoryview(inst.matrix_world), dtype=np.float32)
try:
    per_inst(l_mw_memoryview, "iterate+memoryview(matrix_world) (buffer protocol)")
except Exception as e:
    micro["iterate+memoryview(matrix_world) (buffer protocol)"] = {"error": f"{type(e).__name__}: {e}"}


def l_persist():
    for inst in dg.object_instances:
        inst.is_instance, inst.persistent_id
per_inst(l_persist, "iterate+is_instance+persistent_id")

# originals (non-evaluated) object loop: scene.objects + obj.matrix_world, the no-instance fast loop
objs = list(scene.objects)
micro["scene_objects"] = len(objs)
t = T()
for ob in objs:
    np.asarray(ob.matrix_world, np.float32)
micro["scene.objects+asarray(matrix_world)_ms"] = ms(t)

# ---- foreach_get exact vs mismatched dtype, on the biggest mesh (S1) or a typical S2 mesh
me = max((ob.data for ob in objs if ob.type == 'MESH'), key=lambda m: len(m.vertices))
me.calc_loop_triangles()
nv, nl, nt = len(me.vertices), len(me.loops), len(me.loop_triangles)
micro["fg_mesh"] = {"name": me.name, "verts": nv, "loops": nl, "tris": nt}
fg = {}
micro["foreach_get_ms"] = fg


def tm(label, fn, n=5):
    r = rep(fn, n)
    fg[label] = {"min": r["min"], "median": r["median"]}


a_f32 = np.empty(nv * 3, np.float32); a_f64 = np.empty(nv * 3, np.float64)
tm("vertices.co  f32 (exact)", lambda: me.vertices.foreach_get("co", a_f32))
tm("vertices.co  f64 (mismatch)", lambda: me.vertices.foreach_get("co", a_f64))
a_attr = np.empty(nv * 3, np.float32)
tm("attributes['position'].data.vector f32", lambda: me.attributes["position"].data.foreach_get("vector", a_attr))
b_i32 = np.empty(nt * 3, np.int32); b_i64 = np.empty(nt * 3, np.int64)
tm("loop_triangles.vertices i32 (exact)", lambda: me.loop_triangles.foreach_get("vertices", b_i32))
tm("loop_triangles.vertices i64 (mismatch)", lambda: me.loop_triangles.foreach_get("vertices", b_i64))
b_f32 = np.empty(nt * 3, np.float32)
tm("loop_triangles.vertices f32 (mismatch)", lambda: me.loop_triangles.foreach_get("vertices", b_f32))
c_f32 = np.empty(nl * 3, np.float32); c_f64 = np.empty(nl * 3, np.float64)
tm("corner_normals.vector f32 (exact)", lambda: me.corner_normals.foreach_get("vector", c_f32))
tm("corner_normals.vector f64 (mismatch)", lambda: me.corner_normals.foreach_get("vector", c_f64))
v_f32 = np.empty(nv * 3, np.float32)
tm("vertex_normals.vector f32", lambda: me.vertex_normals.foreach_get("vector", v_f32))
uvc = uv_collection(me)
if uvc is not None:
    u_f32 = np.empty(nl * 2, np.float32); u_f64 = np.empty(nl * 2, np.float64)
    tm("uv_layers.active.uv.vector f32 (exact)", lambda: uvc.foreach_get("vector", u_f32))
    tm("uv_layers.active.uv.vector f64 (mismatch)", lambda: uvc.foreach_get("vector", u_f64))
    uvname = me.uv_layers.active.name
    if uvname in me.attributes:
        tm("attributes[uv].data.vector f32", lambda: me.attributes[uvname].data.foreach_get("vector", u_f32))
tm("calc_loop_triangles (cached, 2nd call)", lambda: me.calc_loop_triangles())
tm("len(me.loop_triangles)", lambda: len(me.loop_triangles))
tm("np.array(me.vertices[i].co) 1000 reads (naive)", lambda: [np.array(me.vertices[i].co) for i in range(1000)], 3)
tm("native copy of whole mesh (sbn.scene_set_mesh) incl. ctypes", lambda: sbn.scene_set_mesh(scene_h, 0, sbn.ptr(pos_s), nv, sbn.ptr(tri_s), nt, sbn.ptr(nrm_s), nl, sbn.ptr(uv_s), nl, sbn.ptr(tlp_s), sbn.ptr(tmt_s)) if nv <= MAXV else 0.0)

# ---- evaluated mesh access paths for instances: ob.data (borrow) vs to_mesh() copy
def first_mesh_objects(n, fn):
    k = 0
    t = T()
    for inst in dg.object_instances:
        ob = inst.object
        if ob.type == 'MESH':
            fn(ob)
            k += 1
            if k >= n:
                break
    return ms(t), k


micro["ob.data x300_ms"], k1 = first_mesh_objects(300, lambda ob: ob.data)


def to_mesh_clear(ob):
    ob.to_mesh()
    ob.to_mesh_clear()


micro["ob.to_mesh()+clear x300_ms"], k2 = first_mesh_objects(300, to_mesh_clear)
micro["per_call_ob.to_mesh_us"] = micro["ob.to_mesh()+clear x300_ms"] * 1e3 / max(1, k2)
# ---- GAS timing summary already in OUT; native scene is kept alive until exit
json.dump(OUT, open(os.path.join(HERE, "out", f"sync_{which}.json"), "w"), indent=1)
print("SYNC_DONE", which)
sys.stdout.flush()
os._exit(0)

