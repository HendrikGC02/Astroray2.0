"""Q2: full first sync + incremental edits as seen from RenderEngine.view_update in a windowed isolated Blender.
Run: blender --factory-startup <scene.blend> --python gui_edits.py -- S1|S2
Edits are made by a timer (as a user/addon would), Blender calls view_update with depsgraph.updates, and we time
(a) edit -> view_update entry (Blender's side), (b) parsing depsgraph.updates, (c) our resync of just that change."""
import os, sys, time, traceback
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import bpy
import gui_common as G
import sbn
import syncmod as SM

T = time.perf_counter
ms = SM.ms
which = SM.which
G.WATCHDOG_S = 600.0


def principled_params(mat):
    b = next(n for n in mat.node_tree.nodes if n.type == 'BSDF_PRINCIPLED')
    bc = list(b.inputs["Base Color"].default_value)
    return np.array(bc + [b.inputs["Roughness"].default_value, b.inputs["Metallic"].default_value] + [0.0] * 10, np.float32)


class Edits:
    done = False

    def __init__(self):
        self.h = None
        self.first = True
        self.edit_i = -1
        self.cur = None
        self.results = G.OUT.setdefault("edits", [])
        self.mat_ids = {}
        self.t_script0 = T()
        self.prev_pos = None
        self.edits = self.make_edits()

    # ---------------------------------------------------------- the edits
    def make_edits(self):
        sc = bpy.context.scene
        o = bpy.data.objects
        edits = []
        if which == "S1":
            ob = o["S1_obj"]
            edits.append(("move one object", lambda: setattr(ob, "location", (ob.location.x + 0.25, ob.location.y, ob.location.z))))
            m = bpy.data.materials["S1_mat"]
            def mat():
                b = next(n for n in m.node_tree.nodes if n.type == 'BSDF_PRINCIPLED')
                b.inputs["Roughness"].default_value = 0.2
            edits.append(("change one material value", mat))
            me = bpy.data.meshes["S1_mesh"]
            def vert():
                me.vertices[12345].co.z += 0.1
                me.update()
            edits.append(("edit one vertex (1M-tri mesh)", vert))
            def add():
                nob = bpy.data.objects.new("NEWOBJ", me)
                sc.collection.objects.link(nob)
                nob.location = (3, 3, 3)
            edits.append(("add one object (linked dup of 1M-tri mesh)", add))
        else:
            ob = o["L0100"]
            edits.append(("move one object (linked duplicate)", lambda: setattr(ob, "location", (ob.location.x + 0.25, ob.location.y, ob.location.z))))
            em = o["E0007"]
            edits.append(("move one collection-instance empty", lambda: setattr(em, "location", (em.location.x + 0.25, em.location.y, em.location.z))))
            m = bpy.data.materials["M017"]
            def mat():
                b = next(n for n in m.node_tree.nodes if n.type == 'BSDF_PRINCIPLED')
                b.inputs["Roughness"].default_value = 0.2
            edits.append(("change one material value", mat))
            me = bpy.data.meshes["U0100"]
            def vert():
                me.vertices[10].co.z += 0.1
                me.update()
            edits.append(("edit one vertex (2.5k-tri mesh)", vert))
            def add():
                nob = bpy.data.objects.new("NEWOBJ", bpy.data.meshes["U0200"])
                sc.collection.objects.link(nob)
                nob.location = (3, 3, 3)
            edits.append(("add one object (linked dup)", add))
            def addnew():
                import bmesh
                bm = bmesh.new(); bmesh.ops.create_cube(bm, size=1.0)
                nme = bpy.data.meshes.new("NEWMESH"); bm.to_mesh(nme); bm.free()
                nob = bpy.data.objects.new("NEWOBJ2", nme)
                sc.collection.objects.link(nob)
            edits.append(("add one object with a new mesh", addnew))
        sel = o["S1_obj" if which == "S1" else "L0100"]
        edits.append(("selection change only", lambda: sel.select_set(True)))
        return edits

    # ---------------------------------------------------------- engine callbacks
    def view_draw(self, eng, ctx, dg):
        pass

    def view_update(self, eng, ctx, dg):
        t_enter = T()
        if self.first:
            self.first = False
            self.first_update(dg, t_enter)
            bpy.app.timers.register(self.next_edit, first_interval=1.5)
            return
        if self.cur is None:
            return
        try:
            self.handle_update(dg, t_enter)
        except Exception:
            G.OUT["errors"].append(traceback.format_exc())
        self.cur = None
        bpy.app.timers.register(self.next_edit, first_interval=0.6)

    def first_update(self, dg, t_enter):
        r = G.OUT.setdefault("first_view_update", {})
        r["script_start_to_first_view_update_s"] = t_enter - self.t_script0
        r["note"] = "includes ~0.5 s fixed timer delay before the viewport is switched to RENDERED"
        t0 = T()
        self.h = sbn.scene_create()
        ph = SM.full_sync(dg, self.h)
        r["full_sync_inside_view_update"] = ph
        r["view_update_total_ms"] = ms(t0)
        if which == "S1":
            me = bpy.data.meshes["S1_mesh"]
            self.prev_pos = np.empty(len(me.vertices) * 3, np.float32)
            me.vertices.foreach_get("co", self.prev_pos)
        G.save()

    def next_edit(self):
        self.edit_i += 1
        if self.edit_i >= len(self.edits):
            self.done = True
            G.OUT["edits_complete"] = True
            for w in bpy.context.window_manager.windows:
                for a in w.screen.areas:
                    a.tag_redraw()
            return None
        name, fn = self.edits[self.edit_i]
        self.cur = {"edit": name, "t_edit": T()}
        fn()
        self.cur["edit_call_ms"] = ms(self.cur["t_edit"])
        # if Blender never calls view_update (e.g. nothing tagged), time out
        i = self.edit_i
        def timeout():
            if self.cur is not None and self.edit_i == i:
                self.cur["view_update_called"] = False
                self.results.append(self.cur)
                self.cur = None
                G.save()
                bpy.app.timers.register(self.next_edit, first_interval=0.3)
            return None
        bpy.app.timers.register(timeout, first_interval=4.0)
        return None

    # ---------------------------------------------------------- the engine-side handling of one update
    def handle_update(self, dg, t_enter):
        c = self.cur
        c["view_update_called"] = True
        c["blender_edit_to_view_update_ms"] = (t_enter - c["t_edit"]) * 1e3
        a = T()
        ups = []
        for u in dg.updates:
            idb = u.id
            ups.append({"type": type(idb).__name__, "name": idb.name, "geometry": u.is_updated_geometry,
                        "shading": u.is_updated_shading, "transform": u.is_updated_transform})
        c["updates"] = ups
        c["parse_depsgraph_updates_ms"] = ms(a)
        c["n_updates"] = len(ups)
        acts = {}
        r0 = T()
        for u in dg.updates:
            idb = u.id
            if isinstance(idb, bpy.types.Object):
                if u.is_updated_transform and idb.type == 'MESH' and idb.name in SM.obj_index:
                    a = T()
                    m = np.asarray(idb.matrix_world, np.float32).reshape(16)
                    sbn.scene_set_xform(self.h, SM.obj_index[idb.name], sbn.ptr(m))
                    acts["set_xform_ms"] = acts.get("set_xform_ms", 0) + ms(a)
                elif u.is_updated_transform and idb.type == 'EMPTY' and idb.instance_type == 'COLLECTION':
                    # instancer moved: its instances are only reachable by re-iterating object_instances
                    a = T()
                    n = 0
                    xf = np.empty((20000, 16), np.float32)
                    for inst in dg.object_instances:
                        if inst.is_instance and inst.parent is not None and inst.parent.name == idb.name:
                            xf[n] = np.asarray(inst.matrix_world, np.float32).reshape(16)
                            n += 1
                    acts["empty_instancer_reiterate_all_instances_ms"] = ms(a)
                    acts["instances_found"] = n
                    a = T()
                    n2 = 0
                    for inst in dg.object_instances:
                        n2 += 1
                    acts["bare_iterate_object_instances_ms"] = ms(a)
                    acts["total_object_instances"] = n2
                elif idb.type == 'MESH' and idb.name not in SM.obj_index and u.is_updated_geometry:
                    a = T()
                    me = idb.data
                    key = me.original.session_uid
                    mid = SM.mesh_ids.get(key)
                    if mid is None:
                        mid = len(SM.mesh_ids); SM.mesh_ids[key] = mid
                        t = {k: 0.0 for k in SM.STEPS}
                        nv, nt, nl = SM.extract_mesh(me, t)
                        sbn.scene_set_mesh(self.h, mid, sbn.ptr(SM.pos_s), nv, sbn.ptr(SM.tri_s), nt, sbn.ptr(SM.nrm_s), nl,
                                           sbn.ptr(SM.uv_s), nl, sbn.ptr(SM.tlp_s), sbn.ptr(SM.tmt_s))
                        acts["new_mesh_extract_ms"] = ms(a)
                    m = np.asarray(idb.matrix_world, np.float32).reshape(16)
                    a = T()
                    sbn.scene_add_object(self.h, mid, sbn.ptr(m))
                    SM.obj_index[idb.name] = -1
                    acts["add_object_ms"] = ms(a)
            elif isinstance(idb, bpy.types.Material) and u.is_updated_shading:
                a = T()
                p = principled_params(idb)
                mid = self.mat_ids.setdefault(idb.name, len(self.mat_ids))
                sbn.scene_set_material(self.h, mid, sbn.ptr(p), 16)
                acts["set_material_ms"] = ms(a)
            elif isinstance(idb, bpy.types.Mesh) and u.is_updated_geometry:
                me = idb
                key = me.original.session_uid
                mid = SM.mesh_ids.get(key)
                if mid is not None:
                    nv = len(me.vertices)
                    # (A) naive: full re-extract of everything + native re-copy
                    a = T()
                    t = {k: 0.0 for k in SM.STEPS}
                    nv, nt, nl = SM.extract_mesh(me, t)
                    acts["A_full_reextract_ms"] = ms(a)
                    a = T()
                    sbn.scene_set_mesh(self.h, mid, sbn.ptr(SM.pos_s), nv, sbn.ptr(SM.tri_s), nt, sbn.ptr(SM.nrm_s), nl,
                                       sbn.ptr(SM.uv_s), nl, sbn.ptr(SM.tlp_s), sbn.ptr(SM.tmt_s))
                    acts["A_native_recopy_ms"] = ms(a)
                    # (A2) fast read path (attributes) + native triangulating copy
                    from collections import defaultdict
                    a = T()
                    nv2, nf2, nl2, hu, hm = SM.extract_mesh_fast(me, defaultdict(float))
                    acts["A2_fast_full_reextract_ms"] = ms(a)
                    a = T()
                    SM.set_mesh_fast(self.h, mid, nv2, nf2, nl2, hu, hm)
                    acts["A2_native_recopy_triangulate_ms"] = ms(a)
                    # (B) positions + corner normals only (attribute fast path)
                    a = T()
                    me.attributes["position"].data.foreach_get("vector", SM.pos_s[:nv * 3])
                    me.corner_normals.foreach_get("vector", SM.nrm_s[:nl * 3])
                    acts["B_read_pos_normals_ms"] = ms(a)
                    # (C) positions only, diff against the previous copy, send only the changed vertices
                    a = T()
                    newp = np.empty(nv * 3, np.float32)
                    me.attributes["position"].data.foreach_get("vector", newp)
                    acts["C_read_pos_only_ms"] = ms(a)
                    if self.prev_pos is not None and self.prev_pos.size == newp.size:
                        a = T()
                        ch = np.flatnonzero(newp != self.prev_pos)
                        acts["C_diff_ms"] = ms(a)
                        acts["C_changed_components"] = int(ch.size)
                        a = T()
                        for vi in np.unique(ch // 3):
                            sbn.scene_edit_vertex(self.h, mid, int(vi), float(newp[vi * 3]), float(newp[vi * 3 + 1]), float(newp[vi * 3 + 2]))
                        acts["C_native_edit_vertices_ms"] = ms(a)
        c["resync_actions_ms"] = acts
        c["resync_total_ms"] = ms(r0)
        c["handler_total_ms"] = ms(t_enter)
        self.results.append(c)
        G.save()


kv = G.parse_args()
G.start(Edits(), os.path.join(G.HERE, "out", f"gui_edits_{which}.json"), kv)
