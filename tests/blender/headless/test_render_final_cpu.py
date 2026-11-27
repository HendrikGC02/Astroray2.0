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
CARD_SPP = 32


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


def cards_scene():
    """Pinhole camera 5 m above z = 0 looking down, a world of radiance 0.5, and three regions whose pixel boundaries
    are the frame's centre lines: an opaque diffuse plane on the left half (depth 5), an opacity-0.5 card at z = 1
    over the upper right (depth 4), and nothing over the lower right (the world)."""
    import bpy

    scene = support.reset_scene()
    scene.render.resolution_x = scene.render.resolution_y = SIZE
    scene.render.resolution_percentage = 100
    cd = bpy.data.cameras.new("Cam")
    cd.sensor_fit, cd.angle = "AUTO", 2 * math.atan(8.0 / 5.0)
    cam = bpy.data.objects.new("Cam", cd)
    scene.collection.objects.link(cam)
    cam.location = (0.0, 0.0, 5.0)
    scene.camera = cam
    for name, (x0, x1, y0, y1), z, alpha in (("Opaque", (-8, 0, -8, 8), 0.0, 1.0), ("Card", (0, 8, 0, 8), 1.0, 0.5)):
        me = bpy.data.meshes.new(name)
        me.from_pydata([(x0, y0, z), (x1, y0, z), (x1, y1, z), (x0, y1, z)], [], [(0, 1, 2, 3)])
        ob = bpy.data.objects.new(name, me)
        scene.collection.objects.link(ob)
        mat = bpy.data.materials.new(name)
        mat.use_nodes = True
        mat.node_tree.nodes["Principled BSDF"].inputs["Alpha"].default_value = alpha
        ob.data.materials.append(mat)
    world = bpy.data.worlds.new("World")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.5, 0.5, 0.5, 1)
    scene.world = world
    bpy.context.view_layer.update()
    scene.astroray.device = "CPU"
    scene.astroray.samples = scene.astroray.viewport_samples = CARD_SPP
    return scene


# Pixel regions of cards_scene's 32 x 32 frame, rows bottom-up, three pixels clear of every boundary (the pixel
# filter reaches 1.5 pixels across one).
def _region(rows, cols):
    return (slice(*rows), slice(*cols))


LEFT, UPPER_RIGHT, LOWER_RIGHT = _region((3, 13), (3, 13)), _region((19, 29), (19, 29)), _region((3, 13), (19, 29))
LEFT_UP = _region((19, 29), (3, 13))


