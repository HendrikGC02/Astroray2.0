# SPDX-License-Identifier: Apache-2.0
"""Addon lowerings I and II (0005 items 36, 38, 40, 42; 0001 V15, 0007 V10): the Emission and Blackbody nodes,
Principled Alpha, Metallic, the Mix Shader and the Transparent BSDF and the implicit colour-to-float link
reach the engine, and every node without a lowering renders magenta with exactly one report entry that
names its type.

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
EVIDENCE_166 = os.path.join(support.ROOT, "out", "evidence", "166")
APPROXIMATED = tm.ABI.AR_REPORT_NODE_APPROXIMATED
SLOT = tm.OPS.RECORD_SLOT
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


def canon(em):
    """The emitter as the compiler reads it: nodes with float32 parameters, and the links sorted."""
    nodes = [(n["op"], [float(np.float32(v)) for v in n["f32"]], n["i32"], n["string"]) for n in tm.decode(em)]
    return nodes, sorted(em.links)


# A graph written by hand with the encoding of 0007 appendix `ar_graph_desc` (not through GraphEmitter.store or
# child_params), in the order the lowering emits it: a child's nodes, then the mix's factor and BRANCH_MIX.
def h_store(em, slot, child, src):
    n = em.add("STORE_SLOT", i32=(SLOT[slot], child))
    em.link(src, n, 0)


def h_principled(em, child, rgb, rough, weight=1.0):
    h_store(em, "base_color", child, em.const_rgb(rgb))
    h_store(em, "specular_roughness", child, em.const_f32(rough))
    h_store(em, "specular_weight", child, em.const_f32(weight))


def h_emission(em, child, rgb, strength):
    s, c = em.const_f32(strength), em.const_rgb(rgb)
    e = em.add("EMIT_LE", i32=(child,))
    em.link(c, e, 0)
    em.link(s, e, 1)


def h_mix(em, tag, a, b, factor):
    f = em.const_f32(factor)
    n = em.add("BRANCH_MIX", i32=(tag, a, b))
    em.link(f, n, 0)


def new_principled(mat, rgb, rough=0.5, level=0.5):
    p = mat.node_tree.nodes.new("ShaderNodeBsdfPrincipled")
    p.inputs["Base Color"].default_value = (*rgb, 1.0)
    p.inputs["Roughness"].default_value = rough
    p.inputs["Specular IOR Level"].default_value = level
    return p


def new_emission(mat, rgb, strength):
    e = mat.node_tree.nodes.new("ShaderNodeEmission")
    e.inputs["Color"].default_value = (*rgb, 1.0)
    e.inputs["Strength"].default_value = strength
    return e


def new_transparent(mat, rgb=(1.0, 1.0, 1.0)):
    n = mat.node_tree.nodes.new("ShaderNodeBsdfTransparent")
    n.inputs["Color"].default_value = (*rgb, 1.0)
    return n


def new_mix(mat, a, b, fac):
    """A Mix Shader of two shader nodes (or mixes) as the material's Surface."""
    t = mat.node_tree
    m = t.nodes.new("ShaderNodeMixShader")
    m.inputs["Fac"].default_value = fac
    t.links.new(a.outputs[0], m.inputs[1])
    t.links.new(b.outputs[0], m.inputs[2])
    t.links.new(m.outputs[0], tm.node(t, "ShaderNodeOutputMaterial").inputs["Surface"])
    return m


def empty_material(mat):
    """The material without its default Principled, so a test builds the whole shader tree itself."""
    mat.node_tree.nodes.remove(principled(mat))
    return mat


def transparent_entries(entries):
    """The compiler's report of a coloured Transparent folded by its mean (not the shadow-ray note every
    non-opaque material gets)."""
    return [x for x in entries if x[0] == APPROXIMATED and x[1].startswith("Transparent")]


