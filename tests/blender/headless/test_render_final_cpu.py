# SPDX-License-Identifier: Apache-2.0
"""F12 on the CPU device renders the generation update() committed (0007 d25; CLAUDE.md rule 5).

render() starts its session right after update()'s commit. A textured scene's generation activates
only when its image jobs finish, and a final session that latched before that rendered the empty
generation 0, converged on it after one pass and delivered a black frame with no report entry.
The image here is large enough that its decode outlasts the CPU session's start. The test drives
render_final's Job the way the RenderEngine does, against the built core (tools/build.ps1).
"""
import ctypes
import importlib
import importlib.util
import math
import os
import sys
import tempfile
import types
import unittest
from unittest import mock

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import support  # noqa: E402

_PKG = "ar_rf_cpu_addon"
BIN = os.path.join(support.ROOT, "out", "bin")
TEX = 2048   # texels per side: a decode of tens of milliseconds
SIZE = 32    # pixels per side
SPP = 4


def load_addon():
    """render_final and native of the addon without its __init__, bound to the built core."""
    support.stage_addon(_PKG)
    return importlib.import_module(_PKG + ".render_final"), importlib.import_module(_PKG + ".native")


def textured_scene(directory):
    """A UV-mapped plane filling the view, emitting a TEX x TEX noise PNG, on a black world."""
    import bpy

    scene = support.reset_scene()
    scene.render.resolution_x = scene.render.resolution_y = SIZE
    scene.render.resolution_percentage = 100
    me = bpy.data.meshes.new("Plane")
    me.from_pydata([(-8, -8, 0), (8, -8, 0), (8, 8, 0), (-8, 8, 0)], [], [(0, 1, 2, 3)])
    uv = me.uv_layers.new(name="UVMap")
    for loop, co in zip(me.loops, ((0, 0), (1, 0), (1, 1), (0, 1))):
        uv.data[loop.index].uv = co
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
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0, 0, 0, 1)
    scene.world = world
    src = bpy.data.images.new("tex_src", TEX, TEX, alpha=False, float_buffer=False)
    src.pixels[0]  # allocates the buffer: foreach_set on a fresh image writes nothing otherwise
    rgba = np.ones((TEX * TEX, 4), np.float32)
    rgba[:, :3] = np.random.RandomState(135).randint(64, 256, (TEX * TEX, 1)) / 255.0
    src.pixels.foreach_set(rgba.reshape(-1))
    src.file_format, src.filepath_raw = "PNG", os.path.join(directory, "tex.png")
    src.save()
    mat = bpy.data.materials.new("Mat")
    mat.use_nodes = True
    plane.data.materials.append(mat)
    t = mat.node_tree
    p = t.nodes["Principled BSDF"]
    p.inputs["Base Color"].default_value = (0, 0, 0, 1)
    p.inputs["Emission Strength"].default_value = 1.0
    tex = t.nodes.new("ShaderNodeTexImage")
    tex.image = bpy.data.images.load(src.filepath_raw)
    t.links.new(tex.outputs["Color"], p.inputs["Emission Color"])
    bpy.context.view_layer.update()
    scene.astroray.device = "CPU"
    scene.astroray.samples = scene.astroray.viewport_samples = SPP
    return scene


class Engine:
    """The RenderEngine calls render() makes, recording the final Combined pixels and the reports."""

    def __init__(self):
        self.reports, self.pixels = [], None

    def report(self, level, text):
        self.reports.append((tuple(level), text))

    def test_break(self):
        return False

    def begin_result(self, x, y, w, h, layer=None):
        rect = types.SimpleNamespace(data=None)
        rect.foreach_set = lambda values: setattr(rect, "data", np.array(values, np.float32))
        return types.SimpleNamespace(layers=[types.SimpleNamespace(passes={"Combined": types.SimpleNamespace(rect=rect)})],
                                     rect=rect, size=(w, h))

    def end_result(self, result):
        if result.rect.data is not None:  # None when render() failed before its first copy
            w, h = result.size
            self.pixels = result.rect.data.reshape(h, w, 4)

    def update_progress(self, value):
        pass

    def update_stats(self, a, b):
        pass

    def update_result(self, result):
        pass


class TestF12Cpu(unittest.TestCase):
    def setUp(self):
        support.acquire_properties()
        self.addCleanup(support.release_properties)
        self._dir = tempfile.TemporaryDirectory()
        self.addCleanup(self._dir.cleanup)

    def test_textured_scene_renders_the_committed_generation(self):
        rf, native = load_addon()
        scene = textured_scene(self._dir.name)
        log = os.path.join(self._dir.name, "context.log")
        seen = {}
        copy = rf.Job._copy_combined

        def copy_final(job, engine, result, w, h, final=False):
            if final:  # the session is still open: read what it rendered
                prog = job.abi.ar_progress()
                native.call(job.lib.ar_session_get_progress, job.session, ctypes.byref(prog))
                seen.update(active=prog.active_generation, state=prog.state, spp=prog.samples_per_pixel)
            copy(job, engine, result, w, h, final)

        with mock.patch.object(native, "_bin_dir", lambda: BIN), mock.patch.object(rf, "_log_path", lambda: log), \
                mock.patch.object(rf.Job, "_copy_combined", copy_final):
            job, engine = rf.Job(), Engine()
            try:
                job.update(support.depsgraph())
                job.render(engine)
            finally:
                job.close()
                rf.shutdown()
        abi = sys.modules[_PKG + "._abi"]
        self.assertEqual(seen["state"], abi.AR_STATE_CONVERGED)
        self.assertGreaterEqual(seen["active"], job.generation, f"rendered an older scene; reports {engine.reports}")
        self.assertEqual(seen["spp"], SPP)
        mean = float(engine.pixels[:, :, :3].mean())
        self.assertGreater(mean, 0.1, f"black frame (mean {mean}); reports {engine.reports}")


class StaleSession:
    """The ar_session_* calls Job._wait makes, for a session that converged on generation 0 with no report
    entry: what a final session that latched before update()'s commit activated would show."""

    def __init__(self, abi):
        self.abi = abi

    def ar_session_wait(self, session, timeout_ms):
        return 0

    def ar_session_get_progress(self, session, out):
        prog = out._obj
        prog.state, prog.active_generation, prog.pending_generation, prog.samples_per_pixel = \
            self.abi.AR_STATE_CONVERGED, 0, 1, 1
        return 0

    def ar_session_report_info(self, session, out):
        out._obj.entry_count, out._obj.revision = 0, 1
        return 0


class TestStaleGeneration(unittest.TestCase):
    def test_converging_below_the_committed_generation_fails_the_render(self):
        # Rule 5: the image of an older scene than update() committed is never delivered as a result.
        rf, _ = load_addon()
        abi = sys.modules[_PKG + "._abi"]
        job = rf.Job.__new__(rf.Job)
        job.lib, job.abi, job.session, job.generation = StaleSession(abi), abi, ctypes.c_void_p(1), 1
        engine = Engine()
        with mock.patch.object(rf.Job, "_copy_combined", lambda *a, **k: None):
            with self.assertRaisesRegex(rf.RenderFailed, "converged on generation 0, but update\\(\\) committed 1"):
                job._wait(engine, None, SIZE, SIZE, SPP)


if __name__ == "__main__":
    support.main()
