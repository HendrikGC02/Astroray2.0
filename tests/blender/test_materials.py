# SPDX-License-Identifier: Apache-2.0
"""Material and world translation, policy and coverage (0005 V10, 0007 V20; task 083).

Run through Invoke-ArBlender. Graph shape cases inspect the emitter's arrays; render cases send the
real graphs through the generated binding to a CPU session over a lit plane and read the Combined
pass and the report entries back.
"""
import csv
import ctypes
import importlib
import importlib.util
import os
import sys
import tempfile
import types
import unittest

import numpy as np

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(HERE, "headless"))
import support  # noqa: E402

GEN = os.path.join(ROOT, "out", "build", "gen")
DLL = os.path.join(ROOT, "out", "bin", "astroray_core.dll")
ADDON = os.path.join(ROOT, "blender", "astroray")


def _load(name, path):
    if not os.path.exists(path):
        raise RuntimeError(f"{path} is missing: run tools/build.ps1 first")
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# The addon package without its __init__ (which needs the engine DLL): relative imports resolve.
_pkg = types.ModuleType("ar_addon")
_pkg.__path__ = [ADDON]
sys.modules["ar_addon"] = _pkg
# The build stages the generated _ops at the package root; translate reads its ids from there.
OPS = _load("ar_ops", os.path.join(GEN, "_ops.py"))
sys.modules["ar_addon._ops"] = OPS
translate = importlib.import_module("ar_addon.materials.translate")
world_sync = importlib.import_module("ar_addon.sync.world")
from ar_addon.sync import camera, edit as edit_mod, instances, keys, mesh, settings  # noqa: E402

ABI = _load("ar_abi", os.path.join(GEN, "_abi.py"))
PROPS = _load("ar_properties", os.path.join(ADDON, "properties.py"))
LOWERING_TABLE = _load("ar_lowering", os.path.join(ADDON, "materials", "lowering_table.py"))
OP = OPS.OP_ID
AR_REPORT_NODE_UNSUPPORTED = ABI.AR_REPORT_NODE_UNSUPPORTED
W, H = 32, 24


def ok(st, what):
    if st != ABI.AR_OK:
        raise RuntimeError(f"{what} returned status {st}")


def decode(em):
    """The emitter's nodes as dicts, with op names, and its strings."""
    names = {v: k for k, v in OP.items()}
    return [dict(op=names[n[0]], f32=em.f32[n[1]:n[1] + n[2]], i32=em.i32[n[3]:n[3] + n[4]],
                 string=em.strings[n[5]] if n[6] else None) for n in em.nodes]


def inputs_of(em, node):
    return {k[3]: k[0] for k in em.links if k[2] == node}


def unsupported_strings(em):
    return sorted(n["string"] for n in decode(em) if n["op"] == "UNSUPPORTED")