def lower(mat):
    return tm.translate.translate_material(mat, tm.keys.KeyMap(), tm.OP)


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

    def test_mix_matches_hand_graph(self):
        """A Mix Shader compiles to the words of the same graph written by hand with 0007's encoding: the
        children's stores and emissions tagged by child, one BRANCH_MIX {tag, a, b} fed by the factor."""
        red, blue, ems = (0.8, 0.1, 0.1), (0.1, 0.1, 0.8), (1.0, 0.5, 0.2)
        cases = []  # (name, lowered emitter, hand-written emitter); a new scene deletes the earlier materials

        scene, plane, mat, world = tm.new_scene()
        empty_material(mat)
        new_mix(mat, new_principled(mat, red, 0.5), new_principled(mat, blue, 0.2), 0.3)
        hand = tm.translate.GraphEmitter(tm.OP)
        h_principled(hand, 0, red, 0.5)
        h_principled(hand, 1, blue, 0.2)
        h_mix(hand, 0, 0, 1, 0.3)
        cases.append(("Mix(Principled, Principled, 0.3)", lower(mat), hand))

        scene, plane, mat, world = tm.new_scene()
        empty_material(mat)
        new_mix(mat, new_transparent(mat), new_principled(mat, red, 0.5), 0.4)
        hand = tm.translate.GraphEmitter(tm.OP)
        h_store(hand, "geometry_opacity", 0, hand.const_f32(0.0))
        h_principled(hand, 1, red, 0.5)
        h_mix(hand, 0, 0, 1, 0.4)
        cases.append(("Mix(Transparent, Principled, 0.4)", lower(mat), hand))

        scene, plane, mat, world = tm.new_scene()
        empty_material(mat)
        new_mix(mat, new_emission(mat, ems, 2.0), new_principled(mat, red, 0.5), 0.5)
        hand = tm.translate.GraphEmitter(tm.OP)
        h_emission(hand, 0, ems, 2.0)
        h_principled(hand, 1, red, 0.5)
        h_mix(hand, 0, 0, 1, 0.5)
        cases.append(("Mix(Emission, Principled, 0.5)", lower(mat), hand))

        for name, em, hand in cases:
            with self.subTest(graph=name):
                self.assertEqual(tm.unsupported_strings(em), [])
                self.assertEqual(canon(em), canon(hand))
        # Without a Mix the words are the ones a graph always had: no `child` parameter anywhere.
        scene, plane, mat, world = tm.new_scene()
        self.assertTrue(all(len(n["i32"]) == 1 for n in tm.decode(lower(mat)) if n["op"] == "STORE_SLOT"))

    def test_mix_input_without_a_shader_is_reported(self):
        scene, plane, mat, world = tm.new_scene()
        empty_material(mat)
        m = new_mix(mat, new_principled(mat, (1.0, 1.0, 1.0)), new_principled(mat, (0.0, 0.0, 0.0)), 0.5)
        mat.node_tree.links.remove(m.inputs[2].links[0])
        self.assertEqual(tm.unsupported_strings(lower(mat)), ["Mix Shader.Shader"])

    def test_mix_non_shader_input_is_reported(self):
        """A lowered node that is not a shader in a Mix socket is reported, not dropped with the other side
        compiled at full weight."""
        scene, plane, mat, world = tm.new_scene()
        empty_material(mat)
        bg = mat.node_tree.nodes.new("ShaderNodeBackground")
        new_mix(mat, bg, new_principled(mat, (1.0, 1.0, 1.0)), 0.5)
        self.assertEqual(tm.unsupported_strings(lower(mat)), ["Mix Shader.Shader"])

    def test_mix_emission_of_strength_zero_is_black(self):
        """Mix(Emission strength 0, Principled, 0.5): the emission child stays an emission child (an EMIT_LE of
        strength 0, no store), so it renders black and the Principled keeps half its weight, not a dielectric."""
        scene, plane, mat, world = tm.new_scene()
        empty_material(mat)
        red = (0.8, 0.1, 0.1)
        new_mix(mat, new_emission(mat, (1.0, 1.0, 1.0), 0.0), new_principled(mat, red), 0.5)
        em = lower(mat)
        nodes = tm.decode(em)
        self.assertEqual(tm.unsupported_strings(em), [])
        self.assertEqual([n["i32"] for n in nodes if n["op"] == "EMIT_LE"], [[0]])
        self.assertEqual([n["i32"] for n in nodes if n["op"] == "STORE_SLOT" and n["i32"][1] == 0], [])
        img, entries, _ = tm.render(scene, plane, mat, world, size=(16, 12), spp=128)
        self.assertEqual([x for x in entries if x[0] == UNSUPPORTED], [])
        scene, plane, mat, world = tm.new_scene()
        mat.node_tree.nodes.remove(principled(mat))
        r = new_principled(mat, red)
        mat.node_tree.links.new(r.outputs[0], tm.node(mat.node_tree, "ShaderNodeOutputMaterial").inputs["Surface"])
        ref = tm.render(scene, plane, mat, world, size=(16, 12), spp=128)[0]
        np.testing.assert_allclose(tm.centre_rgb(img), 0.5 * tm.centre_rgb(ref), atol=0.02)

    def _frequency(self, build, want, label):
        """Mean of the rendered Mix over the mean of its white child alone: the other children are black, so
        the ratio is the frequency the white child is selected with, binomial over every sample."""
        size, spp = (16, 16), 400
        scene, plane, mat, world = tm.new_scene(size=size)
        empty_material(mat)
        white = new_principled(mat, (1.0, 1.0, 1.0))
        mat.node_tree.links.new(white.outputs[0], tm.node(mat.node_tree, "ShaderNodeOutputMaterial").inputs["Surface"])
        ref, entries, _ = tm.render(scene, plane, mat, world, size=size, spp=spp)
        self.assertEqual([x for x in entries if x[0] == UNSUPPORTED], [])
        a = float(ref[:, :, :3].mean())
        self.assertAlmostEqual(a, 1.0, delta=0.01, msg="the white child is the furnace's lossless Principled")
        mat.node_tree.nodes.remove(white)
        build(mat)
        img, entries, _ = tm.render(scene, plane, mat, world, size=size, spp=spp)
        self.assertEqual([x for x in entries if x[0] == UNSUPPORTED], [])
        got = float(img[:, :, :3].mean()) / a
        se = (want * (1.0 - want) / (size[0] * size[1] * spp)) ** 0.5
        print(f"RESULT mix {label}: selected {got:.5f} of {want} at {size[0] * size[1] * spp} samples, SE {se:.5f}")
        self.assertLess(abs(got - want), 3.0 * se, f"{label}: {got} against {want}")

    def test_mix_selection_frequency(self):
        """Mix(white, black, 0.3) selects the white shader with probability 0.7: selected, never lerped."""
        def build(mat):
            new_mix(mat, new_principled(mat, (1.0, 1.0, 1.0)), new_principled(mat, (0.0, 0.0, 0.0), level=0.0), 0.3)
        self._frequency(build, 0.7, "Mix(white, black, 0.3)")

    def test_nested_mix(self):
        """Mix(Mix(white, black, 0.5), black, 0.5): two BRANCH_MIX over three children, the inner one tag 1
        named by the outer as -2; the white child is selected with probability 0.25. A float link into
        the factor goes through COLOR_TO_F32."""
        def build(mat):
            def black():
                return new_principled(mat, (0.0, 0.0, 0.0), level=0.0)
            inner = new_mix(mat, new_principled(mat, (1.0, 1.0, 1.0)), black(), 0.5)
            new_mix(mat, inner, black(), 0.5)
        self._frequency(build, 0.25, "Mix(Mix(white, black, 0.5), black, 0.5)")
        scene, plane, mat, world = tm.new_scene()
        empty_material(mat)
        build(mat)
        em = lower(mat)
        self.assertEqual(sorted(n["i32"] for n in tm.decode(em) if n["op"] == "BRANCH_MIX"), [[0, -2, 2], [1, 0, 1]])
        self.assertEqual(sorted(n["i32"][1] for n in tm.decode(em) if n["op"] == "STORE_SLOT"),
                         [0, 0, 0, 1, 1, 1, 2, 2, 2])
        t = mat.node_tree
        tex = t.nodes.new("ShaderNodeTexImage")
        tex.image = bpy.data.images.new("Fac", 4, 4)
        outer = tm.node(t, "ShaderNodeOutputMaterial").inputs["Surface"].links[0].from_node
        t.links.new(tex.outputs["Color"], outer.inputs["Fac"])
        em = lower(mat)
        self.assertEqual(tm.unsupported_strings(em), [])
        nodes = tm.decode(em)
        outer_mix = next(i for i, n in enumerate(nodes) if n["op"] == "BRANCH_MIX" and n["i32"][0] == 0)
        self.assertEqual(nodes[tm.inputs_of(em, outer_mix)[0]]["op"], "COLOR_TO_F32")

    def test_transparent_folds_into_opacity(self):
        """A white Transparent BSDF is geometry_opacity 0 and the world shows through; a coloured one is folded
        by its mean and the compiler reports it NODE_APPROXIMATED once (0005 item 38)."""
        scene, plane, mat, world = tm.new_scene()
        only_shader(mat, "ShaderNodeBsdfTransparent")
        em = lower(mat)
        self.assertEqual(tm.unsupported_strings(em), [])
        src = stores(em)[SLOT["geometry_opacity"]]
        self.assertEqual((src["op"], src["f32"]), ("CONST_F32", [0.0]))
        self.assertEqual(sorted(stores(em)), [SLOT["geometry_opacity"]])
        img, entries, _ = tm.render(scene, plane, mat, world, size=(16, 12), spp=128)
        self.assertEqual([x for x in entries if x[0] == UNSUPPORTED] + transparent_entries(entries), [])
        np.testing.assert_allclose(tm.centre_rgb(img), [1.0, 1.0, 1.0], atol=0.005)
        # Under a Mix, a coloured Transparent (opacity 1 - mean) is reported once, white never.
        for rgb, count in (((1.0, 1.0, 1.0), 0), ((0.2, 0.5, 0.8), 1)):
            with self.subTest(colour=rgb):
                scene, plane, mat, world = tm.new_scene()
                empty_material(mat)
                new_mix(mat, new_transparent(mat, rgb), new_principled(mat, (0.8, 0.1, 0.1)), 0.4)
                em = lower(mat)
                self.assertEqual(tm.unsupported_strings(em), [])
                first = next(n for n in tm.decode(em) if n["op"] == "CONST_F32")  # the opacity: child 0's only node
                self.assertAlmostEqual(first["f32"][0], 1.0 - sum(rgb) / 3.0, places=6)
                _, entries, _ = tm.render(scene, plane, mat, world, size=(16, 12), spp=16)
                self.assertEqual([x for x in entries if x[0] == UNSUPPORTED], [])
                self.assertEqual(len(transparent_entries(entries)), count, entries)
        # Alone, a coloured Transparent has no lowering (the compiler would read it as a plain material).
        scene, plane, mat, world = tm.new_scene()
        n = only_shader(mat, "ShaderNodeBsdfTransparent")
        n.inputs["Color"].default_value = (0.5, 0.5, 0.5, 1.0)
        em = lower(mat)
        self.assertEqual(tm.unsupported_strings(em), ["Transparent.Color"])
        self.assertNotIn(SLOT["geometry_opacity"], stores(em))
        _, entries, _ = tm.render(scene, plane, mat, world, size=(8, 8), spp=1)
        self.assertEqual(len([x for x in entries if x[0] == UNSUPPORTED]), 1)
        # A colour from a texture has no constant mean: reported by name.
        scene, plane, mat, world = tm.new_scene()
        n = only_shader(mat, "ShaderNodeBsdfTransparent")
        tex = mat.node_tree.nodes.new("ShaderNodeTexImage")
        tex.image = bpy.data.images.new("Tc", 4, 4)
        mat.node_tree.links.new(tex.outputs["Color"], n.inputs["Color"])
        self.assertEqual(tm.unsupported_strings(lower(mat)), ["Transparent.Color"])

    def test_metallic_reaches_the_record(self):
        scene, plane, mat, world = tm.new_scene()
        p = principled(mat)
        slot = SLOT["base_metalness"]
        self.assertNotIn(slot, stores(lower(mat)), "Metallic 0 stores nothing")
        p.inputs["Metallic"].default_value = 1.0
        em = lower(mat)
        self.assertEqual(tm.unsupported_strings(em), [])
        self.assertEqual((stores(em)[slot]["op"], stores(em)[slot]["f32"]), ("CONST_F32", [1.0]))
        # Cycles' metal ignores Specular IOR Level and the engine's is scaled by 2L: reported, never approximated.
        p.inputs["Specular IOR Level"].default_value = 0.8
        self.assertEqual(tm.unsupported_strings(lower(mat)), ["Principled BSDF.Specular IOR Level"])
        p.inputs["Specular IOR Level"].default_value = 0.5
        p.inputs["Specular Tint"].default_value = (0.5, 0.5, 0.5, 1.0)
        self.assertEqual(tm.unsupported_strings(lower(mat)), ["Principled BSDF.Specular Tint"])
        p.inputs["Specular Tint"].default_value = (1.0, 1.0, 1.0, 1.0)
        # A linked level has no constant to compare with 0.5: reported too, and no metal is stored.
        lvl = mat.node_tree.nodes.new("ShaderNodeTexImage")
        lvl.image = bpy.data.images.new("Lvl", 4, 4)
        mat.node_tree.links.new(lvl.outputs["Color"], p.inputs["Specular IOR Level"])
        em = lower(mat)
        self.assertEqual(tm.unsupported_strings(em), ["Principled BSDF.Specular IOR Level"])
        self.assertNotIn(slot, stores(em))
        mat.node_tree.links.remove(p.inputs["Specular IOR Level"].links[0])
        # A link goes through the float path (a colour becomes its luminance).
        tex = mat.node_tree.nodes.new("ShaderNodeTexImage")
        tex.image = bpy.data.images.new("Met", 4, 4)
        mat.node_tree.links.new(tex.outputs["Color"], p.inputs["Metallic"])
        em = lower(mat)
        self.assertEqual(tm.unsupported_strings(em), [])
        self.assertEqual(stores(em)[slot]["op"], "COLOR_TO_F32")
        # In a Mix the store carries its child.
        empty_material(mat)
        metal = new_principled(mat, (1.0, 1.0, 1.0))
        metal.inputs["Metallic"].default_value = 1.0
        new_mix(mat, new_transparent(mat), metal, 0.5)
        met = [n["i32"] for n in tm.decode(lower(mat)) if n["op"] == "STORE_SLOT" and n["i32"][0] == slot]
        self.assertEqual(met, [[slot, 1]])
        # Rendered, a rough white metal under a white world is the world (energy compensation, 163).
        scene, plane, mat, world = tm.new_scene()
        p = principled(mat)
        p.inputs["Base Color"].default_value = (1.0, 1.0, 1.0, 1.0)
        p.inputs["Metallic"].default_value = 1.0
        p.inputs["Roughness"].default_value = 0.3
        img, entries, _ = tm.render(scene, plane, mat, world, size=(32, 24), spp=64)
        self.assertEqual([x for x in entries if x[0] == UNSUPPORTED], [])
        mean = float(img[:, :, :3].mean())
        # Band: 163's metal furnace, Ref: tests/cpu/test_lobe_stack.cpp "lobe_stack/metal white furnace".
        print(f"RESULT metallic plane mean {mean:.5f}")
        self.assertGreaterEqual(mean, 0.99)
        self.assertLessEqual(mean, 1.002)

    def _sphere(self, scene, material):
        """A smooth unit sphere at the origin."""
        import bmesh

        bm = bmesh.new()
        bmesh.ops.create_uvsphere(bm, u_segments=48, v_segments=24, radius=1.0)
        me = bpy.data.meshes.new("Ball")
        bm.to_mesh(me)
        bm.free()
        me.polygons.foreach_set("use_smooth", [True] * len(me.polygons))
        me.materials.append(material)
        ob = bpy.data.objects.new("Ball", me)
        scene.collection.objects.link(ob)
        return ob

    def test_mix_and_metal_evidence(self):
        """The metallic sphere, a Mix(Transparent, red, 0.5) plane and a Mix(Emission, red, 0.5) plane beside
        each other (out/evidence/166/mix_metal.png) and their means: the sphere is the white world, the Mix
        planes are half the world or the emission plus half the red plane alone."""
        red, ems, size, spp = (0.8, 0.1, 0.1), (1.0, 0.5, 0.2), (48, 36), 64
        shots = {}
        # Sphere: the plane is a white Transparent, so the sphere stands in the world alone.
        scene, plane, mat, world = tm.new_scene(size=size)
        only_shader(mat, "ShaderNodeBsdfTransparent")
        metal = bpy.data.materials.new("Metal")
        metal.use_nodes = True
        p = principled(metal)
        p.inputs["Base Color"].default_value = (1.0, 1.0, 1.0, 1.0)
        p.inputs["Metallic"].default_value = 1.0
        p.inputs["Roughness"].default_value = 0.3
        ball = self._sphere(scene, metal)
        bpy.context.view_layer.update()
        shots["metal"] = tm.render(scene, plane, mat, world, extra=((ball, metal),), size=size, spp=spp)[0]
        for name in ("red", "transparent", "emission"):
            scene, plane, mat, world = tm.new_scene(size=size)
            empty_material(mat)
            r = new_principled(mat, red)
            if name == "red":
                mat.node_tree.links.new(r.outputs[0], tm.node(mat.node_tree, "ShaderNodeOutputMaterial").inputs["Surface"])
            elif name == "transparent":
                new_mix(mat, new_transparent(mat), r, 0.5)
            else:
                new_mix(mat, new_emission(mat, ems, 2.0), r, 0.5)
            img, entries, _ = tm.render(scene, plane, mat, world, size=size, spp=spp)
            self.assertEqual([x for x in entries if x[0] == UNSUPPORTED], [])
            shots[name] = img
        def middle(img):  # inside the sphere's disc (it fills 0.58 of the half-frame), clear of its silhouette
            h, w = img.shape[:2]
            return img[h // 2 - 5:h // 2 + 5, w // 2 - 5:w // 2 + 5, :3].reshape(-1, 3).mean(axis=0)
        means = {k: middle(v) for k, v in shots.items()}
        print(f"RESULT mix_metal means {means}")
        np.testing.assert_allclose(means["metal"], [1.0, 1.0, 1.0], atol=0.012)
        np.testing.assert_allclose(means["transparent"], 0.5 * np.ones(3) + 0.5 * means["red"], atol=0.02)
        np.testing.assert_allclose(means["emission"], 0.5 * np.array([2.0, 1.0, 0.4]) + 0.5 * means["red"], atol=0.02)
        srgb_png(os.path.join(EVIDENCE_166, "mix_metal.png"),
                 np.concatenate([shots[k][:, :, :3] for k in ("metal", "transparent", "emission")], axis=1))

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
