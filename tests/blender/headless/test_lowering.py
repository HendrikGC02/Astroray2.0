# SPDX-License-Identifier: Apache-2.0
"""Addon lowerings I (0005 items 36, 40, 42; 0001 V15, 0007 V10): the Emission and Blackbody nodes,
Principled Alpha and the implicit colour-to-float link reach the engine, and every node without a
lowering renders magenta with exactly one report entry that names its type.

Needs the built engine (tools/build.ps1): graph cases read the emitter's arrays, render cases send the
real graphs to a CPU session through the generated binding, as tests/blender/test_materials.py does
(this file reuses its session and scene helpers).
"""
import csv
import os
import struct
import sys
import time
import types
import unittest
import zlib

import numpy as np

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import support  # noqa: E402

sys.path.insert(0, os.path.dirname(HERE))
import test_materials as tm  # noqa: E402  (the module, not its TestCase)

CSV = os.path.join(tm.GEN, "coverage.csv")
EVIDENCE = os.path.join(support.ROOT, "out", "evidence", "165")
UNSUPPORTED = tm.AR_REPORT_NODE_UNSUPPORTED
SIZE_V10, SPP_V10 = (4, 4), 1
# A node type of coverage.csv that cannot be created (the abstract base class of every shader node).
NOT_A_NODE = frozenset(("ShaderNode",))
# What Blender does to the lone node of each unsupported row (read from the measured run, pinned so a
# wiring bug or a Blender change cannot move rows into "removed" unnoticed): inline_shader_nodes drops
# a group with no tree, a node nothing reaches (no outputs, or an internal light-tree node) and a constant
# Value; these types cannot be created at all. ShaderNodeShadowRaycast is an EEVEE node: its removal
# must be re-checked if Cycles ever reads it.
EXPECTED_REMOVED = frozenset(("ShaderNodeGroup", "ShaderNodeLightEvaluation", "ShaderNodeLightInfo",
                              "ShaderNodeLightIterInternalOutput", "ShaderNodeOutputLineStyle",
                              "ShaderNodeScript", "ShaderNodeShadowRaycast", "ShaderNodeValue"))
EXPECTED_UNCREATABLE = frozenset(("ShaderNodeCustomGroup", "ShaderNodeTree"))
MIN_MAGENTA = 80  # measured 86 of 97 rows


def node_ops(em):
    return [n["op"] for n in tm.decode(em)]


def stores(em):
    """{record slot id: the node that feeds its STORE_SLOT}."""
    nodes = tm.decode(em)
    return {n["i32"][0]: nodes[tm.inputs_of(em, i)[0]] for i, n in enumerate(nodes) if n["op"] == "STORE_SLOT"}


def principled(mat):
    return tm.node(mat.node_tree, "ShaderNodeBsdfPrincipled")


def only_shader(mat, idname):
    """Replaces the material's shader by one node of `idname` (a fresh node with default sockets)."""
    t = mat.node_tree
    t.nodes.remove(principled(mat))
    n = t.nodes.new(idname)
    t.links.new(n.outputs[0], tm.node(t, "ShaderNodeOutputMaterial").inputs["Surface"])
    return n


def srgb_png(path, rgb):
    """Writes scene-linear rgb (h, w, 3) as an 8 bit sRGB PNG (the evidence image; no imaging library)."""
    a = np.clip(rgb, 0.0, 1.0)
    enc = np.where(a <= 0.0031308, 12.92 * a, 1.055 * np.power(a, 1 / 2.4) - 0.055)
    px = (enc * 255.0 + 0.5).astype(np.uint8)
    raw = b"".join(b"\0" + px[y].tobytes() for y in range(px.shape[0]))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", px.shape[1], px.shape[0], 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