class Session:
    """One CPU session over the current scene; keeps the report and the Combined pass."""

    def __init__(self, scene, plane, material, world, extra=(), size=(W, H), spp=8):
        self.w, self.h, self.spp = size[0], size[1], spp
        self.lib = ctypes.CDLL(DLL)
        ABI.check(self.lib)
        ABI.bind(self.lib)
        self.ctx, self.scene_h, self.session = ctypes.c_void_p(), ctypes.c_void_p(), ctypes.c_void_p()
        self._tmp = tempfile.TemporaryDirectory()
        try:
            self._build(scene, plane, material, world, extra)
        except BaseException:
            self.close()
            raise

    def _build(self, scene, plane, material, world, extra):
        lib, a = self.lib, ABI
        ok(lib.ar_context_create(ctypes.byref(a.ar_context_desc(
            a.AR_DEVICE_CPU, 0, os.path.join(self._tmp.name, "context.log").encode(), None)),
            ctypes.byref(self.ctx)), "ar_context_create")
        ok(lib.ar_scene_create(self.ctx, ctypes.byref(a.ar_scene_desc(None, 2, 0)),
                               ctypes.byref(self.scene_h)), "ar_scene_create")
        dg = support.depsgraph()
        km = keys.KeyMap()
        table = instances.build(dg, km)
        scene.astroray.samples = scene.astroray.viewport_samples = self.spp
        scene.astroray.pixel_filter_type = "BOX"
        fields, _ = settings.build(scene, bpy.data.colorspace, passes=())
        cam_fields, _ = camera.final(dg, scene)
        self.graph = translate.translate_material(material.evaluated_get(dg), km, OP)
        with edit_mod.Edit(a, lib, self.scene_h) as e:
            settings.set_settings(e, a, fields)
            for ob in (plane, *(o for o, _ in extra)):
                gk = int(table.arrays["geom_keys"][table.by_object[ob.original.session_uid][0]])
                mesh.sync_mesh(e, a, ob.data, gk, ob.name)
            translate.set_material(e, a, km, material.original.session_uid, self.graph)
            for _, m in extra:
                translate.set_material(e, a, km, m.original.session_uid,
                                       translate.translate_material(m.evaluated_get(dg), km, OP))
            self.world_graph = world_sync.sync(e, a, world.evaluated_get(dg) if world else None, km, OP)
            instances.set_instances(e, a, table)
            camera.set_camera(e, a, cam_fields)
        sd = a.ar_session_desc(a.AR_SESSION_MODE_FINAL, self.w, self.h, a.AR_DENOISER_NONE)
        ok(lib.ar_session_create(self.scene_h, ctypes.byref(sd), ctypes.byref(self.session)), "ar_session_create")
        view = camera.view_desc(a, cam_fields, self.w, self.h)
        ok(lib.ar_session_set_view(self.session, ctypes.byref(view)), "ar_session_set_view")
        ok(lib.ar_session_start(self.session), "ar_session_start")
        prog = a.ar_progress()
        for _ in range(600):
            ok(lib.ar_session_wait(self.session, 100), "ar_session_wait")
            ok(lib.ar_session_get_progress(self.session, ctypes.byref(prog)), "ar_session_get_progress")
            if prog.state in (a.AR_STATE_CONVERGED, a.AR_STATE_FAILED, a.AR_STATE_CLOSED):
                break
        if prog.state != a.AR_STATE_CONVERGED:
            err = (ctypes.c_uint8 * 512)()
            lib.ar_session_last_error(self.session, 512, err)
            raise RuntimeError(f"session ended in state {prog.state}: "
                               + bytes(err).split(bytes(1))[0].decode("utf-8", "replace"))

    def combined(self):
        """Combined pass as (h, w, components)."""
        W, H = self.w, self.h
        lib, a = self.lib, ABI
        n = ctypes.c_uint32()
        ok(lib.ar_session_pass_count(self.session, ctypes.byref(n)), "ar_session_pass_count")
        for i in range(n.value):
            info = a.ar_pass_info()
            ok(lib.ar_session_pass_info(self.session, i, ctypes.byref(info)), "ar_session_pass_info")
            if bytes(info.name).split(b"\0")[0] == b"Combined":
                c = info.component_count
                buf = np.zeros(W * H * c, np.float32)
                ok(lib.ar_session_read_pass(self.session, i, a.AR_QUANTITY_MEAN, buf.size,
                                            buf.ctypes.data_as(ctypes.POINTER(ctypes.c_float))),
                   "ar_session_read_pass")
                return buf.reshape(H, W, c)
        raise RuntimeError("the session offers no Combined pass")

    def entries(self):
        """[(code, detail)] of every report entry."""
        lib, a = self.lib, ABI
        info = a.ar_report_info()
        ok(lib.ar_session_report_info(self.session, ctypes.byref(info)), "ar_session_report_info")
        out = []
        for i in range(info.entry_count):
            e = a.ar_report_entry()
            ok(lib.ar_session_report_entry(self.session, i, ctypes.byref(e)), "ar_session_report_entry")
            out.append((e.code, bytes(e.detail).split(b"\0")[0].decode("utf-8", "replace")))
        return out

    def close(self):
        for fn, h in ((self.lib.ar_session_destroy, self.session), (self.lib.ar_scene_destroy, self.scene_h),
                      (self.lib.ar_context_destroy, self.ctx)):
            if h.value:
                fn(h)
                h.value = None
        self._tmp.cleanup()


