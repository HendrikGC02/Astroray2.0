"""Which mesh reads hit a fast (memcpy-class) path in Blender 5.3? Run: blender -b <scene.blend> --python probe_fastpaths.py -- S1|S2
Writes out/fastpaths_<scene>.json."""
import os, sys, time, json
import numpy as np
import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
which = sys.argv[sys.argv.index("--") + 1]
T = time.perf_counter
me = max((o.data for o in bpy.context.scene.objects if o.type == 'MESH'), key=lambda m: len(m.vertices))
nv, nl, nf = len(me.vertices), len(me.loops), len(me.polygons)
me.calc_loop_triangles()
nt = len(me.loop_triangles)
out = {"mesh": me.name, "verts": nv, "loops": nl, "faces": nf, "tris": nt,
       "attributes": [(a.name, a.domain, a.data_type) for a in me.attributes]}
res = out.setdefault("ms", {})


def t(label, fn, n=5):
    ts = []
    for _ in range(n):
        a = T()
        try:
            fn()
        except Exception as e:
            res[label] = f"ERROR {type(e).__name__}: {str(e)[:100]}"
            return
        ts.append((T() - a) * 1e3)
    res[label] = {"min": min(ts), "median": sorted(ts)[len(ts) // 2]}


i32_l = np.empty(nl, np.int32); i32_f = np.empty(nf, np.int32); i32_t = np.empty(nt * 3, np.int32); f32_v = np.empty(nv * 3, np.float32)
t("vertices.foreach_get('co') f32", lambda: me.vertices.foreach_get("co", f32_v))
t("attributes['position'].data.foreach_get('vector') f32", lambda: me.attributes["position"].data.foreach_get("vector", f32_v))
t("attributes['.corner_vert'].data.foreach_get('value') i32", lambda: me.attributes[".corner_vert"].data.foreach_get("value", i32_l))
t("loops.foreach_get('vertex_index') i32", lambda: me.loops.foreach_get("vertex_index", i32_l))
t("loops.foreach_get('edge_index') i32", lambda: me.loops.foreach_get("edge_index", i32_l))
t("polygons.foreach_get('loop_start') i32", lambda: me.polygons.foreach_get("loop_start", i32_f))
t("polygons.foreach_get('loop_total') i32", lambda: me.polygons.foreach_get("loop_total", i32_f))
t("polygons.foreach_get('material_index') i32", lambda: me.polygons.foreach_get("material_index", i32_f))
t("polygons.foreach_get('use_smooth') bool", lambda: me.polygons.foreach_get("use_smooth", np.empty(nf, np.bool_)))
t("attributes['material_index'].data.foreach_get('value')", lambda: me.attributes["material_index"].data.foreach_get("value", i32_f))
t("attributes['sharp_face'].data.foreach_get('value')", lambda: me.attributes["sharp_face"].data.foreach_get("value", np.empty(nf, np.bool_)))
t("loop_triangles.foreach_get('vertices') i32", lambda: me.loop_triangles.foreach_get("vertices", i32_t))
t("loop_triangles.foreach_get('polygon_index') i32", lambda: me.loop_triangles.foreach_get("polygon_index", np.empty(nt, np.int32)))
t("loop_triangle_polygons.foreach_get('value') i32", lambda: me.loop_triangle_polygons.foreach_get("value", np.empty(nt, np.int32)))
t("corner_normals.foreach_get('vector') f32", lambda: me.corner_normals.foreach_get("vector", np.empty(nl * 3, np.float32)))
t("polygon_normals.foreach_get('vector') f32", lambda: me.polygon_normals.foreach_get("vector", np.empty(nf * 3, np.float32)))
t("vertex_normals.foreach_get('vector') f32", lambda: me.vertex_normals.foreach_get("vector", f32_v))
for uvl in me.uv_layers:
    nm = uvl.name
    t(f"uv_layers[{nm}].uv.foreach_get('vector') f32", lambda: uvl.uv.foreach_get("vector", np.empty(nl * 2, np.float32)))
    t(f"attributes[{nm}].data.foreach_get('vector') f32", lambda: me.attributes[nm].data.foreach_get("vector", np.empty(nl * 2, np.float32)))
t("calc_loop_triangles() after fresh geometry tag (cached)", lambda: me.calc_loop_triangles())
json.dump(out, open(os.path.join(HERE, "out", f"fastpaths_{which}.json"), "w"), indent=1)
print("FASTPATHS_DONE")
os._exit(0)