class Engine:
    """The RenderEngine calls render() makes, recording the final pixels of every pass the result holds
    (`passes`: name -> flat floats) and the reports. add_pass makes a pass exist as Blender's does."""

    def __init__(self):
        self.reports, self.pixels, self.passes, self.added = [], None, {}, []

    def report(self, level, text):
        self.reports.append((tuple(level), text))

    def test_break(self):
        return False

    def add_pass(self, name, channels, chan_id, layer=""):
        self.added.append((name, channels, chan_id))

    def begin_result(self, x, y, w, h, layer=None):
        def make():
            rect = types.SimpleNamespace(data=None)
            rect.foreach_set = lambda values: setattr(rect, "data", np.array(values, np.float32))
            return types.SimpleNamespace(rect=rect)

        passes = {"Combined": make(), **{name: make() for name, _, _ in self.added}}
        return types.SimpleNamespace(layers=[types.SimpleNamespace(passes=passes)], size=(w, h))

    def end_result(self, result):
        w, h = result.size
        for name, p in result.layers[0].passes.items():
            if p.rect.data is not None:  # None when render() failed before its first copy
                self.passes[name] = p.rect.data
        if "Combined" in self.passes:
            self.pixels = self.passes["Combined"].reshape(h, w, 4)

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
        copy = rf.Job._copy_passes

        def copy_final(job, engine, result, w, h, final=False):
            if final:  # the session is still open: read what it rendered
                prog = job.abi.ar_progress()
                native.call(job.lib.ar_session_get_progress, job.session, ctypes.byref(prog))
                seen.update(active=prog.active_generation, state=prog.state, spp=prog.samples_per_pixel)
            copy(job, engine, result, w, h, final)

        with mock.patch.object(native, "_bin_dir", lambda: BIN), mock.patch.object(rf, "_log_path", lambda: log), \
                mock.patch.object(rf.Job, "_copy_passes", copy_final):
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

    def render_cards(self, transparent, **layer_flags):
        """cards_scene through Job.update and render with the fake Engine; Film Transparent as given."""
        import bpy

        rf, native = load_addon()
        scene = cards_scene()
        scene.render.film_transparent = transparent
        for flag, on in layer_flags.items():
            setattr(bpy.context.view_layer, flag, on)
        log = os.path.join(self._dir.name, "context.log")
        with mock.patch.object(native, "_bin_dir", lambda: BIN), mock.patch.object(rf, "_log_path", lambda: log):
            job, engine = rf.Job(), Engine()
            try:
                job.update(support.depsgraph())
                job.render(engine)
            finally:
                job.close()
                rf.shutdown()
        return engine

    def test_transparent_film_alpha(self):
        on, off = self.render_cards(True), self.render_cards(False)
        a = on.pixels[:, :, 3]
        self.assertEqual(sorted(on.passes), ["Combined"], "Alpha is Combined's channel, not a pass")
        self.assertFalse([t for _, t in on.reports if "Transparent" in t], on.reports)
        np.testing.assert_array_equal(a[LEFT], 1.0)
        np.testing.assert_array_equal(a[LEFT_UP], 1.0)
        np.testing.assert_array_equal(a[LOWER_RIGHT], 0.0)
        np.testing.assert_array_equal(on.pixels[:, :, :3][LOWER_RIGHT], 0.0)  # camera rays do not see the world
        # A camera ray passes the card with probability 0.5 per sample: the region mean is binomial.
        n = a[UPPER_RIGHT].size * CARD_SPP
        se = 0.5 / math.sqrt(n)
        self.assertLess(abs(float(a[UPPER_RIGHT].mean()) - 0.5), 3 * se, f"alpha mean {a[UPPER_RIGHT].mean()}, SE {se}")
        # Off: Combined's alpha is 1 everywhere, the world is visible, and a surface the world does not reach
        # directly is unchanged to the bit.
        np.testing.assert_array_equal(off.pixels[:, :, 3], 1.0)
        self.assertGreater(float(off.pixels[:, :, :3][LOWER_RIGHT].mean()), 0.4)
        np.testing.assert_array_equal(on.pixels[:, :, :3][LEFT], off.pixels[:, :, :3][LEFT])

    def test_depth_pass(self):
        engine = self.render_cards(False, use_pass_z=True)
        self.assertEqual(engine.added, [("Depth", 1, "Z")])
        d = engine.passes["Depth"]
        self.assertEqual(d.size, SIZE * SIZE)
        d = d.reshape(SIZE, SIZE)
        np.testing.assert_allclose(d[LEFT], 5.0, rtol=0, atol=1e-4)  # view-space z of the plane under a pinhole
        np.testing.assert_array_equal(d[LOWER_RIGHT], 1e10)          # a ray that misses reads 1e10
        np.testing.assert_array_equal(engine.pixels[:, :, 3], 1.0)


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
        with mock.patch.object(rf.Job, "_copy_passes", lambda *a, **k: None):
            with self.assertRaisesRegex(rf.RenderFailed, "converged on generation 0, but update\\(\\) committed 1"):
                job._wait(engine, None, SIZE, SIZE, SPP)


def lit_scene():
    """A diffuse plane under an area lamp at few samples: noisy enough that OIDN changes pixels, fast on the CPU."""
    import bpy

    scene = support.reset_scene()
    scene.render.resolution_x = scene.render.resolution_y = SIZE
    scene.render.resolution_percentage = 100
    me = bpy.data.meshes.new("Floor")
    me.from_pydata([(-3, -3, 0), (3, -3, 0), (3, 3, 0), (-3, 3, 0)], [], [(0, 1, 2, 3)])
    floor = bpy.data.objects.new("Floor", me)
    scene.collection.objects.link(floor)
    mat = bpy.data.materials.new("Mat")
    mat.use_nodes = True
    floor.data.materials.append(mat)
    lamp_data = bpy.data.lights.new("Lamp", "AREA")
    lamp_data.energy, lamp_data.size = 500.0, 1.0
    lamp = bpy.data.objects.new("Lamp", lamp_data)
    scene.collection.objects.link(lamp)
    lamp.location = (0.0, 0.0, 2.0)
    cd = bpy.data.cameras.new("Cam")
    cam = bpy.data.objects.new("Cam", cd)
    scene.collection.objects.link(cam)
    cam.location = (0.0, -4.0, 3.0)
    cam.rotation_euler = (math.radians(55), 0.0, 0.0)
    scene.camera = cam
    world = bpy.data.worlds.new("World")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.02, 0.02, 0.02, 1)
    scene.world = world
    bpy.context.view_layer.update()
    scene.astroray.device = "CPU"
    scene.astroray.samples = scene.astroray.viewport_samples = SPP * 2
    return scene