def new_scene(world_color=(1.0, 1.0, 1.0), size=(W, H)):
    """A 16 m plane under a camera that sees only the plane, lit by a constant white world."""
    scene = support.reset_scene()
    scene.render.resolution_x, scene.render.resolution_y, scene.render.resolution_percentage = size[0], size[1], 100
    me = bpy.data.meshes.new("Plane")
    me.from_pydata([(-8, -8, 0), (8, -8, 0), (8, 8, 0), (-8, 8, 0)], [], [(0, 1, 2, 3)])
    plane = bpy.data.objects.new("Plane", me)
    scene.collection.objects.link(plane)
    cam = bpy.data.objects.new("Cam", bpy.data.cameras.new("Cam"))
    scene.collection.objects.link(cam)
    cam.location = (0.0, 0.0, 5.0)
    scene.camera = cam
    world = bpy.data.worlds.new("World")
    world.use_nodes = True
    bg = world.node_tree.nodes["Background"]
    bg.inputs["Color"].default_value = (*world_color, 1.0)
    bg.inputs["Strength"].default_value = 1.0
    scene.world = world
    mat = bpy.data.materials.new("Mat")
    mat.use_nodes = True
    plane.data.materials.append(mat)
    bpy.context.view_layer.update()
    return scene, plane, mat, world


def render(scene, plane, mat, world, extra=(), size=(W, H), spp=8):
    s = Session(scene, plane, mat, world, extra, size, spp)
    try:
        return s.combined(), s.entries(), s.graph
    finally:
        s.close()


def node(tree, idname):
    return next(n for n in tree.nodes if n.bl_idname == idname)


