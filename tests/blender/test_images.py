# SPDX-License-Identifier: Apache-2.0
"""Blender image sync: what the engine decodes against Blender's own pixels (0007 d21, V19; task 110).

Run through Invoke-ArBlender. Each case builds a 16 m plane seen 1:1 by a 16 x 16 pixel
camera, so one pixel samples one texel centre, and renders the image as Principled emission on a black
world with a 0.01 px pixel filter (the emission-plane method, 0005 Verification 6). The Combined pass is
then compared with Blender's `image.pixels` of the same file. `Rig` is also the harness of
test_node_values.py.
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
if "ar_addon" not in sys.modules:
    _pkg = types.ModuleType("ar_addon")
    _pkg.__path__ = [ADDON]
    sys.modules["ar_addon"] = _pkg
translate = importlib.import_module("ar_addon.materials.translate")
world_sync = importlib.import_module("ar_addon.sync.world")
from ar_addon.sync import camera, classify, edit as edit_mod, images, instances, keys, mesh, settings  # noqa: E402

ABI = _load("ar_abi", os.path.join(GEN, "_abi.py"))
OPS = _load("ar_ops", os.path.join(GEN, "_ops.py"))
PROPS = _load("ar_properties", os.path.join(ADDON, "properties.py"))
OP = OPS.OP_ID
N = 16  # pixels and texels per side


def unexpected(entries):
    """Report entries other than the pool-capacity notice every session raises."""
    return [e for e in entries if e[0] != ABI.AR_REPORT_POOL_CAPACITY]


def ok(st, what):
    if st != ABI.AR_OK:
        raise RuntimeError(f"{what} returned status {st}")


class Rig:
    """One CPU engine scene over the current Blender scene: sync() edits it, render() runs a session
    and returns (Combined as (N, N, 3) rows bottom-up, report entries)."""

    def __init__(self, scene, plane, material, world):
        self.scene, self.plane, self.material, self.world = scene, plane, material, world
        self.lib = ctypes.CDLL(DLL)
        ABI.check(self.lib)
        ABI.bind(self.lib)
        self.ctx, self.scene_h = ctypes.c_void_p(), ctypes.c_void_p()
        self._tmp = tempfile.TemporaryDirectory()
        self.keymap, self.state = keys.KeyMap(), images.ImageState()
        a = ABI
        ok(self.lib.ar_context_create(ctypes.byref(a.ar_context_desc(
            a.AR_DEVICE_CPU, 0, os.path.join(self._tmp.name, "context.log").encode(), None)),
            ctypes.byref(self.ctx)), "ar_context_create")
        ok(self.lib.ar_scene_create(self.ctx, ctypes.byref(a.ar_scene_desc(None, 2, 0)),
                                    ctypes.byref(self.scene_h)), "ar_scene_create")

    def sync(self, spp=2048, send_images=True):
        """A full sync in 0007 d20's order: settings, world, images, materials, geometry, camera.
        Returns the image report lines."""
        lib, a, scene = self.lib, ABI, self.scene
        dg = support.depsgraph()
        table = instances.build(dg, self.keymap)
        scene.astroray.samples = scene.astroray.viewport_samples = spp
        scene.astroray.pixel_filter_type, scene.astroray.pixel_filter_width = "BOX", 0.01
        fields, _ = settings.build(scene, bpy.data.colorspace, passes=())
        cam_fields, _ = camera.final(dg, scene)
        mat = self.material.evaluated_get(dg)
        used = images.used_images([mat.node_tree])
        lines = []
        with edit_mod.Edit(a, lib, self.scene_h) as e:
            settings.set_settings(e, a, fields)
            if send_images:
                lines = images.sync(e, a, self.keymap, used, self.state)
            self.graph = translate.translate_material(mat, self.keymap, OP)
            gk = int(table.arrays["geom_keys"][table.by_object[self.plane.original.session_uid][0]])
            mesh.sync_mesh(e, a, self.plane.data, gk, self.plane.name)
            translate.set_material(e, a, self.keymap, self.material.original.session_uid, self.graph)
            world_sync.sync(e, a, self.world.evaluated_get(dg), self.keymap, OP)
            instances.set_instances(e, a, table)
            camera.set_camera(e, a, cam_fields)
        self.cam_fields = cam_fields
        return lines

    def render(self):
        lib, a = self.lib, ABI
        session = ctypes.c_void_p()
        sd = a.ar_session_desc(a.AR_SESSION_MODE_FINAL, N, N, a.AR_DENOISER_NONE)
        ok(lib.ar_session_create(self.scene_h, ctypes.byref(sd), ctypes.byref(session)), "ar_session_create")
        try:
            view = camera.view_desc(a, self.cam_fields, N, N)
            ok(lib.ar_session_set_view(session, ctypes.byref(view)), "ar_session_set_view")
            ok(lib.ar_session_start(session), "ar_session_start")
            prog = a.ar_progress()
            for _ in range(600):
                ok(lib.ar_session_wait(session, 100), "ar_session_wait")
                ok(lib.ar_session_get_progress(session, ctypes.byref(prog)), "ar_session_get_progress")
                if prog.state in (a.AR_STATE_CONVERGED, a.AR_STATE_FAILED, a.AR_STATE_CLOSED):
                    break
            if prog.state != a.AR_STATE_CONVERGED:
                err = (ctypes.c_uint8 * 512)()
                lib.ar_session_last_error(session, 512, err)
                raise RuntimeError(f"session ended in state {prog.state}: "
                                   + bytes(err).split(bytes(1))[0].decode("utf-8", "replace"))
            return self._combined(session), self._entries(session)
        finally:
            lib.ar_session_destroy(session)

    def _combined(self, session):
        lib, a = self.lib, ABI
        n = ctypes.c_uint32()
        ok(lib.ar_session_pass_count(session, ctypes.byref(n)), "ar_session_pass_count")
        for i in range(n.value):
            info = a.ar_pass_info()
            ok(lib.ar_session_pass_info(session, i, ctypes.byref(info)), "ar_session_pass_info")
            if bytes(info.name).split(b"\0")[0] == b"Combined":
                buf = np.zeros(N * N * info.component_count, np.float32)
                ok(lib.ar_session_read_pass(session, i, a.AR_QUANTITY_MEAN, buf.size,
                                            buf.ctypes.data_as(ctypes.POINTER(ctypes.c_float))),
                   "ar_session_read_pass")
                return buf.reshape(N, N, info.component_count)[:, :, :3].copy()
        raise RuntimeError("the session offers no Combined pass")

    def _entries(self, session):
        lib, a = self.lib, ABI
        info = a.ar_report_info()
        ok(lib.ar_session_report_info(session, ctypes.byref(info)), "ar_session_report_info")
        out = []
        for i in range(info.entry_count):
            e = a.ar_report_entry()
            ok(lib.ar_session_report_entry(session, i, ctypes.byref(e)), "ar_session_report_entry")
            out.append((e.code, bytes(e.detail).split(b"\0")[0].decode("utf-8", "replace")))
        return out

    def close(self):
        for fn, h in ((self.lib.ar_scene_destroy, self.scene_h), (self.lib.ar_context_destroy, self.ctx)):
            if h.value:
                fn(h)
                h.value = None
        self._tmp.cleanup()


def new_scene():
    """A 16 m UV-mapped plane filling a 16 x 16 pixel view, a black world, and a material
    whose only light is its emission (set by the caller)."""
    scene = support.reset_scene()
    scene.render.resolution_x = scene.render.resolution_y = N
    scene.render.resolution_percentage = 100
    me = bpy.data.meshes.new("Plane")
    me.from_pydata([(-8, -8, 0), (8, -8, 0), (8, 8, 0), (-8, 8, 0)], [], [(0, 1, 2, 3)])
    uv = me.uv_layers.new(name="UVMap")
    for loop, co in zip(me.loops, ((0, 0), (1, 0), (1, 1), (0, 1))):
        uv.data[loop.index].uv = co
    plane = bpy.data.objects.new("Plane", me)
    scene.collection.objects.link(plane)
    cd = bpy.data.cameras.new("Cam")
    # The engine has no orthographic camera yet; a perspective one straight above a plane maps pixels
    # to plane points linearly, and this angle makes the plane's edges the frame's edges.
    cd.sensor_fit, cd.angle = "AUTO", 2 * math.atan(8.0 / 5.0)
    cam = bpy.data.objects.new("Cam", cd)
    scene.collection.objects.link(cam)
    cam.location = (0.0, 0.0, 5.0)
    scene.camera = cam
    world = bpy.data.worlds.new("World")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0, 0, 0, 1)
    scene.world = world
    mat = bpy.data.materials.new("Mat")
    mat.use_nodes = True
    plane.data.materials.append(mat)
    p = mat.node_tree.nodes["Principled BSDF"]
    p.inputs["Base Color"].default_value = (0, 0, 0, 1)
    p.inputs["Emission Strength"].default_value = 1.0
    bpy.context.view_layer.update()
    return scene, plane, mat, world


def emit_texture(mat, image, interpolation="Closest", extension="EXTEND"):
    t = mat.node_tree
    tex = t.nodes.new("ShaderNodeTexImage")
    tex.image, tex.interpolation, tex.extension = image, interpolation, extension
    t.links.new(tex.outputs["Color"], t.nodes["Principled BSDF"].inputs["Emission Color"])
    return tex


def pixels_of(image):
    """image.pixels as (rows bottom-up, width, channels) floats."""
    w, h = image.size
    p = np.zeros(w * h * image.channels, np.float32)
    image.pixels.foreach_get(p)
    return p.reshape(h, w, image.channels)


def srgb_decode(v):
    v = np.asarray(v, np.float64)
    return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4)


def srgb_encode(v):
    v = np.clip(np.asarray(v, np.float64), 0.0, 1.0)
    return np.where(v <= 0.0031308, v * 12.92, 1.055 * v ** (1 / 2.4) - 0.055)


def write_image(directory, name, fmt, seed):
    """A file-backed N x N RGB image of random bytes, loaded back from disk. Blender's pixels are the
    decoder's own output, so the expected values are never the bytes that were written."""
    rng = np.random.RandomState(seed)
    src = bpy.data.images.new(name + "_src", N, N, alpha=False, float_buffer=False)
    src.pixels[0]  # allocates the buffer: foreach_set on a fresh image writes nothing otherwise
    # Texels are a random grey plus a few codes of chroma per channel. The engine is spectral, so a
    # saturated texel adds wavelength noise of about 1e-3 at this sample count while a nearly grey
    # one adds under 1e-4: the byte comparison below needs the second.
    base = rng.randint(0, 256, (N * N, 1))
    codes = np.clip(base + rng.randint(-8, 9, (N * N, 3)), 0, 255)
    rgba = np.ones((N * N, 4), np.float32)
    rgba[:, :3] = codes / 255.0
    src.pixels.foreach_set(rgba.reshape(-1))
    ext = {"PNG": "png", "JPEG": "jpg", "TARGA": "tga"}[fmt]
    src.file_format = fmt
    src.filepath_raw = os.path.join(directory, f"{name}.{ext}")
    src.save()
    return bpy.data.images.load(src.filepath_raw)


class ImageCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        PROPS.register()

    @classmethod
    def tearDownClass(cls):
        PROPS.unregister()

    def setUp(self):
        self._dir = tempfile.TemporaryDirectory()
        self.addCleanup(self._dir.cleanup)
        self.rig = None

    def tearDown(self):
        if self.rig is not None:
            self.rig.close()

    def start(self):
        scene, plane, mat, world = new_scene()
        self.mat = mat
        self.rig = Rig(scene, plane, mat, world)
        return self.rig


class TestImages(ImageCase):
    def check_decode(self, fmt, colourspace, seed):
        rig = self.start()
        image = write_image(self._dir.name, f"{fmt}_{colourspace}", fmt, seed)
        image.colorspace_settings.name = colourspace
        emit_texture(self.mat, image)
        lines = rig.sync()
        got, entries = rig.render()
        want = pixels_of(image)[:, :, :3]
        if colourspace == "sRGB":
            want = srgb_decode(want)
        err = np.abs(got - want)
        print(f"RESULT image {fmt} {colourspace} max abs error {err.max():.3e} mean {err.mean():.3e}"
              f" lines {lines} entries {entries}")
        self.assertEqual((lines, unexpected(entries)), ([], []))
        back = srgb_encode(got) if colourspace == "sRGB" else got
        codes = np.rint(back * 255) - np.rint(pixels_of(image)[:, :, :3] * 255)
        if fmt == "JPEG":
            # JPEG decoders are not bit-exact with each other: stb_image (the engine's) and Blender's
            # libjpeg round the inverse DCT and the colour conversion differently: measured up to two
            # codes on a few texels (about 4% of them differ at all). That is the most a JPEG can ask.
            print(f"RESULT image JPEG {colourspace}: {int((codes != 0).any(axis=2).sum())} of {N * N} texels "
                  f"differ, max {int(np.abs(codes).max())} codes")
            self.assertLessEqual(np.abs(codes).max(), 2)
            self.assertLessEqual((codes != 0).any(axis=2).mean(), 0.10)
            return
        # The render adds the spectral round trip, so values agree to well under half a code value
        # of the 8-bit image; and every texel must still map back to its own byte.
        self.assertLess(err.max(), 5e-4)
        self.assertTrue(np.array_equal(codes, np.zeros_like(codes)))

    def test_png_srgb_exact(self):
        self.check_decode("PNG", "sRGB", 1)

    def test_png_non_color_exact(self):
        self.check_decode("PNG", "Non-Color", 2)

    def test_jpeg_srgb_exact(self):
        self.check_decode("JPEG", "sRGB", 3)

    def test_jpeg_non_color_exact(self):
        self.check_decode("JPEG", "Non-Color", 4)

    def test_tga_srgb_exact(self):
        self.check_decode("TARGA", "sRGB", 5)

    def test_tga_non_color_exact(self):
        self.check_decode("TARGA", "Non-Color", 6)

    def test_packed_image_goes_as_memory_and_matches_the_file(self):
        rig = self.start()
        image = write_image(self._dir.name, "packed", "PNG", 7)
        image.pack()
        self.assertEqual(images.source_of(image), "MEMORY")
        want = srgb_decode(pixels_of(image)[:, :, :3])
        os.remove(image.filepath_from_user())  # only the packed bytes remain
        emit_texture(self.mat, image)
        self.assertEqual(rig.sync(), [])
        got, entries = rig.render()
        print(f"RESULT packed max abs error {np.abs(got - want).max():.3e} entries {entries}")
        self.assertEqual(unexpected(entries), [])
        self.assertLess(np.abs(got - want).max(), 5e-4)

    def test_unsupported_interop_id_renders_magenta_with_one_entry(self):
        """ACES2065-1 (lin_ap0_scene) is a matrix plus transfer the engine does not decode in M1a."""
        rig = self.start()
        image = write_image(self._dir.name, "aces", "PNG", 8)
        image.colorspace_settings.name = "ACES2065-1"
        self.assertEqual(image.colorspace_settings.interop_id, "lin_ap0_scene")
        emit_texture(self.mat, image)
        rig.sync()
        got, entries = rig.render()
        mine = unexpected(entries)
        print(f"RESULT unsupported interop mean rgb {got.reshape(-1, 3).mean(axis=0)} entries {entries}")
        self.assertEqual(len(mine), 1, entries)
        self.assertEqual(mine[0][0], ABI.AR_REPORT_COLORSPACE_UNSUPPORTED)
        self.assertEqual(mine[0][1], "lin_ap0_scene")
        mean = got.reshape(-1, 3).mean(axis=0)
        self.assertLess(mean[1], 0.25 * min(mean[0], mean[2]))
        self.assertGreater(mean[0], 0.1)

    def test_generated_image_reports_m1b(self):
        rig = self.start()
        image = bpy.data.images.new("gen", N, N)
        self.assertEqual(images.source_of(image), "PIXELS")
        emit_texture(self.mat, image)
        lines = rig.sync()
        self.assertEqual(lines, ["image 'gen': generated images land in M1b (it renders magenta)"])
        got, entries = rig.render()  # the edit survived the engine's refusal
        mean = got.reshape(-1, 3).mean(axis=0)
        print(f"RESULT generated mean rgb {mean}")
        self.assertLess(mean[1], 0.25 * min(mean[0], mean[2]))

    def test_dirty_file_image_goes_as_pixels(self):
        self.start()
        image = write_image(self._dir.name, "dirty", "PNG", 9)
        self.assertEqual(images.source_of(image), "FILE")
        image.pixels[0] = 0.5
        self.assertTrue(image.is_dirty)
        self.assertEqual(images.source_of(image), "PIXELS")

    def test_image_key_is_the_translators(self):
        rig = self.start()
        image = write_image(self._dir.name, "key", "PNG", 10)
        emit_texture(self.mat, image)
        rig.sync()
        names = {v: k for k, v in OP.items()}
        tex = [n for n in rig.graph.nodes if names[n[0]] == "TEX_IMAGE"]
        self.assertEqual(len(tex), 1)
        self.assertEqual(rig.graph.i32[tex[0][3]], images.image_key(rig.keymap, image))

    def test_reload_with_unchanged_mtime_decodes_again(self):
        """Two TGAs of equal size and one mtime: only the reload counter tells the engine's cache
        that the file changed (0007 d21)."""
        rig = self.start()
        image = write_image(self._dir.name, "reload", "TARGA", 11)
        path = image.filepath_from_user()
        stat = os.stat(path)
        emit_texture(self.mat, image, "Closest")
        rig.sync()
        first, _ = rig.render()
        other = write_image(self._dir.name, "other", "TARGA", 12)
        with open(other.filepath_from_user(), "rb") as f:
            data = f.read()
        self.assertEqual(len(data), stat.st_size)
        with open(path, "wb") as f:
            f.write(data)
        os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns))
        rig.sync()
        stale, _ = rig.render()
        self.assertTrue(np.array_equal(stale, first), "the unchanged key must hit the engine's cache")
        rig.state.reloads[image.session_uid] = 1
        rig.sync()
        fresh, _ = rig.render()
        image.reload()
        want = srgb_decode(pixels_of(image)[:, :, :3])
        print(f"RESULT reload stale==first {np.array_equal(stale, first)} fresh max abs error "
              f"{np.abs(fresh - want).max():.3e}")
        self.assertLess(np.abs(fresh - want).max(), 5e-4)
        self.assertGreater(np.abs(fresh - first).max(), 0.05)

    def test_reload_counter_follows_image_updates(self):
        state = images.ImageState()
        Act = classify.Action
        state.note_updates([Act(classify.IMAGES, "IMAGE", 7, 1), Act(classify.MATERIALS, "MATERIAL", 7, 1)],
                           classify.IMAGES)
        state.note_updates([Act(classify.IMAGES, "IMAGE", 7, 1)], classify.IMAGES)
        self.assertEqual(state.reloads, {7: 2})


if __name__ == "__main__":
    support.main()