class TestF12Passes(unittest.TestCase):
    """What Job.render() puts in the RenderResult beside Combined (the Denoise and Denoising Data settings,
    COPIED_PASSES), against the built core with OIDN."""

    def setUp(self):
        support.acquire_properties()
        self.addCleanup(support.release_properties)
        self._dir = tempfile.TemporaryDirectory()
        self.addCleanup(self._dir.cleanup)
        self.rf, native = load_addon()
        log = os.path.join(self._dir.name, "context.log")
        for patch in (mock.patch.object(native, "_bin_dir", lambda: BIN),
                      mock.patch.object(self.rf, "_log_path", lambda: log)):
            patch.start()
            self.addCleanup(patch.stop)
        self.scene = lit_scene()

    def render(self, denoise, store=False, **layer_flags):
        """One F12 job on the open scene; returns the fake Engine and what the session offered at the end."""
        import bpy

        a = self.scene.astroray
        a.use_denoising, a.denoising_store_passes = denoise, store
        for flag, on in layer_flags.items():
            setattr(bpy.context.view_layer, flag, on)
        rf, seen = self.rf, {}
        copy = rf.Job._copy_passes

        def spy(job, engine, result, w, h, final=False):
            if final:
                seen["names"] = rf.offered_passes(job.lib, job.abi, job.session)
                if any(p.name == "Normal" for p in job.wanted):
                    seen["normal"] = rf.read_pass(job.lib, job.abi, job.session, "Normal", job.abi.AR_QUANTITY_MEAN)
            copy(job, engine, result, w, h, final)

        job, engine = rf.Job(), Engine()
        try:
            with mock.patch.object(rf.Job, "_copy_passes", spy):
                job.update(support.depsgraph())
                job.render(engine)
        finally:
            job.close()
            rf.shutdown()
        return engine, seen

    @staticmethod
    def rgb(engine, name):
        a = engine.passes[name]
        return a.reshape(SIZE, SIZE, a.size // (SIZE * SIZE))[:, :, :3]

    def test_requested_passes_are_copied(self):
        engine, seen = self.render(True, True, use_pass_normal=True, use_pass_position=True)
        n = SIZE * SIZE
        sizes = {"Combined": 4 * n, "Noisy Image": 3 * n, "Denoising Albedo": 3 * n, "Denoising Normal": 3 * n,
                 "Normal": 3 * n}
        self.assertEqual({k: v.size for k, v in engine.passes.items()}, sizes, engine.reports)
        self.assertIn("Position", seen["names"], "the session enumerates Position, which makes the next check bite")
        self.assertNotIn("Position", engine.passes)
        np.testing.assert_array_equal(self.rgb(engine, "Normal"), seen["normal"])
        diff = float(np.abs(self.rgb(engine, "Noisy Image") - self.rgb(engine, "Combined")).max())
        self.assertGreater(diff, 0.0, "Combined equals the noisy image: no denoise ran")

    def test_noisy_image_equals_undenoised_render(self):
        on, _ = self.render(True, True)
        off, _ = self.render(False)
        self.assertEqual(sorted(off.passes), ["Combined"], "no denoiser, no denoising passes")
        np.testing.assert_array_equal(self.rgb(on, "Noisy Image"), self.rgb(off, "Combined"))
        self.assertGreater(float(np.abs(self.rgb(on, "Combined") - self.rgb(on, "Noisy Image")).max()), 0.0)

    def test_failed_denoiser_shows_noisy_and_warns(self):
        os.environ["ASTRORAY_TEST_DENOISER_FAIL"] = "library"
        try:
            engine, _ = self.render(True, True)
        finally:
            del os.environ["ASTRORAY_TEST_DENOISER_FAIL"]
        lines = [(lv, t) for lv, t in engine.reports if t.startswith("DENOISER_UNAVAILABLE")]
        self.assertEqual(len(lines), 1, engine.reports)
        self.assertEqual(lines[0][0], ("WARNING",))
        np.testing.assert_array_equal(self.rgb(engine, "Combined"), self.rgb(engine, "Noisy Image"))

    def test_depth_is_registered_and_not_refused_by_the_engine(self):
        # The engine produces Depth (0008 d10), so use_pass_z raises no PASS_UNSUPPORTED report, and the addon
        # registers and copies it as Blender's Z pass beside Combined.
        engine, _ = self.render(False, use_pass_z=True)
        lines = [t for _, t in engine.reports if t.startswith("PASS_UNSUPPORTED")]
        self.assertEqual(lines, [], engine.reports)
        self.assertEqual(sorted(engine.passes), ["Combined", "Depth"])
        self.assertEqual(engine.added, [("Depth", 1, "Z")])


if __name__ == "__main__":
    support.main()
