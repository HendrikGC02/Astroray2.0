"""Engine-style scene sync into the native module (spike B, throwaway). Shared by sync_bench.py and gui_edits.py.
Scene name (S1|S2) is read from the first argument after '--' to size the scratch buffers."""
import sys, time
import numpy as np
import bpy
import sbn

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else ["S1"]
which = argv[0] if argv and argv[0] in ("S1", "S2") else "S1"
T = time.perf_counter


def ms(t0):
    return (T() - t0) * 1e3


obj_index = {}   # object name -> instance index (non-instanced objects), filled by full_sync
mesh_ids = {}    # mesh session_uid -> native mesh id
state = {"n_inst": 0}
# ------------------------------------------------------------------ scratch buffers (exact dtypes) for foreach_get
MAXV, MAXL, MAXT = 600_000, 2_100_000, 1_100_000
if which == "S2":
    MAXV, MAXL, MAXT = 4000, 12000, 6000
pos_s = np.empty(MAXV * 3, np.float32)
tri_s = np.empty(MAXT * 3, np.int32)
tlp_s = np.empty(MAXT * 3, np.int32)
tmt_s = np.empty(MAXT, np.int32)
nrm_s = np.empty(MAXL * 3, np.float32)
uv_s = np.empty(MAXL * 2, np.float32)
cv_s = np.empty(MAXL, np.int32)
fs_s = np.empty(MAXT + 1, np.int32)
fm_s = np.empty(MAXT, np.int32)


def uv_collection(me):
    return me.uv_layers.active.uv if me.uv_layers.active else None


def extract_mesh(me, t):
    """Reads one mesh into the scratch arrays; accumulates per-step ms in dict t. Returns (nv, nt, nl)."""
    a = T(); me.calc_loop_triangles(); t["calc_loop_triangles"] += ms(a)
    nv, nl, nt = len(me.vertices), len(me.loops), len(me.loop_triangles)
    a = T(); me.vertices.foreach_get("co", pos_s[:nv * 3]); t["positions"] += ms(a)
    a = T(); me.loop_triangles.foreach_get("vertices", tri_s[:nt * 3]); t["tri_vertices"] += ms(a)
    a = T(); me.loop_triangles.foreach_get("loops", tlp_s[:nt * 3]); t["tri_loops"] += ms(a)
    a = T(); me.loop_triangles.foreach_get("material_index", tmt_s[:nt]); t["tri_material"] += ms(a)
    a = T(); me.corner_normals.foreach_get("vector", nrm_s[:nl * 3]); t["corner_normals"] += ms(a)
    uvc = uv_collection(me)
    a = T()
    if uvc is not None:
        uvc.foreach_get("vector", uv_s[:nl * 2])
    t["uv"] += ms(a)
    return nv, nt, nl


def extract_mesh_fast(me, t):
    """Fast read path: float-vector attributes (memcpy-class), raw corner_vert + face starts; triangulation is native.
    Returns (nv, nf, nl, has_uv, has_mat)."""
    nv, nl, nf = len(me.vertices), len(me.loops), len(me.polygons)
    a = T(); me.attributes["position"].data.foreach_get("vector", pos_s[:nv * 3]); t["positions"] += ms(a)
    a = T(); me.attributes[".corner_vert"].data.foreach_get("value", cv_s[:nl]); t["corner_vert"] += ms(a)
    a = T(); me.polygons.foreach_get("loop_start", fs_s[:nf]); fs_s[nf] = nl; t["face_start"] += ms(a)
    has_mat = len(me.materials) > 1
    a = T()
    if has_mat:
        me.polygons.foreach_get("material_index", fm_s[:nf])
    t["face_material"] += ms(a)
    a = T(); me.corner_normals.foreach_get("vector", nrm_s[:nl * 3]); t["corner_normals"] += ms(a)
    a = T()
    uvl = me.uv_layers.active
    has_uv = uvl is not None
    if has_uv:
        me.attributes[uvl.name].data.foreach_get("vector", uv_s[:nl * 2])
    t["uv"] += ms(a)
    return nv, nf, nl, has_uv, has_mat


