# SPDX-License-Identifier: Apache-2.0
"""Engine camera rays against Blender's projection (0007 V18, perspective cases; task 082).

Run through Invoke-ArBlender on a gate build (ASTRORAY_TEST_PIXEL_CENTRE needs AR_GATE_HOOKS).
A large plane faces the camera; the CPU engine renders the Position pass at 64x48 with zero
filter offset, so each pixel's primary hit is the point under that pixel's centre. Mapping that
point back through Blender's own projection must give the pixel centre's NDC within 1e-5.
Position is requested through ar_settings.passes (117) and read with ar_session_read_pass.
"""
import ctypes
import importlib.util
import math
import os
import sys
import tempfile
import unittest
from types import SimpleNamespace

import numpy as np

import bpy
from bpy_extras.object_utils import world_to_camera_view
from mathutils import Matrix, Vector

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(HERE, "headless"))
import support  # noqa: E402

support.load_sync()
from ar_sync import camera, edit as edit_mod, instances, keys, lights, mesh, settings  # noqa: E402

GEN = os.path.join(ROOT, "out", "build", "gen")
DLL = os.path.join(ROOT, "out", "bin", "astroray_core.dll")
# Set before the engine DLL loads: its static CRT copies the environment when it is loaded, so a
# later assignment is not seen by read_test_hooks.
os.environ["ASTRORAY_TEST_PIXEL_CENTRE"] = "1"
TOLERANCE = 1e-5  # NDC, 0007 V18


def _load(name, path):
    if not os.path.exists(path):
        raise RuntimeError(f"{path} is missing: run tools/build.ps1 first")
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


PROPS = _load("ar_properties", os.path.join(ROOT, "blender", "astroray", "properties.py"))
ABI = _load("ar_abi", os.path.join(GEN, "_abi.py"))


def ok(st, what):
    if st != ABI.AR_OK:
        raise RuntimeError(f"{what} returned status {st}")


class Rendered:
    """One CPU session over the current Blender scene, converged at one sample per pixel."""

    def __init__(self, scene, cam_fields, width, height, origin=(0.0, 0.0, 0.0)):
        self.lib = ctypes.CDLL(DLL)
        ABI.check(self.lib)
        ABI.bind(self.lib)
        self.ctx, self.scene_h, self.session = ctypes.c_void_p(), ctypes.c_void_p(), ctypes.c_void_p()
        self._tmp = tempfile.TemporaryDirectory()
        try:
            self._build(scene, cam_fields, width, height, origin)
        except BaseException:
            self.close()
            raise

    def _build(self, scene, cam_fields, w, h, origin):
        lib, a = self.lib, ABI
        ok(lib.ar_context_create(ctypes.byref(a.ar_context_desc(a.AR_DEVICE_CPU, 0, os.path.join(self._tmp.name, 'context.log').encode(), None)),
                                 ctypes.byref(self.ctx)), "ar_context_create")
        ok(lib.ar_scene_create(self.ctx, ctypes.byref(a.ar_scene_desc(None, 2, 0)),
                               ctypes.byref(self.scene_h)), "ar_scene_create")
        dg = support.depsgraph()
        km = keys.KeyMap()
        table = instances.build(dg, km, origin)
        scene.astroray.samples = scene.astroray.viewport_samples = 1
        scene.astroray.pixel_filter_type = "BOX"
        fields, _ = settings.build(scene, bpy.data.colorspace, passes=("Position",))
        with edit_mod.Edit(a, lib, self.scene_h) as e:
            settings.set_settings(e, a, fields)
            mesh.sync_mesh(e, a, scene.objects["Plane"].data, int(table.arrays["geom_keys"][0]), "Plane")
            instances.set_instances(e, a, table)
            camera.set_camera(e, a, cam_fields)
        sd = a.ar_session_desc(a.AR_SESSION_MODE_FINAL, w, h, a.AR_DENOISER_NONE)
        ok(lib.ar_session_create(self.scene_h, ctypes.byref(sd), ctypes.byref(self.session)), "ar_session_create")
        view = camera.view_desc(a, cam_fields, w, h)
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
        self.size = (w, h)

    def position(self):
        """Position pass as (h, w, 3), row 0 at the bottom (Blender's buffer order)."""
        lib, a = self.lib, ABI
        n = ctypes.c_uint32()
        ok(lib.ar_session_pass_count(self.session, ctypes.byref(n)), "ar_session_pass_count")
        for i in range(n.value):
            info = a.ar_pass_info()
            ok(lib.ar_session_pass_info(self.session, i, ctypes.byref(info)), "ar_session_pass_info")
            if bytes(info.name).split(b"\0")[0] == b"Position":
                w, h = self.size
                buf = np.zeros(w * h * 3, np.float32)
                ok(lib.ar_session_read_pass(self.session, i, a.AR_QUANTITY_MEAN, buf.size,
                                            buf.ctypes.data_as(ctypes.POINTER(ctypes.c_float))),
                   "ar_session_read_pass")
                return buf.reshape(h, w, 3)
        raise RuntimeError("the session offers no Position pass; ar_settings.passes did not reach the film (117)")

    def close(self):
        for fn, h in ((self.lib.ar_session_destroy, self.session), (self.lib.ar_scene_destroy, self.scene_h),
                      (self.lib.ar_context_destroy, self.ctx)):
            if h.value:
                fn(h)
                h.value = None
        self._tmp.cleanup()


