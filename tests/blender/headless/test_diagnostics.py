# SPDX-License-Identifier: Apache-2.0
"""The Diagnostics panel lists the last render's report (0007 d38, 0001 D2.11).

F12 on the CPU device runs through render_final's Job as the RenderEngine drives it; the store
(diagnostics.py) is read afterwards and the panel's draw is called with a recording layout, so no window
is needed. Unsupported nodes are the report source: a Wave or Magic Texture linked into the shader makes the
engine raise one NODE_UNSUPPORTED entry for the material (0005 item 40).
"""
import importlib
import math
import os
import sys
import tempfile
import types
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import support  # noqa: E402
from test_render_final_cpu import Engine  # noqa: E402

_PKG = "ar_diag_addon"
BIN = os.path.join(support.ROOT, "out", "bin")
SIZE, SPP = 16, 2
UNSUPPORTED = ("ShaderNodeTexWave", "ShaderNodeTexMagic", "ShaderNodeTexBrick")


def load():
    """rf, native, diagnostics, panels, ui of the addon without its __init__, bound to the built core."""
    support.stage_addon(_PKG)
    return tuple(importlib.import_module(f"{_PKG}.{m}") for m in
                 ("render_final", "native", "diagnostics", "ui.panels", "ui"))


def scene_with(unsupported):
    """A lit plane in view with one material; `unsupported` is how many unsupported nodes it holds."""
    import bpy

    scene = support.reset_scene()
    scene.render.resolution_x = scene.render.resolution_y = SIZE
    scene.render.resolution_percentage = 100
    me = bpy.data.meshes.new("Plane")
    me.from_pydata([(-8, -8, 0), (8, -8, 0), (8, 8, 0), (-8, 8, 0)], [], [(0, 1, 2, 3)])
    plane = bpy.data.objects.new("Plane", me)
    scene.collection.objects.link(plane)
    cd = bpy.data.cameras.new("Cam")
    cd.sensor_fit, cd.angle = "AUTO", 2 * math.atan(8.0 / 5.0)
    cam = bpy.data.objects.new("Cam", cd)
    scene.collection.objects.link(cam)
    cam.location = (0.0, 0.0, 5.0)
    scene.camera = cam
    world = bpy.data.worlds.new("World")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.5, 0.5, 0.5, 1)
    scene.world = world
    mat = bpy.data.materials.new("Magenta")
    mat.use_nodes = True
    plane.data.materials.append(mat)
    # Linked into the principled shader: today the engine reports an unsupported node only when the graph
    # reaches it (an unlinked one is listed once 165 lands). Different sockets, so the links do not replace each other.
    for kind, socket in zip(UNSUPPORTED[:unsupported], ("Base Color", "Emission Color", "Roughness")):
        node = mat.node_tree.nodes.new(kind)
        mat.node_tree.links.new(node.outputs[0], mat.node_tree.nodes["Principled BSDF"].inputs[socket])
    bpy.context.view_layer.update()
    scene.astroray.device = "CPU"
    scene.astroray.samples = scene.astroray.viewport_samples = SPP
    return scene


class Recorder:
    """A UILayout stand-in: every label's text, in order; sub-layouts are itself."""

    def __init__(self):
        self.labels, self.props = [], []

    def label(self, text="", icon="NONE", **_):
        self.labels.append((text, icon))

    def prop(self, data, name, **_):
        self.props.append(name)

    def __getattr__(self, name):  # column, row, box, separator, ...
        return lambda *a, **k: self


def draw(panel, context=None):
    rec = Recorder()
    panel.draw(types.SimpleNamespace(layout=rec), context or types.SimpleNamespace(scene=None))
    return rec


def draw_header(panel):
    rec = Recorder()
    panel.draw_header(types.SimpleNamespace(layout=rec), None)
    return rec