def set_mesh_fast(scene_h, mid, nv, nf, nl, has_uv, has_mat):
    return sbn.scene_set_mesh_poly(scene_h, mid, sbn.ptr(pos_s), nv, sbn.ptr(cv_s), nl, sbn.ptr(fs_s), nf, sbn.ptr(nrm_s),
                                   sbn.ptr(uv_s) if has_uv else None, sbn.ptr(fm_s) if has_mat else None)


STEPS = ["calc_loop_triangles", "positions", "tri_vertices", "tri_loops", "tri_material", "corner_normals", "uv"]


def full_sync(dg, scene_h, fast=False):
    """What an engine's first view_update would do. Returns dict of ms per phase."""
    from collections import defaultdict
    t = defaultdict(float)
    ph = {"strategy": "fast" if fast else "loop_triangles"}
    t_all = T()
    # --- phase 1: iterate instances, collect per-instance transform + mesh key
    a = T()
    n_est = 12000 if which == "S2" else 4
    xf = np.empty((n_est, 16), np.float32)
    mids = np.empty(n_est, np.int32)
    seen = mesh_ids
    seen.clear(); obj_index.clear()
    unique_me = []
    i = 0
    for inst in dg.object_instances:
        ob = inst.object
        if ob.type != 'MESH':
            continue
        me = ob.data
        key = me.original.session_uid   # NOT me.session_uid: evaluated copies share very few session_uids (S2: 2 of 2001)
        mid = seen.get(key)
        if mid is None:
            mid = len(unique_me)
            seen[key] = mid
            unique_me.append(me)
        if i >= n_est:
            xf = np.resize(xf, (i * 2, 16)); mids = np.resize(mids, i * 2); n_est = i * 2
        xf[i] = np.asarray(inst.matrix_world, np.float32).reshape(16)
        mids[i] = mid
        if not inst.is_instance:
            obj_index[ob.name] = i
        i += 1
    ph["iterate_instances"] = ms(a)
    n_inst = i
    # --- phase 2: per unique mesh extract + hand to native
    copy_ms = 0.0
    call_ms = 0.0
    tot_tris = 0
    for mid, me in enumerate(unique_me):
        if fast:
            nv, nf, nl, has_uv, has_mat = extract_mesh_fast(me, t)
            a = T()
            c = set_mesh_fast(scene_h, mid, nv, nf, nl, has_uv, has_mat)
            nt = 0  # triangle count is known natively (scene_totals)
        else:
            nv, nt, nl = extract_mesh(me, t)
            a = T()
            c = sbn.scene_set_mesh(scene_h, mid, sbn.ptr(pos_s), nv, sbn.ptr(tri_s), nt, sbn.ptr(nrm_s), nl,
                                   sbn.ptr(uv_s), nl, sbn.ptr(tlp_s), sbn.ptr(tmt_s))
        call_ms += ms(a)
        copy_ms += c
        tot_tris += nt
    ph["native_copy_ms"] = copy_ms   # fast strategy: copy + native triangulation
    ph["native_call_total_ms"] = call_ms   # includes ctypes overhead + the copy
    a = T()
    sbn.scene_set_objects(scene_h, n_inst, sbn.ptr(mids), sbn.ptr(np.ascontiguousarray(xf[:n_inst])))
    ph["set_objects"] = ms(a)
    ph.update({"extract_" + k: v for k, v in t.items()})
    ph["extract_total"] = sum(t.values())
    ph["total"] = ms(t_all)
    ph["n_instances"] = n_inst
    ph["n_unique_meshes"] = len(unique_me)
    ph["unique_tris"] = tot_tris
    return ph


def run_full_syncs(dg, n, fast=False):
    res = []
    h = None
    for k in range(n):
        h = sbn.scene_create()
        res.append(full_sync(dg, h, fast))
        if k < n - 1:
            sbn.scene_destroy(h)
    return res, h