class TestLowering(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tm.PROPS.register()

    @classmethod
    def tearDownClass(cls):
        tm.PROPS.unregister()

    def test_use_nodes_is_always_on(self):
        """Blender 5.x keeps Material.use_nodes True (the addon relies on it: a material is always its node
        tree); if a Blender update lets it be False, this fails and diffuse_color needs a lowering."""
        m = bpy.data.materials.new("Bare")
        self.assertTrue(m.use_nodes)
        self.assertIsNotNone(m.node_tree)
        try:
            m.use_nodes = False
        except Exception:  # noqa: BLE001 - a read-only property is as good as an ignored write
            pass
        self.assertTrue(m.use_nodes, "use_nodes can be False in this Blender: lower diffuse_color as base_color")
        self.assertIsNotNone(tm.translate.translate_material(m, tm.keys.KeyMap(), tm.OP).nodes)

    def test_emission_lowers(self):
        scene, plane, mat, world = tm.new_scene()  # a white world: a lit lobe would add to the emission
        e = only_shader(mat, "ShaderNodeEmission")
        e.inputs["Color"].default_value = (1.0, 0.5, 0.2, 1.0)
        e.inputs["Strength"].default_value = 2.0
        em = tm.translate.translate_material(mat, tm.keys.KeyMap(), tm.OP)
        ops = node_ops(em)
        self.assertEqual(tm.unsupported_strings(em), [])
        self.assertEqual(ops.count("EMIT_LE"), 1)
        st = stores(em)
        slot = tm.OPS.RECORD_SLOT
        self.assertEqual(sorted(st), sorted([slot["base_weight"], slot["specular_weight"]]))
        for k in (slot["base_weight"], slot["specular_weight"]):
            self.assertEqual((st[k]["op"], st[k]["f32"]), ("CONST_F32", [0.0]))
        nodes = tm.decode(em)
        emit = ops.index("EMIT_LE")
        src = tm.inputs_of(em, emit)
        self.assertEqual([round(v, 6) for v in nodes[src[0]]["f32"]], [1.0, 0.5, 0.2])
        self.assertEqual(nodes[src[1]]["f32"], [2.0])
        # Rendered under a white world: the plane's mean equals strength x colour only because the base and
        # specular weights are 0 (a lit lobe would reflect the world on top).
        img, entries, _ = tm.render(scene, plane, mat, world, size=(32, 24), spp=64)
        rgb = tm.centre_rgb(img)
        print(f"RESULT emission mean {rgb}, entries {entries}")
        np.testing.assert_allclose(rgb, [2.0, 1.0, 0.4], rtol=1e-3, atol=1e-3)
        self.assertEqual([x for x in entries if x[0] == UNSUPPORTED], [])

    def test_principled_emission_keeps_its_path(self):
        scene, plane, mat, world = tm.new_scene()
        p = principled(mat)
        p.inputs["Emission Color"].default_value = (1.0, 0.5, 0.25, 1.0)
        p.inputs["Emission Strength"].default_value = 3.0
        em = tm.translate.translate_material(mat, tm.keys.KeyMap(), tm.OP)
        self.assertEqual(node_ops(em).count("EMIT_LE"), 1)
        self.assertNotIn(tm.OPS.RECORD_SLOT["base_weight"], stores(em))

    def test_blender_folds_a_constant_blackbody(self):
        """inline_shader_nodes bakes a Blackbody whose Temperature is constant (or a Value node) into the
        consumer's RGB default, so the addon sees an ordinary colour and the BLACKBODY op is never reached
        through it. This pins that premise: when a Blender update stops folding, this fails and the
        Blackbody lowering below becomes live."""
        scene, plane, mat, world = tm.new_scene()
        t = mat.node_tree
        e = only_shader(mat, "ShaderNodeEmission")
        bb = t.nodes.new("ShaderNodeBlackbody")
        bb.inputs["Temperature"].default_value = 2700.0
        t.links.new(bb.outputs["Color"], e.inputs["Color"])
        inlined = mat.inline_shader_nodes()
        self.assertNotIn("ShaderNodeBlackbody", [n.bl_idname for n in inlined.node_tree.nodes])
        em = tm.translate.translate_material(mat, tm.keys.KeyMap(), tm.OP)
        self.assertNotIn("BLACKBODY", node_ops(em))
        self.assertEqual(tm.unsupported_strings(em), [])
        nodes = tm.decode(em)
        rgb = nodes[tm.inputs_of(em, node_ops(em).index("EMIT_LE"))[0]]["f32"]
        self.assertGreater(rgb[0] / rgb[2], 5.0, f"2700 K as Blender's RGB {rgb}")

    def test_blackbody_colour_orders_by_temperature(self):
        scene, plane, mat, world = tm.new_scene((0.0, 0.0, 0.0), size=(96, 32))
        # Three planes in one frame: Blackbody 2700 K (centre, `mat`), a flat Emission (1, 0.5, 0.2) on
        # the left and Blackbody 12000 K on the right.
        mats = []
        for temp in (None, 12000.0, 2700.0):
            m2 = mat if temp == 2700.0 else bpy.data.materials.new("M")
            t2 = m2.node_tree
            e2 = only_shader(m2, "ShaderNodeEmission")
            if temp is None:
                e2.inputs["Color"].default_value = (1.0, 0.5, 0.2, 1.0)
            else:
                b2 = t2.nodes.new("ShaderNodeBlackbody")
                b2.inputs["Temperature"].default_value = temp
                t2.links.new(b2.outputs["Color"], e2.inputs["Color"])
            mats.append(m2)
        scene.camera.location = (0.0, 0.0, 12.0)
        plane.scale = (0.125, 0.125, 1.0)  # the 16 m plane becomes 2 m
        extras = []
        for x, m2, nm in ((-2.5, mats[0], "L"), (2.5, mats[1], "R")):
            me = bpy.data.meshes.new("P" + nm)
            me.from_pydata([(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)], [], [(0, 1, 2, 3)])
            me.materials.append(m2)
            ob = bpy.data.objects.new("P" + nm, me)
            ob.location = (x, 0.0, 0.0)
            scene.collection.objects.link(ob)
            extras.append((ob, m2))
        bpy.context.view_layer.update()
        img, entries, _ = tm.render(scene, plane, mat, world, extra=tuple(extras), size=(96, 32), spp=64)
        self.assertEqual([x for x in entries if x[0] == UNSUPPORTED], [])
        w = img.shape[1]

        def mean_at(cx):
            return img[12:20, int(cx) - 5:int(cx) + 5, :3].reshape(-1, 3).mean(axis=0)

        left, mid, right = mean_at(w * (0.5 - 2.5 / 8.6)), mean_at(w * 0.5), mean_at(w * (0.5 + 2.5 / 8.6))
        print(f"RESULT emission {left} blackbody 2700 K {mid} blackbody 12000 K {right}")
        np.testing.assert_allclose(left, [1.0, 0.5, 0.2], rtol=2e-3, atol=2e-3)
        self.assertGreater(mid[0] / mid[2], 1.5, "2700 K is orange")
        self.assertLess(right[0] / right[2], 0.9, "12000 K is blue")
        self.assertGreater(mid[0] / mid[2], right[0] / right[2] * 2.0)
        srgb_png(os.path.join(EVIDENCE, "emission_blackbody.png"), img[:, :, :3])

    @staticmethod
    def _stub_blackbody_into(label, temp):
        """A colour socket linked from a Blackbody node, without Blender: the real node never reaches the
        translator while its Temperature is constant (test_blender_folds_a_constant_blackbody)."""
        ns = types.SimpleNamespace
        temp_sock = ns(name="Temperature", links=[], default_value=temp)
        node = ns(bl_idname="ShaderNodeBlackbody", inputs={"Temperature": temp_sock})
        link = ns(is_muted=False, from_node=node, from_socket=ns(name="Color", type="RGBA"))
        return ns(name=label, links=[link], default_value=(0.0, 0.0, 0.0, 1.0)), temp_sock

    def test_blackbody_only_into_emission(self):
        """A Blackbody is a `spec` value only into an emission colour (0005 item 36); anywhere else it is
        reported with its consumer named, once, and never converted to a colour."""
        km = tm.keys.KeyMap()
        # The lowering itself, on a stand-in node: constant Temperature, scale 1.
        consumer, _ = self._stub_blackbody_into("Color", 2700.0)
        em = tm.translate.GraphEmitter(tm.OP)
        n = tm.translate._colour(em, consumer, km, "Emission.Color", lift=False, emission=True)
        self.assertIsNotNone(n)
        self.assertEqual([x["f32"] for x in tm.decode(em) if x["op"] == "BLACKBODY"], [[2700.0, 1.0]])
        self.assertEqual(tm.unsupported_strings(em), [])
        for lift in (True, False):  # Base Color (lifted) and any non-emission colour
            consumer, _ = self._stub_blackbody_into("Base Color", 2700.0)
            em = tm.translate.GraphEmitter(tm.OP)
            self.assertIsNone(tm.translate._colour(em, consumer, km, "Principled BSDF.Base Color", lift=lift))
            self.assertEqual(tm.unsupported_strings(em), ["Principled BSDF.Base Color"])
            self.assertNotIn("BLACKBODY", node_ops(em))
        # A Temperature driven by another lowered node is not lowered (a linked Temperature is out of scope).
        consumer, temp = self._stub_blackbody_into("Color", 2700.0)
        temp.links = [types.SimpleNamespace(is_muted=False, from_socket=types.SimpleNamespace(name="Color", type="RGBA"),
                                            from_node=types.SimpleNamespace(bl_idname="ShaderNodeTexImage"))]
        em = tm.translate.GraphEmitter(tm.OP)
        self.assertIsNone(tm.translate._colour(em, consumer, km, "Emission.Color", lift=False, emission=True))
        self.assertEqual(tm.unsupported_strings(em), ["Blackbody.Temperature"])
        # In Blender: a Temperature that is not constant (an image's colour) keeps the node in the tree.
        scene, plane, mat, world = tm.new_scene()
        t = mat.node_tree
        p = principled(mat)
        bb = t.nodes.new("ShaderNodeBlackbody")
        tex = t.nodes.new("ShaderNodeTexImage")
        tex.image = bpy.data.images.new("Temp", 4, 4)
        t.links.new(tex.outputs["Color"], bb.inputs["Temperature"])
        t.links.new(bb.outputs["Color"], p.inputs["Base Color"])
        em = tm.translate.translate_material(mat, km, tm.OP)
        self.assertEqual(tm.unsupported_strings(em), ["Principled BSDF.Base Color"])
        img, entries, _ = tm.render(scene, plane, mat, world, size=(8, 8), spp=4)
        self.assertTrue(tm.is_magenta(tm.centre_rgb(img)))
        self.assertEqual([d for c, d in entries if c == UNSUPPORTED], ["Principled BSDF.Base Color"])
        t.links.clear()
        t.links.new(p.outputs["BSDF"], tm.node(t, "ShaderNodeOutputMaterial").inputs["Surface"])
        p.inputs["Emission Strength"].default_value = 1.0
        t.links.new(bb.outputs["Color"], p.inputs["Emission Color"])
        t.links.new(tex.outputs["Color"], bb.inputs["Temperature"])
        self.assertEqual(tm.unsupported_strings(tm.translate.translate_material(mat, km, tm.OP)),
                         ["Blackbody.Temperature"])
        # Into a float socket (the Emission node's Strength): the consumer is named.
        t.links.clear()
        e = only_shader(mat, "ShaderNodeEmission")
        t.links.new(tex.outputs["Color"], bb.inputs["Temperature"])
        t.links.new(bb.outputs["Color"], e.inputs["Strength"])
        self.assertEqual(tm.unsupported_strings(tm.translate.translate_material(mat, km, tm.OP)),
                         ["Emission.Strength"])

    def test_alpha_lowers_and_reaches_the_record(self):
        scene, plane, mat, world = tm.new_scene()
        p = principled(mat)
        slot = tm.OPS.RECORD_SLOT["geometry_opacity"]
        self.assertNotIn(slot, stores(tm.translate.translate_material(mat, tm.keys.KeyMap(), tm.OP)),
                         "Alpha 1 stores nothing")
        means = {}
        for a in (0.0, 0.5, 1.0):
            p.inputs["Alpha"].default_value = a
            em = tm.translate.translate_material(mat, tm.keys.KeyMap(), tm.OP)
            self.assertEqual(tm.unsupported_strings(em), [])
            if a < 1.0:
                src = stores(em)[slot]
                self.assertEqual((src["op"], src["f32"]), ("CONST_F32", [a]))
            img, entries, _ = tm.render(scene, plane, mat, world, size=(16, 12), spp=128)
            self.assertEqual([x for x in entries if x[0] == UNSUPPORTED], [])
            means[a] = tm.centre_rgb(img)
        print(f"RESULT alpha means {means}")
        # The stochastic pass-through (156) mixes the surface and the world behind it by Alpha.
        np.testing.assert_allclose(means[0.5], 0.5 * (means[0.0] + means[1.0]), rtol=0.04, atol=0.01)
        self.assertGreater(abs(means[0.0][0] - means[1.0][0]), 0.05)

    def test_colour_into_float_is_luminance(self):
        scene, plane, mat, world = tm.new_scene()
        t = mat.node_tree
        p = principled(mat)
        tex = t.nodes.new("ShaderNodeTexImage")
        tex.image = bpy.data.images.new("Grey", 4, 4)
        t.links.new(tex.outputs["Color"], p.inputs["Roughness"])
        em = tm.translate.translate_material(mat, tm.keys.KeyMap(), tm.OP)
        self.assertEqual(tm.unsupported_strings(em), [])
        nodes = tm.decode(em)
        st = next(i for i, n in enumerate(nodes) if n["op"] == "STORE_SLOT" and n["i32"] == [tm.translate.SLOT_SPECULAR_ROUGHNESS])
        conv = tm.inputs_of(em, st)[0]
        self.assertEqual(nodes[conv]["op"], "COLOR_TO_F32")
        self.assertEqual(nodes[tm.inputs_of(em, conv)[0]]["op"], "TEX_IMAGE")
        # Specular IOR Level: the luminance times 2 into specular_weight.
        t.links.clear()
        t.links.new(p.outputs["BSDF"], tm.node(t, "ShaderNodeOutputMaterial").inputs["Surface"])
        t.links.new(tex.outputs["Color"], p.inputs["Specular IOR Level"])
        em = tm.translate.translate_material(mat, tm.keys.KeyMap(), tm.OP)
        self.assertEqual(tm.unsupported_strings(em), [])
        nodes = tm.decode(em)
        st = next(i for i, n in enumerate(nodes) if n["op"] == "STORE_SLOT" and n["i32"] == [tm.translate.SLOT_SPECULAR_WEIGHT])
        mul = tm.inputs_of(em, st)[0]
        self.assertEqual(nodes[mul]["op"], "MUL_F32")
        feeds = tm.inputs_of(em, mul)
        self.assertEqual(nodes[feeds[0]]["op"], "COLOR_TO_F32")
        self.assertEqual((nodes[feeds[1]]["op"], nodes[feeds[1]]["f32"]), ("CONST_F32", [2.0]))
        self.assertEqual(nodes[tm.inputs_of(em, feeds[0])[0]]["op"], "TEX_IMAGE")
        # The same link into Alpha and Emission Strength.
        t.links.clear()
        t.links.new(p.outputs["BSDF"], tm.node(t, "ShaderNodeOutputMaterial").inputs["Surface"])
        t.links.new(tex.outputs["Color"], p.inputs["Alpha"])
        em = tm.translate.translate_material(mat, tm.keys.KeyMap(), tm.OP)
        self.assertEqual(stores(em)[tm.OPS.RECORD_SLOT["geometry_opacity"]]["op"], "COLOR_TO_F32")
        t.links.clear()
        t.links.new(p.outputs["BSDF"], tm.node(t, "ShaderNodeOutputMaterial").inputs["Surface"])
        t.links.new(tex.outputs["Color"], p.inputs["Emission Strength"])
        em = tm.translate.translate_material(mat, tm.keys.KeyMap(), tm.OP)
        self.assertIn("COLOR_TO_F32", node_ops(em))
        self.assertEqual(node_ops(em).count("EMIT_LE"), 1)
        # Image Alpha is a later task; a vector (Blender averages it) is not lowered: each is one entry, named by consumer.
        t.links.clear()
        t.links.new(p.outputs["BSDF"], tm.node(t, "ShaderNodeOutputMaterial").inputs["Surface"])
        t.links.new(tex.outputs["Alpha"], p.inputs["Alpha"])
        self.assertEqual(tm.unsupported_strings(tm.translate.translate_material(mat, tm.keys.KeyMap(), tm.OP)),
                         ["Principled BSDF.Alpha"])
        t.links.clear()
        t.links.new(p.outputs["BSDF"], tm.node(t, "ShaderNodeOutputMaterial").inputs["Surface"])
        t.links.new(t.nodes.new("ShaderNodeTexCoord").outputs["UV"], p.inputs["Roughness"])
        self.assertEqual(tm.unsupported_strings(tm.translate.translate_material(mat, tm.keys.KeyMap(), tm.OP)),
                         ["Principled BSDF.Roughness"])

    def _v10_rows(self):
        if not os.path.exists(CSV):
            raise RuntimeError(f"{CSV} is missing: run tools/build.ps1 first")
        with open(CSV, encoding="utf-8", newline="") as f:
            return [r["node_type"] for r in csv.DictReader(f) if r["disposition"] == "unsupported"]

    def _holding(self, mat, idname):
        """Puts one node of `idname` in the material, fed by an image so that Blender cannot fold it into
        a constant (inline_shader_nodes bakes a node whose inputs are all constants into its consumer):
        a shader goes to Surface, any other output to the Principled's Base Color. Returns the node, or
        None when this type cannot be created."""
        t = mat.node_tree
        out = tm.node(t, "ShaderNodeOutputMaterial")
        p = principled(mat)
        try:
            n = t.nodes.new(idname)
        except RuntimeError:
            return None
        tex = t.nodes.new("ShaderNodeTexImage")
        tex.image = bpy.data.images.new("Feed", 4, 4)
        for s in n.inputs:
            if s.enabled and s.type != "SHADER" and not s.hide_value:
                t.links.new(tex.outputs["Color"], s)
                break
        if n.outputs:
            o = n.outputs[0]
            if o.type == "SHADER":
                t.nodes.remove(p)
                t.links.new(o, out.inputs["Surface"])
            else:
                t.links.new(o, p.inputs["Base Color"])
        return n

    def test_unsupported_nodes_render_magenta_one_entry(self):
        """0007 V10: every coverage row with disposition unsupported, alone in a material, renders magenta
        with exactly one report entry that names its node type (4 x 4 px, 1 spp). A node Blender removes
        before the addon sees the tree (a constant folded into its consumer, a node nothing reaches) has
        nothing to report: it must raise no entry, and is counted separately."""
        rows = [r for r in self._v10_rows() if r not in NOT_A_NODE]
        self.assertGreater(len(rows), 20)
        t0 = time.perf_counter()
        magenta, removed, uncreatable = [], [], []
        for idname in rows:
            with self.subTest(node=idname):
                scene, plane, mat, world = tm.new_scene(size=SIZE_V10)
                n = self._holding(mat, idname)
                if n is None:
                    uncreatable.append(idname)
                    continue
                bpy.context.view_layer.update()
                inlined = mat.inline_shader_nodes()  # owns the tree: keep it alive while reading
                present = idname in [x.bl_idname for x in inlined.node_tree.nodes]
                img, entries, graph = tm.render(scene, plane, mat, world, size=SIZE_V10, spp=SPP_V10)
                got = [(c, d) for c, d in entries if c == UNSUPPORTED]
                rgb = img[:, :, :3].reshape(-1, 3).mean(axis=0)
                if present:
                    self.assertEqual(got, [(UNSUPPORTED, idname)], f"{idname}: entries {entries}")
                    self.assertTrue(tm.is_magenta(rgb), f"{idname}: {rgb}")
                    magenta.append(idname)
                else:
                    self.assertEqual(got, [], f"{idname} is gone after inlining but is reported: {entries}")
                    removed.append(idname)
        secs = time.perf_counter() - t0
        # The split must be exactly the one measured.
        self.assertEqual(set(removed), EXPECTED_REMOVED)
        self.assertEqual(set(uncreatable), EXPECTED_UNCREATABLE)
        self.assertGreaterEqual(len(magenta), MIN_MAGENTA)
        print(f"RESULT V10 {len(magenta)} magenta with one entry, {len(removed)} removed by Blender before "
              f"the addon ({', '.join(removed)}), {len(uncreatable)} not creatable ({', '.join(uncreatable)}), "
              f"of {len(rows)} rows, {secs:.2f} s")


if __name__ == "__main__":
    support.main()
