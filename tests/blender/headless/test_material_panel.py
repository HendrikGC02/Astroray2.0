# SPDX-License-Identifier: Apache-2.0
"""Material tab "Astroray Extension" panel: kind GRATING on `material.astroray`, synced into ext_kind and
ext_constants (0005 items 16, 23; 0007 d37, V17).

Headless: the translation is read from the emitter and from the ar_material_desc that set_material sends (a
recording edit stands in for the ABI call), the panel's draw runs against a recording layout, and edits are
classified through a depsgraph update, so no window or render is needed.
Needs the built tree (tools/build.ps1) for the generated bindings, as tests/blender/test_materials.py does.
"""
import os
import re
import sys
import types
import unittest

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import support  # noqa: E402

sys.path.insert(0, os.path.dirname(HERE))
import test_materials as tm  # noqa: E402  (the module, not its TestCase)

support.load_sync()
from ar_sync import cache as cache_mod  # noqa: E402
from ar_sync import classify as C  # noqa: E402

ROOT = support.ROOT
AR = tm._load("ar_scene_lib", os.path.join(ROOT, "scenes", "lib", "ar.py"))
PANEL = tm._load("ar_material_panel", os.path.join(ROOT, "blender", "astroray", "ui", "material_panel.py"))
HEADER = os.path.join(ROOT, "src", "host", "material", "grating_authored.h")
# One value per property, each different from the default and exactly representable in float32.
EDITS = {"grating_pitch_nm": 2000.0, "grating_depth_nm": 150.0, "grating_m_min": -3, "grating_m_max": 3,
         "grating_coating_n": 0.5, "grating_coating_k": 4.0}


class Layout:
    """Records what a panel draws: props, labels (text, icon) and the `enabled` state of the column they sit in."""

    def __init__(self, log=None, enabled=True):
        self.log, self.enabled = [] if log is None else log, enabled

    def column(self):
        return Layout(self.log, self.enabled)

    def prop(self, data, name, **kw):
        self.log.append(("prop", name, self.enabled))

    def label(self, text="", icon="NONE"):
        self.log.append(("label", text, icon))


def drawn(mat):
    layout = Layout()
    ctx = types.SimpleNamespace(material=mat, engine="ASTRORAY")
    PANEL.ASTRORAY_PT_material_extension.draw(types.SimpleNamespace(layout=layout), ctx)
    return layout.log


class RecordingEdit:
    def __init__(self):
        self.calls = []

    def call(self, name, *args):
        self.calls.append((name, args))


def sent_desc(em, key=7):
    """The ar_material_desc set_material hands to ar_edit_set_material for a translation."""
    edit = RecordingEdit()
    tm.translate.set_material(edit, tm.ABI, tm.keys.KeyMap(), key, em)
    (_, args), = [c for c in edit.calls if c[0] == "ar_edit_set_material"]
    return args[1]._obj


def desc_bytes(desc):
    return bytes(desc.ext_constants)


def lowered(mat):
    return tm.translate.translate_material(mat, tm.keys.KeyMap(), tm.OP)