class TestDiagnostics(unittest.TestCase):
    def setUp(self):
        support.acquire_properties()
        self.addCleanup(support.release_properties)
        self.rf, self.native, self.diag, self.panels, self.ui = load()
        self.diag.reset()
        self.addCleanup(self.diag.reset)
        self._dir = tempfile.TemporaryDirectory()
        self.addCleanup(self._dir.cleanup)

    def render(self, unsupported):
        scene = scene_with(unsupported)
        engine = Engine()
        with mock.patch.object(self.native, "_bin_dir", lambda: BIN),                 mock.patch.object(self.rf, "_log_path", lambda: os.path.join(self._dir.name, "context.log")):
            job = self.rf.Job()
            try:
                job.update(support.depsgraph())
                job.render(engine)
            finally:
                job.close()
                self.rf.shutdown()
        return scene, engine

    def entries(self):
        report = self.diag.latest()
        return [] if report is None else report.entries

    def test_unsupported_node_listed_once(self):
        self.render(1)
        hits = [e for e in self.entries() if e.code == "NODE_UNSUPPORTED"]
        self.assertEqual(len(hits), 1, self.entries())
        e = hits[0]
        self.assertEqual(e.level, "WARNING")
        self.assertIn("Magenta", e.subject)
        self.assertIn("ShaderNodeTexWave", e.detail)
        rec = draw(self.panels.ASTRORAY_PT_diagnostics)
        subject_rows = [(t, icon) for t, icon in rec.labels if "Magenta" in t]
        self.assertEqual(len(subject_rows), 1, rec.labels)
        self.assertEqual(subject_rows[0][1], "ERROR")  # the warning triangle
        self.assertEqual(sum(1 for t, _ in rec.labels if "ShaderNodeTexWave" in t), 1, rec.labels)

    def test_two_unsupported_nodes_listed_twice(self):
        self.render(2)
        hits = [e for e in self.entries() if e.code == "NODE_UNSUPPORTED"]
        self.assertTrue(all(any(n in e.detail for n in UNSUPPORTED[:2]) for e in hits), hits)
        self.assertEqual(len({e.detail for e in hits}), 2, hits)
        self.assertEqual(len(hits), 2, self.entries())
        rec = draw(self.panels.ASTRORAY_PT_diagnostics)
        self.assertEqual(sum(1 for t, _ in rec.labels if "Magenta" in t), 2, rec.labels)

    def test_clean_scene_has_no_warnings_and_empty_report_says_no_entries(self):
        self.render(0)
        # The engine's own INFO entries (pool capacity) are listed; a clean scene adds no WARNING or ERROR.
        self.assertEqual([e for e in self.entries() if e.level != "INFO"], [])
        self.diag.begin("FINAL", "CPU")
        rec = draw(self.panels.ASTRORAY_PT_diagnostics)
        self.assertIn("no entries", [t for t, _ in rec.labels])
        self.assertIn("Device in use: CPU", [t for t, _ in rec.labels])

    def test_second_render_replaces_entries(self):
        def hits():
            return [e for e in self.entries() if e.code == "NODE_UNSUPPORTED"]
        self.render(2)
        self.assertEqual(len(hits()), 2, self.entries())
        self.render(1)
        self.assertEqual(len(hits()), 1, self.entries())
        self.render(0)
        self.assertEqual(hits(), [], self.entries())

    def test_header_count_is_singular_for_one(self):
        d = self.diag
        d.begin("FINAL", "CPU")
        d.add_engine("FINAL", "WARNING", "NODE_UNSUPPORTED", 10, "material 'M'", "ShaderNodeTexWave")
        header = lambda: draw_header(self.panels.ASTRORAY_PT_diagnostics).labels[0][0]  # noqa: E731
        self.assertEqual(header(), "0 errors, 1 warning")
        d.add_engine("FINAL", "WARNING", "NODE_UNSUPPORTED", 10, "material 'M'", "ShaderNodeTexMagic")
        d.add_engine("FINAL", "ERROR", "GENERATION_REJECTED", 1, "", "bad graph")
        self.assertEqual(header(), "1 error, 2 warnings")

    def test_error_is_listed_first(self):
        d = self.diag
        d.begin("FINAL", "CPU")
        d.add_engine("FINAL", "INFO", "PASS_UNSUPPORTED", 3, "", "Mist")
        d.add_engine("FINAL", "WARNING", "NODE_UNSUPPORTED", 10, "material 'M'", "ShaderNodeTexWave")
        d.add_host("FINAL", "WARNING", "camera 'C': type PANO is not supported")
        d.add_engine("FINAL", "ERROR", "GENERATION_REJECTED", 1, "", "bad graph")
        d.add_engine("FINAL", "ERROR", "GENERATION_REJECTED", 1, "", "bad graph")  # one key, listed once
        order = [(e.level, e.code, e.host) for e in d.latest().ordered()]
        self.assertEqual(order, [("ERROR", "GENERATION_REJECTED", False), ("WARNING", "NODE_UNSUPPORTED", False),
                                 ("WARNING", "", True), ("INFO", "PASS_UNSUPPORTED", False)])
        rec = draw(self.panels.ASTRORAY_PT_diagnostics)
        texts = [t for t, _ in rec.labels]
        self.assertLess(texts.index("GENERATION_REJECTED"), texts.index("NODE_UNSUPPORTED material 'M'"))
        self.assertEqual(texts.count("GENERATION_REJECTED"), 1)

    def test_unregister_restores_builtin_panels(self):
        import bpy

        compat = importlib.import_module(f"{_PKG}.ui.compat_panels")
        panel_classes = self.panels.classes
        for _ in range(2):
            self.ui.register()
            self.assertTrue(all("ASTRORAY" in getattr(bpy.types, n).COMPAT_ENGINES for n in compat.PANELS))
            self.assertTrue(all(hasattr(bpy.types, c.__name__) for c in panel_classes))
            self.ui.unregister()
            self.assertFalse(any(hasattr(bpy.types, c.__name__) for c in panel_classes))
            left = [p.__name__ for p in bpy.types.Panel.__subclasses__()
                    if "ASTRORAY" in getattr(p, "COMPAT_ENGINES", ())
                    and p not in panel_classes]
            self.assertEqual(left, [])


if __name__ == "__main__":
    support.main()