def make_scene(width, height, aspect=(1.0, 1.0), **cam_props):
    """A 16 m plane at z = 0 and a tilted camera 5 m above it that sees only the plane. The plane is
    large enough to fill every frustum here and no larger: float32 intersection error grows with the
    triangle's extent, and the 1e-5 NDC tolerance is a camera tolerance, not a 100 m one."""
    scene = support.reset_scene()
    scene.render.resolution_x, scene.render.resolution_y, scene.render.resolution_percentage = width, height, 100
    scene.render.pixel_aspect_x, scene.render.pixel_aspect_y = aspect
    me = bpy.data.meshes.new("Plane")
    me.from_pydata([(-8, -8, 0), (8, -8, 0), (8, 8, 0), (-8, 8, 0)], [], [(0, 1, 2, 3)])
    ob = bpy.data.objects.new("Plane", me)
    scene.collection.objects.link(ob)
    cam = bpy.data.cameras.new("Cam")
    for k, v in cam_props.items():
        setattr(cam, k, v)
    co = bpy.data.objects.new("Cam", cam)
    scene.collection.objects.link(co)
    co.location = (0.3, -0.2, 5.0)
    co.rotation_euler = (0.1, 0.05, 0.3)  # looks almost straight down, never at the horizon
    scene.camera = co
    bpy.context.view_layer.update()
    return scene, co


def pixel_centres(w, h):
    """NDC of every pixel centre, row 0 at the bottom: arrays (h, w) of x and y."""
    x = (np.arange(w) + 0.5) / w * 2 - 1
    y = (np.arange(h) + 0.5) / h * 2 - 1
    return np.broadcast_to(x, (h, w)), np.broadcast_to(y[:, None], (h, w))