class TestMaterialPanel(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tm.PROPS.register()

    @classmethod
    def tearDownClass(cls):
        tm.PROPS.unregister()

    def setUp(self):
        self.scene, self.plane, self.mat, self.world = tm.new_scene()

    def set_grating(self, **over):
        p = self.mat.astroray
        p.ext_kind = "GRATING"
        for k, v in {**EDITS, **over}.items():
            setattr(p, k, v)
        return p

    def test_bytes_equal_scene_builder(self):
        # A plain material sends kind 0 and 64 zero bytes.
        d = sent_desc(lowered(self.mat))
        self.assertEqual((d.ext_kind, desc_bytes(d)), (0, bytes(64)))
        # A grating's bytes are the scene builder's for the same values.
        p = self.set_grating()
        d = sent_desc(lowered(self.mat))
        want = AR.grating_block(p.grating_pitch_nm, p.grating_depth_nm, (p.grating_coating_n, p.grating_coating_k),
                                p.grating_m_min, p.grating_m_max)
        self.assertEqual(d.ext_kind, 1)
        self.assertEqual(desc_bytes(d), want)
        # The addon's mirrored layout follows the header's field list (the one source ar.py reads).
        self.assertEqual(tm.PROPS.GRATING_BLOCK_FIELDS, tuple(AR.GRATING_FIELDS))
        self.assertEqual(tm.PROPS.GRATING_BLOCK_FORMAT, AR.GRATING_FORMAT)
        self.assertEqual(tm.PROPS.ext_block(p), (1, want))

    def test_each_property_changes_bytes(self):
        self.set_grating(**{k: v for k, v in zip(EDITS, (1600.0, 100.0, -2, 2, 1.0, 6.0))})
        base = desc_bytes(sent_desc(lowered(self.mat)))
        for name, value in EDITS.items():
            with self.subTest(prop=name):
                setattr(self.mat.astroray, name, value)
                changed = desc_bytes(sent_desc(lowered(self.mat)))
                self.assertNotEqual(changed, base)
                setattr(self.mat.astroray, name, {"grating_pitch_nm": 1600.0, "grating_depth_nm": 100.0,
                                                  "grating_m_min": -2, "grating_m_max": 2,
                                                  "grating_coating_n": 1.0, "grating_coating_k": 6.0}[name])
                self.assertEqual(desc_bytes(sent_desc(lowered(self.mat))), base)

    def test_edit_arrives_as_material_update(self):
        """Every property edit is a MATERIAL depsgraph update, so classify re-translates the material."""
        cache = cache_mod.SyncCache()
        cache.remember(support.depsgraph(), self.scene)
        for name, value in {"ext_kind": "GRATING", **EDITS}.items():
            with self.subTest(prop=name):
                acts = support.in_update(lambda: setattr(self.mat.astroray, name, value),
                                         lambda dg: C.classify(dg, cache))
                self.assertIn((C.MATERIALS, "MATERIAL"), [(a.kind, a.id_type) for a in acts or ()])
                cache.remember(support.depsgraph(), self.scene)

    def test_kind_with_mix_reports_once(self):
        t = self.mat.node_tree
        mix = t.nodes.new("ShaderNodeMixShader")
        out = tm.node(t, "ShaderNodeOutputMaterial")
        principled = tm.node(t, "ShaderNodeBsdfPrincipled")
        t.links.new(principled.outputs[0], mix.inputs[1])
        t.links.new(principled.outputs[0], mix.inputs[2])
        t.links.new(mix.outputs[0], out.inputs["Surface"])
        self.assertEqual(tm.unsupported_strings(lowered(self.mat)), [])  # a Mix alone is lowered
        self.set_grating()
        em = lowered(self.mat)
        self.assertEqual(tm.unsupported_strings(em), ["Material.Astroray Extension with Mix Shader"])
        # Only the report remains in the graph: no store reaches the record, so the material is magenta.
        self.assertEqual([n["op"] for n in tm.decode(em)], ["UNSUPPORTED"])

    def test_rows_greyed_with_one_info_line(self):
        self.assertEqual(drawn(self.mat), [("prop", "ext_kind", True)])  # kind None: no grating rows
        self.set_grating()
        self.assertFalse(PANEL.GRATING_RENDERS)
        log = drawn(self.mat)
        self.assertEqual([e for e in log if e[0] == "label" and e[2] == "INFO"], [("label", PANEL.NOT_YET, "INFO")])
        rows = [e for e in log if e[0] == "prop" and e[1] != "ext_kind"]
        self.assertEqual([e[1] for e in rows], list(PANEL.GRATING_ROWS))
        self.assertEqual(len(rows), 6)
        self.assertTrue(all(not e[2] for e in rows), rows)
        lines = [e[1] for e in log if e[0] == "label" and e[1].endswith("lines/mm")]
        self.assertEqual(lines, ["500.0 lines/mm"])  # the pitch EDITS sets, 2000 nm
        # Routing flips the constant: the rows are live and the line is gone.
        PANEL.GRATING_RENDERS = True
        try:
            log = drawn(self.mat)
        finally:
            PANEL.GRATING_RENDERS = False
        self.assertEqual([e for e in log if e[0] == "label" and e[2] == "INFO"], [])
        self.assertTrue(all(e[2] for e in log if e[0] == "prop"))
        # The panel is offered to this engine only.
        self.assertTrue(PANEL.ASTRORAY_PT_material_extension.poll(types.SimpleNamespace(engine="ASTRORAY", material=self.mat)))
        self.assertFalse(PANEL.ASTRORAY_PT_material_extension.poll(types.SimpleNamespace(engine="CYCLES", material=self.mat)))

    def test_limits_equal_the_parse_limits(self):
        """RNA limits come from the header's parse limits (read from the header text here)."""
        with open(HEADER, encoding="utf-8") as f:
            text = f.read()
        lo, hi = map(int, re.search(r"m_min < (-?\d+) \|\| g\.m_min > (\d+)", text).groups())
        self.assertEqual(re.findall(r"outside (-?\d+)\.\.(\d+)", text), [(str(lo), str(hi))] * 2)
        p = self.mat.astroray
        for name in ("grating_m_min", "grating_m_max"):
            prop = p.bl_rna.properties[name]
            self.assertEqual((prop.hard_min, prop.hard_max), (lo, hi), name)
            # Blender clamps an out-of-range assignment to the limit, so no value past it is stored.
            for bad, kept in ((lo - 1, lo), (hi + 1, hi)):
                setattr(p, name, bad)
                self.assertEqual(getattr(p, name), kept, name)
        self.assertEqual(tm.PROPS.GRATING_ORDER_LIMIT, hi)
        # pitch and depth must be positive, coating n and k not negative (an exclusive bound cannot be an RNA limit).
        for name in ("grating_pitch_nm", "grating_depth_nm"):
            self.assertTrue(0.0 < p.bl_rna.properties[name].hard_min <= 1.001e-3, name)
            setattr(p, name, 0.0)
            self.assertGreater(getattr(p, name), 0.0, name)
        for name in ("grating_coating_n", "grating_coating_k"):
            self.assertEqual(p.bl_rna.properties[name].hard_min, 0.0, name)
        self.assertIn("is not finite and positive", text)
        self.assertIn("is not finite and >= 0", text)
        # Whatever the RNA accepts packs into a block the header's parser accepts: the defaults do.
        self.assertEqual(len(tm.PROPS.pack_grating(*[tm.PROPS.GRATING_DEFAULTS[k] for k in (
            "pitch_nm", "depth_nm", "coating_n", "coating_k", "m_min", "m_max")])), 64)

    def test_register_twice_leaves_no_class(self):
        names = ("AstrorayScene", "AstrorayMaterial")
        for _ in range(2):
            tm.PROPS.unregister()
            self.assertFalse(hasattr(bpy.types.Material, "astroray"))
            self.assertFalse(hasattr(bpy.types.Scene, "astroray"))
            for n in names:
                self.assertFalse(hasattr(bpy.types, n), n)
            tm.PROPS.register()
            self.assertTrue(hasattr(bpy.types.Material, "astroray"))
        for _ in range(2):
            for c in PANEL.classes:
                bpy.utils.register_class(c)
            self.assertTrue(hasattr(bpy.types, "ASTRORAY_PT_material_extension"))
            for c in reversed(PANEL.classes):
                bpy.utils.unregister_class(c)
            self.assertFalse(hasattr(bpy.types, "ASTRORAY_PT_material_extension"))
        self.assertEqual(self.mat.astroray.ext_kind, "NONE")


if __name__ == "__main__":
    support.main()