def centre_rgb(img):
    h, w = img.shape[:2]
    return img[h // 4: 3 * h // 4, w // 4: 3 * w // 4, :3].reshape(-1, 3).mean(axis=0)


def is_magenta(rgb):
    return rgb[1] < 0.25 * min(rgb[0], rgb[2])


class TestMaterials(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        PROPS.register()

    @classmethod
    def tearDownClass(cls):
        PROPS.unregister()

    def test_two_node_types_two_entries(self):
        """Two Glass (one material) and a Toon (another): magenta, one NODE_UNSUPPORTED per node type (0007 V20)."""
        scene, plane, mat, world = new_scene()
        t = mat.node_tree
        out = node(t, "ShaderNodeOutputMaterial")
        # The inlined tree holds only nodes the output reaches, and Surface and Volume are the
        # only output sockets that keep a shader link (Blender drops a shader linked to
        # Displacement as invalid). So two Glass nodes reach this material's output, and the
        # Toon is in a second material on a second object (outside the camera's view).
        t.links.new(t.nodes.new("ShaderNodeBsdfGlass").outputs["BSDF"], out.inputs["Surface"])
        t.links.new(t.nodes.new("ShaderNodeBsdfGlass").outputs["BSDF"], out.inputs["Volume"])
        mat2 = bpy.data.materials.new("Mat2")
        mat2.use_nodes = True
        t2 = mat2.node_tree
        t2.links.new(t2.nodes.new("ShaderNodeBsdfToon").outputs["BSDF"],
                     node(t2, "ShaderNodeOutputMaterial").inputs["Surface"])
        me2 = bpy.data.meshes.new("Plane2")
        me2.from_pydata([(100, 100, 0), (101, 100, 0), (101, 101, 0)], [], [(0, 1, 2)])
        me2.materials.append(mat2)
        plane2 = bpy.data.objects.new("Plane2", me2)
        scene.collection.objects.link(plane2)
        bpy.context.view_layer.update()
        img, entries, graph = render(scene, plane, mat, world, extra=((plane2, mat2),))
        rgb = centre_rgb(img)
        print(f"RESULT unsupported material centre rgb {rgb}, entries {entries}")
        self.assertEqual(unsupported_strings(graph), ["ShaderNodeBsdfGlass", "ShaderNodeBsdfGlass"])
        self.assertTrue(is_magenta(rgb), f"expected magenta, got {rgb}")
        got = sorted(d for c, d in entries if c == AR_REPORT_NODE_UNSUPPORTED)
        self.assertEqual(got, ["ShaderNodeBsdfGlass", "ShaderNodeBsdfToon"])

    def test_specular_ior_level_lowered(self):
        """Specular IOR Level 0 lowers to specular_weight 0 and renders without magenta."""
        scene, plane, mat, world = new_scene()
        node(mat.node_tree, "ShaderNodeBsdfPrincipled").inputs["Specular IOR Level"].default_value = 0.0
        img, entries, graph = render(scene, plane, mat, world)
        nodes = decode(graph)
        stores = [i for i, n in enumerate(nodes) if n["op"] == "STORE_SLOT" and n["i32"] == [OPS.RECORD_SLOT["specular_weight"]]]
        self.assertEqual(len(stores), 1)
        src = nodes[inputs_of(graph, stores[0])[0]]
        self.assertEqual((src["op"], src["f32"]), ("CONST_F32", [0.0]))
        self.assertEqual(unsupported_strings(graph), [])
        # A nonzero level: specular_weight = 2L (0005 item 42), so 0.3 gives 0.6.
        node(mat.node_tree, "ShaderNodeBsdfPrincipled").inputs["Specular IOR Level"].default_value = 0.3
        em = translate.translate_material(mat, keys.KeyMap(), OP)
        n3 = decode(em)
        st = next(i for i, n in enumerate(n3) if n["op"] == "STORE_SLOT" and n["i32"] == [OPS.RECORD_SLOT["specular_weight"]])
        self.assertAlmostEqual(n3[inputs_of(em, st)[0]]["f32"][0], 0.6, places=6)
        rgb = centre_rgb(img)
        print(f"RESULT specular level 0 centre rgb {rgb}, entries {entries}")
        self.assertFalse(is_magenta(rgb), f"unexpected magenta {rgb}")
        self.assertEqual([e for e in entries if e[0] == AR_REPORT_NODE_UNSUPPORTED], [])

    def test_material_without_node_setup_lowers_the_default_principled(self):
        """A datablock nobody gave a node setup still carries Blender's default node tree (Blender 5.x keeps
        use_nodes always on), so it lowers as the default Principled BSDF, not as a "no nodes" entry."""
        mat = bpy.data.materials.new("Bare")
        em = translate.translate_material(mat, keys.KeyMap(), OP)
        self.assertEqual(unsupported_strings(em), [])
        stores = [n for n in decode(em) if n["op"] == "STORE_SLOT" and n["i32"] == [translate.SLOT_SPECULAR_WEIGHT]]
        self.assertEqual(len(stores), 1)

    def test_world_noise_raises_one_entry(self):
        scene, plane, mat, world = new_scene()
        t = world.node_tree
        noise = t.nodes.new("ShaderNodeTexNoise")
        t.links.new(noise.outputs["Color"], t.nodes["Background"].inputs["Color"])
        _, entries, graph = render(scene, plane, mat, world)
        self.assertEqual(unsupported_strings(translate.translate_world(world, keys.KeyMap(), OP)),
                         ["ShaderNodeTexNoise"])
        self.assertEqual([e for e in entries if e[0] == AR_REPORT_NODE_UNSUPPORTED],
                         [(AR_REPORT_NODE_UNSUPPORTED, "ShaderNodeTexNoise")])

    def test_constant_world_is_one_emit_with_private_constants(self):
        scene, plane, mat, world = new_scene((0.2, 0.4, 0.6))
        em = translate.translate_world(world, keys.KeyMap(), OP)
        nodes = decode(em)
        self.assertEqual([n["op"] for n in nodes], ["EMIT_LE", "CONST_F32X3", "CONST_F32"])
        self.assertEqual([round(v, 6) for v in nodes[1]["f32"]], [0.2, 0.4, 0.6])
        self.assertEqual(nodes[2]["f32"], [1.0])

    def test_constant_principled_has_no_lift_and_private_emission_constants(self):
        scene, plane, mat, world = new_scene()
        p = node(mat.node_tree, "ShaderNodeBsdfPrincipled")
        p.inputs["Emission Color"].default_value = (1.0, 0.5, 0.25, 1.0)
        p.inputs["Emission Strength"].default_value = 3.0
        nodes = decode(translate.translate_material(mat, keys.KeyMap(), OP))
        ops = [n["op"] for n in nodes]
        self.assertNotIn("RGB_LIFT", ops)  # RGB_LIFT never folds: a constant graph must not hold one
        self.assertEqual(ops.count("EMIT_LE"), 1)
        self.assertEqual(ops.count("STORE_SLOT"), 3)

    def test_image_texture_path(self):
        scene, plane, mat, world = new_scene()
        t = mat.node_tree
        tex = t.nodes.new("ShaderNodeTexImage")
        tex.image = bpy.data.images.new("Img", 4, 4)
        tex.interpolation, tex.extension = "Closest", "EXTEND"
        t.links.new(tex.outputs["Color"], node(t, "ShaderNodeBsdfPrincipled").inputs["Base Color"])
        km = keys.KeyMap()
        em = translate.translate_material(mat, km, OP)
        nodes = decode(em)
        tex_ops = [n for n in nodes if n["op"] == "TEX_IMAGE"]
        self.assertEqual(len(tex_ops), 1)
        self.assertEqual(tex_ops[0]["i32"], [km.key(("image", tex.image.session_uid)), OPS.TEX_MODE["EXTEND"]])
        self.assertEqual([n["op"] for n in nodes if n["op"] in ("RGB_LIFT", "LOAD_CTX_UV")], ["LOAD_CTX_UV", "RGB_LIFT"])
        self.assertEqual(unsupported_strings(em), [])
        tex.interpolation, tex.extension = "Linear", "REPEAT"
        lin = [n for n in decode(translate.translate_material(mat, km, OP)) if n["op"] == "TEX_IMAGE"]
        self.assertEqual(lin[0]["i32"][1], OPS.TEX_MODE["LINEAR"])
        tex.interpolation = "Cubic"
        self.assertEqual(unsupported_strings(translate.translate_material(mat, km, OP)),
                         ["Image Texture.interpolation"])
        tex.interpolation = "Linear"
        tex.image.source = "TILED"
        self.assertEqual(unsupported_strings(translate.translate_material(mat, km, OP)),
                         ["Image Texture.source"])

    def test_other_principled_inputs_are_reported_by_name(self):
        scene, plane, mat, world = new_scene()
        p = node(mat.node_tree, "ShaderNodeBsdfPrincipled")
        p.inputs["Metallic"].default_value = 1.0
        self.assertEqual(unsupported_strings(translate.translate_material(mat, keys.KeyMap(), OP)),
                         ["Principled BSDF.Metallic"])

    def test_defaults_table_matches_blender(self):
        """A fresh Principled node reports nothing: the defaults table equals Blender's."""
        scene, plane, mat, world = new_scene()
        p = node(mat.node_tree, "ShaderNodeBsdfPrincipled")
        self.assertEqual(unsupported_strings(translate.translate_material(mat, keys.KeyMap(), OP)), [])
        names = {s.name for s in p.inputs if s.enabled and s.name != "Weight"}
        known = set(translate._DEFAULTS) | translate._LOWERED_INPUTS
        self.assertEqual(names - known, set(), "an input of this Blender has no table row")

    def test_lowering_table_equals_translator(self):
        self.assertEqual(set(LOWERING_TABLE.LOWERING), set(translate.lowered_types()))

    def test_coverage_has_a_row_for_every_shader_node_type(self):
        with open(os.path.join(ROOT, "tests", "data", "shader_node_types_5_3.txt"), encoding="utf-8") as f:
            nodes = [ln.strip() for ln in f if ln.strip()]
        with open(os.path.join(GEN, "coverage.csv"), encoding="utf-8", newline="") as f:
            rows = list(csv.DictReader(f))
        self.assertEqual([r["node_type"] for r in rows], nodes)
        for r in rows:
            self.assertTrue(r["node_type"] and r["disposition"] and r["note"], f"blank field in {r}")
        by = {r["node_type"]: r["disposition"] for r in rows}
        for n in translate.lowered_types():
            self.assertEqual(by[n], "implemented", n)
        for n, ent in LOWERING_TABLE.PLANNED.items():
            self.assertEqual(by[n], ent["disposition"], n)
            self.assertNotIn(n, translate.lowered_types())
        self.assertEqual(sorted(n for n, d in by.items() if d == "implemented"), sorted(translate.lowered_types()))

    def test_uv_into_colour_and_colour_into_vector_are_reported_by_consumer(self):
        scene, plane, mat, world = new_scene()
        t = mat.node_tree
        p = node(t, "ShaderNodeBsdfPrincipled")
        t.links.new(t.nodes.new("ShaderNodeTexCoord").outputs["UV"], p.inputs["Base Color"])
        self.assertEqual(unsupported_strings(translate.translate_material(mat, keys.KeyMap(), OP)),
                         ["Principled BSDF.Base Color"])
        t.links.clear()
        t.links.new(p.outputs["BSDF"], node(t, "ShaderNodeOutputMaterial").inputs["Surface"])
        tex = t.nodes.new("ShaderNodeTexImage")
        tex.image = bpy.data.images.new("Img2", 4, 4)
        tex2 = t.nodes.new("ShaderNodeTexImage")
        tex2.image = tex.image
        t.links.new(tex2.outputs["Color"], tex.inputs["Vector"])
        t.links.new(tex.outputs["Color"], p.inputs["Base Color"])
        self.assertEqual(unsupported_strings(translate.translate_material(mat, keys.KeyMap(), OP)),
                         ["Image Texture.Vector"])

    def test_instancer_uv_and_emission_sampling(self):
        scene, plane, mat, world = new_scene()
        t = mat.node_tree
        uv = t.nodes.new("ShaderNodeTexCoord")
        uv.from_instancer = True
        tex = t.nodes.new("ShaderNodeTexImage")
        tex.image = bpy.data.images.new("Img3", 4, 4)
        t.links.new(uv.outputs["UV"], tex.inputs["Vector"])
        t.links.new(tex.outputs["Color"], node(t, "ShaderNodeBsdfPrincipled").inputs["Base Color"])
        self.assertEqual(unsupported_strings(translate.translate_material(mat, keys.KeyMap(), OP)),
                         ["ShaderNodeTexCoord.from_instancer"])
        mat.cycles.emission_sampling = "FRONT"
        self.assertEqual(translate.translate_material(mat, keys.KeyMap(), OP).emission_sampling, "FRONT")
        for name in translate.EMISSION_SAMPLING_NAMES:
            self.assertTrue(hasattr(ABI, "AR_EMISSION_SAMPLING_" + name), name)

    def test_translation_cache_retranslates_only_what_changed(self):
        scene, plane, mat, world = new_scene()
        other = bpy.data.materials.new("Other")
        other.use_nodes = True
        group = bpy.data.node_groups.new("G", "ShaderNodeTree")
        gn = mat.node_tree.nodes.new("ShaderNodeGroup")
        gn.node_tree = group
        # Both materials must be in the depsgraph to have evaluated copies; the cache is fed those
        # copies (as the engine does) and must still key by the original's session_uid.
        scene.collection.objects.link(bpy.data.objects.new("P2", bpy.data.meshes.new("M2")))
        scene.objects["P2"].data.materials.append(other)
        bpy.context.view_layer.update()
        dg = support.depsgraph()
        ev_mat, ev_other = mat.evaluated_get(dg), other.evaluated_get(dg)
        self.assertIsNot(ev_mat, mat)
        cache, km = translate.TranslationCache(), keys.KeyMap()
        Act = lambda id_type, uid: types.SimpleNamespace(id_type=id_type, uid=uid)  # noqa: E731
        cache.get(ev_mat, km, OP)
        cache.get(ev_other, km, OP)
        cache.get(ev_mat, km, OP)
        self.assertEqual(cache.translations, 2)
        cache.invalidate([Act("MATERIAL", other.session_uid)])
        cache.get(ev_mat, km, OP)
        cache.get(ev_other, km, OP)
        self.assertEqual(cache.translations, 3)
        cache.invalidate([Act("NODETREE", group.session_uid)])  # a group in mat's raw tree
        cache.get(ev_mat, km, OP)
        cache.get(ev_other, km, OP)
        self.assertEqual(cache.translations, 4)


if __name__ == "__main__":
    support.main()