class TestCameraNdc(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        PROPS.register()

    @classmethod
    def tearDownClass(cls):
        PROPS.unregister()

    def check(self, w, h, project=None, make=None, aspect=(1.0, 1.0), origin=(0.0, 0.0, 0.0), **cam_props):
        scene, co = make_scene(w, h, aspect, **cam_props)
        if make is None:
            fields, _ = camera.final(support.depsgraph(), scene, origin)
        else:
            fields = make(scene, co)
        r = Rendered(scene, fields, w, h, origin)
        try:
            pos = r.position()
        finally:
            r.close()
        self.assertTrue(np.isfinite(pos).all(), "a pixel missed the plane")
        pos = pos + np.asarray(origin, np.float32)  # the engine works relative to the scene origin
        if project is None:
            def project(p):
                v = world_to_camera_view(scene, co, Vector(p))
                return 2 * v.x - 1, 2 * v.y - 1
        got = np.array([project(p) for p in pos.reshape(-1, 3)]).reshape(h, w, 2)
        nx, ny = pixel_centres(w, h)
        diff = max(np.abs(got[..., 0] - nx).max(), np.abs(got[..., 1] - ny).max())
        print(f"NDC max difference {diff:.3e} ({w}x{h}, aspect {aspect}, origin {origin}, {cam_props})")
        self.assertLessEqual(diff, TOLERANCE)

    def test_persp(self):
        self.check(64, 48)

    def test_shift_x(self):
        self.check(64, 48, shift_x=0.2, shift_y=-0.1)

    def test_sensor_fit_horizontal(self):
        self.check(64, 48, sensor_fit="HORIZONTAL", sensor_width=24.0, lens=35.0)

    def test_sensor_fit_vertical(self):
        self.check(64, 48, sensor_fit="VERTICAL", sensor_height=20.0, lens=35.0)

    def test_pixel_aspect(self):
        self.check(64, 48, aspect=(2.0, 1.0))
        self.check(64, 48, aspect=(1.0, 2.0))

    def test_scene_origin_is_folded_into_the_camera(self):
        origin = (3.0, -2.0, 1.0)
        self.check(64, 48, origin=origin, shift_x=0.1)
        scene, co = make_scene(64, 48)
        f, _ = camera.final(support.depsgraph(), scene, origin)
        np.testing.assert_allclose([f["cam_to_world"][i] for i in (3, 7, 11)],
                                   np.asarray(co.location) - origin, atol=1e-6)

    def test_portrait(self):
        self.check(48, 64)

    def test_viewport_camera_view(self):
        """A region in camera view with a zoom: Blender's window and view matrices are the
        projection, so the expectation multiplies them (the camera frame no longer fills the NDC)."""
        w, h = 64, 48

        def make(scene, co):
            self.window = np.asarray(co.calc_matrix_camera(support.depsgraph(), x=w, y=h), np.float64)
            self.window[:2, :] *= 1.5  # camera zoom: a narrower field
            self.view = np.asarray(co.matrix_world.inverted(), np.float64)
            rv3d = SimpleNamespace(view_matrix=Matrix(self.view), window_matrix=Matrix(self.window),
                                   view_perspective="CAMERA", is_perspective=True)
            space = SimpleNamespace(camera=None, use_local_camera=False, clip_start=0.1, clip_end=1000.0)
            fields, _ = camera.viewport(scene, space, rv3d, support.depsgraph())
            return fields

        def project(p):
            q = self.window @ self.view @ np.array([*p, 1.0])
            return q[0] / q[3], q[1] / q[3]

        self.check(w, h, project=project, make=make)

    def test_lamps_and_settings_are_accepted_by_the_engine(self):
        """The real generated structs: an edit with settings, camera and every lamp kind commits."""
        scene, co = make_scene(64, 48)
        for kind, loc in (("POINT", (0, 0, 8)), ("AREA", (1, 0, 8)), ("SPOT", (2, 0, 8))):
            light = bpy.data.lights.new(kind, kind)
            if kind == "POINT":
                light.use_soft_falloff, light.shadow_soft_size = True, 0.25  # reported as approximated
            ob = bpy.data.objects.new(kind, light)
            scene.collection.objects.link(ob)
            ob.location = loc
        bpy.context.view_layer.update()
        lib = ctypes.CDLL(DLL)
        ABI.check(lib)
        ABI.bind(lib)
        ctx, scene_h = ctypes.c_void_p(), ctypes.c_void_p()
        with tempfile.TemporaryDirectory() as tmp:
            desc = ABI.ar_context_desc(ABI.AR_DEVICE_CPU, 0, os.path.join(tmp, "c.log").encode(), None)
            ok(lib.ar_context_create(ctypes.byref(desc), ctypes.byref(ctx)), "ar_context_create")
            ok(lib.ar_scene_create(ctx, ctypes.byref(ABI.ar_scene_desc(None, 2, 0)), ctypes.byref(scene_h)),
               "ar_scene_create")
            try:
                dg = support.depsgraph()
                fields, _ = settings.build(scene, bpy.data.colorspace, passes=())
                cam, _ = camera.final(dg, scene)
                with edit_mod.Edit(ABI, lib, scene_h) as e:
                    settings.set_settings(e, ABI, fields)
                    camera.set_camera(e, ABI, cam)
                    lines = lights.sync(e, ABI, dg, keys.KeyMap())
                self.assertEqual(len(lines), 2, lines)  # the SPOT skip and the POINT's Soft Falloff
                self.assertTrue(any("lamp type SPOT lands in M4" in x for x in lines), lines)
            finally:
                lib.ar_scene_destroy(scene_h)
                lib.ar_context_destroy(ctx)

    def test_view_hash_changes_only_with_the_view(self):
        scene, co = make_scene(64, 48)
        view = np.eye(4)
        win = np.asarray(co.calc_matrix_camera(support.depsgraph(), x=64, y=48), np.float64)
        rv = lambda v, wm: SimpleNamespace(view_matrix=v, window_matrix=wm)  # noqa: E731
        h0 = camera.view_hash(rv(view, win), 64, 48, True)
        self.assertEqual(h0, camera.view_hash(rv(view.copy(), win.copy()), 64, 48, True))
        moved = view.copy()
        moved[0, 3] = 1.0
        self.assertNotEqual(h0, camera.view_hash(rv(moved, win), 64, 48, True))
        zoomed = win.copy()
        zoomed[:2, :] *= 1.5  # zoom or lens change
        self.assertNotEqual(h0, camera.view_hash(rv(view, zoomed), 64, 48, True))
        self.assertNotEqual(h0, camera.view_hash(rv(view, win), 65, 48, True))
        self.assertNotEqual(h0, camera.view_hash(rv(view, win), 64, 48, False))
        self.assertNotEqual(h0, camera.view_hash(rv(view, win), 64, 48, True, (0, 0, 0.5, 0.5)))
        calls = []
        vs = camera.ViewSync()
        fields, _ = camera.final(support.depsgraph(), scene)
        send = lambda s, v: calls.append(v) or 0  # noqa: E731
        self.assertTrue(vs.update(send, ABI, None, fields, h0, 64, 48))
        self.assertFalse(vs.update(send, ABI, None, fields, h0, 64, 48))
        self.assertTrue(vs.update(send, ABI, None, fields, h0 + 1, 64, 48))
        self.assertEqual(len(calls), 2)


if __name__ == "__main__":
    support.main()
