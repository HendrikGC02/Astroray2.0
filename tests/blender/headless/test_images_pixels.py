# SPDX-License-Identifier: Apache-2.0
"""Generated, edited, float and packed-EXR images reach the engine intact (0007 d21; task 167).

The recorder captures the staged payload of ar_edit_image_begin at ar_edit_item_commit, so equality of
that payload with `Image.pixels` is checked on the bytes the engine received. Renders use the
emission-plane method of tests/blender/test_images.py (its Rig: a 16 x 16 pixel plane, a black world,
a box pixel filter of 0.01 px, CPU engine). A 4 x 4 image on that plane covers 4 x 4 pixels a texel.
Run through Invoke-ArBlender or with the bpy wheel.
Ref: docs/decisions/0007-blender-integration.md d13, d21; 0005 items 10, 34.
"""
import ctypes
import importlib.util
import os
import sys
import tempfile
import unittest

import numpy as np

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import support  # noqa: E402
import test_images as ti  # noqa: E402  (the Rig and the plane harness)

ABI, images = ti.ABI, ti.images
N = ti.N
T = 4  # texels per side of the test images
LIMIT = np.frombuffer(np.uint32(0x7EFFFFF7).tobytes(), np.float32)[0]  # src/host/image/decode.h kMaxTexelMagnitude
EVIDENCE = os.path.join(support.ROOT, "out", "evidence", "167")


def load_arrec():
    path = os.path.join(ti.GEN, "arrec.py")
    spec = importlib.util.spec_from_file_location("ar_arrec_pixels", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class RecordingRig(ti.Rig):
    """A Rig whose context records its ABI calls to `record_path`."""

    def __init__(self, scene, plane, material, world, record_path):
        self.scene, self.plane, self.material, self.world = scene, plane, material, world
        self.extra_materials = ()
        self.lib = ctypes.CDLL(ti.DLL)
        ABI.check(self.lib)
        ABI.bind(self.lib)
        self.ctx, self.scene_h = ctypes.c_void_p(), ctypes.c_void_p()
        self._tmp = tempfile.TemporaryDirectory()
        self.keymap, self.state = ti.keys.KeyMap(), images.ImageState()
        a = ABI
        ti.ok(self.lib.ar_context_create(ctypes.byref(a.ar_context_desc(
            a.AR_DEVICE_CPU, 0, os.path.join(self._tmp.name, "context.log").encode(), record_path.encode())),
            ctypes.byref(self.ctx)), "ar_context_create")
        ti.ok(self.lib.ar_scene_create(self.ctx, ctypes.byref(a.ar_scene_desc(None, 2, 0)),
                                       ctypes.byref(self.scene_h)), "ar_scene_create")

    def entries_with_subject(self, session):
        info = ABI.ar_report_info()
        ti.ok(self.lib.ar_session_report_info(session, ctypes.byref(info)), "ar_session_report_info")
        out = []
        for i in range(info.entry_count):
            e = ABI.ar_report_entry()
            ti.ok(self.lib.ar_session_report_entry(session, i, ctypes.byref(e)), "ar_session_report_entry")
            out.append((e.code, e.subject, bytes(e.detail).split(b"\0")[0].decode("utf-8", "replace")))
        return out


def float_image(name, rgba):
    """A generated float image from rgba (h, w, 4) float32, rows bottom-up."""
    h, w = rgba.shape[:2]
    im = bpy.data.images.new(name, w, h, alpha=True, float_buffer=True)
    im.pixels[0]  # allocates the buffer: foreach_set on a fresh image writes nothing otherwise
    im.pixels.foreach_set(np.ascontiguousarray(rgba, np.float32).reshape(-1))
    return im


def pattern(spike=True):
    """A gradient with values above 1 (one grey texel at 50.0, so the spectral round trip of the render
    stays as accurate there as elsewhere): (T, T, 4) rows bottom-up."""
    y, x = np.mgrid[0:T, 0:T]
    px = np.zeros((T, T, 4), np.float32)
    px[..., 0] = 0.1 + 0.3 * x
    px[..., 1] = 0.1 + 0.3 * y
    px[..., 2] = 0.25 + 0.05 * (x + y)
    px[..., 3] = 1.0
    if spike:
        px[2, 1, :3] = 50.0
    return px


def pixels_bytes(image):
    p = np.empty(image.size[0] * image.size[1] * 4, np.float32)
    image.pixels.foreach_get(p)
    return p.tobytes()


def save_exr(image, path, depth="32"):
    scene = bpy.context.scene
    st = scene.render.image_settings
    st.file_format, st.color_mode, st.color_depth, st.exr_codec = "OPEN_EXR", "RGBA", depth, "ZIP"
    image.save_render(path, scene=scene)


def block_expand(px):
    """(T, T, c) texels to the (N, N, c) pixels of the plane at Closest."""
    return np.kron(px, np.ones((N // T, N // T, 1), px.dtype))


class TestPixels(ti.ImageCase):
    def staged_pixels(self, record_path, expect=1):
        """The `pixels` payloads (bytes) the recording took at ar_edit_item_commit, in order."""
        _, calls = load_arrec().decode(record_path)
        out = []
        for name, args in calls:
            if name == "ar_edit_item_commit":
                out += [data for field, dtype, count, data in args["staged"] if field == "pixels"]
        self.assertEqual(len(out), expect)
        return out

    def record(self, setup):
        """Runs setup(rig) in a recording rig, closes it and returns the recording's path."""
        path = os.path.join(self._dir.name, "pixels.arrec")
        scene, plane, mat, world = ti.new_scene()
        self.mat = mat
        self.rig = RecordingRig(scene, plane, mat, world, path)
        setup(self.rig)
        self.rig.close()
        self.rig = None
        return path

    def test_generated_float_equals_pixels(self):
        """Values above 1 and below 0 reach the engine bit for bit."""
        px = pattern()
        px[0, 0, :3] = (-0.5, -3.0, 1.0e-30)
        px[3, 3, :3] = (1.0e4, 0.0, 123456.789)
        made = {}

        def setup(rig):
            made["image"] = image = float_image("gen_float", px)
            self.assertTrue(image.is_float)
            self.assertEqual(images.source_of(image), "PIXELS")
            ti.emit_texture(self.mat, image)
            self.assertEqual(rig.sync(), [])
            made["want"] = pixels_bytes(image)

        (payload,) = self.staged_pixels(self.record(setup))
        want = made["want"]
        self.assertEqual(len(payload), T * T * 4 * 4)
        self.assertEqual(payload, want)
        got = np.frombuffer(payload, np.float32).reshape(T, T, 4)
        self.assertEqual(got[2, 1, 0], np.float32(50.0))
        self.assertEqual(got[0, 0, 0], np.float32(-0.5))

    def test_dirty_byte_equals_pixels(self):
        """An edited 8-bit file image goes as PIXELS: its encoded values reach the engine bit for bit
        (the engine applies the sRGB transfer; the C++ ingest case checks that part)."""
        made = {}

        def setup(rig):
            image = ti.write_image(self._dir.name, "dirty_byte", "PNG", 31)
            image.pixels[0] = 0.5
            image.pixels[5] = 0.123
            self.assertTrue(image.is_dirty)
            self.assertFalse(image.is_float)
            self.assertEqual(images.source_of(image), "PIXELS")
            ti.emit_texture(self.mat, image)
            self.assertEqual(rig.sync(), [])
            made["want"] = pixels_bytes(image)

        (payload,) = self.staged_pixels(self.record(setup))
        self.assertEqual(payload, made["want"])
        p = np.frombuffer(payload, np.float32)
        self.assertLessEqual(abs(float(p[0]) - 0.5), 1 / 255)  # the byte image holds the nearest code
        self.assertLessEqual(abs(float(p[5]) - 0.123), 1 / 255)

    def test_generated_float_renders_the_pattern(self):
        """The 4 x 4 pattern, one texel at 50.0, on an emission plane at Closest: every pixel is its texel
        (a magenta stand-in would miss by far more than the render's spectral round trip)."""
        rig = self.start()
        px = pattern()
        image = float_image("gen_pattern", px)
        ti.emit_texture(self.mat, image)
        self.assertEqual(rig.sync(), [])
        got, entries = rig.render()
        self.assertEqual(ti.unexpected(entries), [])
        want = block_expand(px[..., :3])
        rel = np.abs(got - want) / np.maximum(want, 1e-3)
        worst = np.unravel_index(rel.argmax(), rel.shape)
        print(f"RESULT generated float pattern max relative error {rel.max():.3e} at {worst}: got {got[worst]} want {want[worst]}")
        self.assertLess(rel.max(), 5e-3)
        self.assertGreater(got[2 * 4 + 1, 1 * 4 + 1].min(), 40.0)  # the 50.0 texel

    def test_packed_exr_equals_pixels(self):
        """The same pixels saved as EXR and loaded packed (MEMORY) render as the generated image does;
        the generated and packed planes go to out/evidence/167/generated_pixels.png."""
        rig = self.start()
        px = pattern()
        gen = float_image("gen_for_exr", px)
        path = os.path.join(self._dir.name, "packed.exr")
        save_exr(gen, path)
        packed = bpy.data.images.load(path)
        packed.pack()
        os.remove(path)  # only the packed bytes remain
        self.assertEqual(images.source_of(packed), "MEMORY")
        self.assertEqual(images.source_of(gen), "PIXELS")
        want_exr = np.frombuffer(pixels_bytes(packed), np.float32).reshape(T, T, 4)
        self.assertTrue(np.array_equal(want_exr, px))  # a float EXR round trip is exact

        tex = ti.emit_texture(self.mat, gen)
        self.assertEqual(rig.sync(), [])
        from_pixels, entries = rig.render()
        self.assertEqual(ti.unexpected(entries), [])
        tex.image = packed
        self.assertEqual(rig.sync(), [])
        from_exr, entries = rig.render()
        self.assertEqual(ti.unexpected(entries), [])
        want = block_expand(want_exr[..., :3])
        rel = np.abs(from_exr - want) / np.maximum(want, 1e-3)
        both = np.abs(from_exr - from_pixels) / np.maximum(want, 1e-3)
        print(f"RESULT packed EXR max relative error {rel.max():.3e}; against the generated plane {both.max():.3e}")
        self.assertLess(rel.max(), 5e-3)
        self.assertLess(both.max(), 5e-3)
        self.write_evidence(from_pixels, from_exr)

    def write_evidence(self, left, right):
        """Generated plane | packed EXR plane, display-encoded and clipped at 1 so the 50.0 texel is white."""
        os.makedirs(EVIDENCE, exist_ok=True)
        both = np.concatenate([left, np.ones((N, 2, 3), np.float32), right], axis=1)
        enc = ti.srgb_encode(np.clip(both, 0.0, 1.0)).astype(np.float32)
        enc = np.kron(enc, np.ones((8, 8, 1), np.float32))
        h, w = enc.shape[:2]
        rgba = np.ones((h, w, 4), np.float32)
        rgba[..., :3] = enc
        out = bpy.data.images.new("evidence", w, h, alpha=False)
        out.pixels[0]
        out.pixels.foreach_set(rgba.reshape(-1))
        out.filepath_raw = os.path.join(EVIDENCE, "generated_pixels.png")
        out.file_format = "PNG"
        out.save()

    def render_refusal(self, texel_value):
        """A clean pattern image and one with a bad red texel in the same material; returns
        (clean key, bad key, report entries with their subjects, the picture)."""
        rig_scene = ti.new_scene()
        scene, plane, mat, world = rig_scene
        self.mat = mat
        rig = RecordingRig(scene, plane, mat, world, os.path.join(self._dir.name, "refusal.arrec"))
        self.rig = rig
        clean = float_image("clean", pattern(spike=False))
        bad_px = pattern(spike=False)
        bad_px[1, 2, 0] = texel_value
        bad = float_image("bad", bad_px)
        t = mat.node_tree
        ti.emit_texture(mat, clean)  # drives the emission, so the plane shows the clean image
        extra = t.nodes.new("ShaderNodeTexImage")
        extra.image = bad
        t.links.new(extra.outputs["Color"], t.nodes["Principled BSDF"].inputs["Base Color"])
        self.assertEqual(rig.sync(), [])
        session = ctypes.c_void_p()
        a, lib = ABI, rig.lib
        sd = a.ar_session_desc(a.AR_SESSION_MODE_FINAL, N, N, a.AR_DENOISER_NONE)
        ti.ok(lib.ar_session_create(rig.scene_h, ctypes.byref(sd), ctypes.byref(session)), "ar_session_create")
        try:
            view = ti.camera.view_desc(a, rig.cam_fields, N, N)
            ti.ok(lib.ar_session_set_view(session, ctypes.byref(view)), "ar_session_set_view")
            ti.ok(lib.ar_session_start(session), "ar_session_start")
            prog = a.ar_progress()
            for _ in range(600):
                ti.ok(lib.ar_session_wait(session, 100), "ar_session_wait")
                ti.ok(lib.ar_session_get_progress(session, ctypes.byref(prog)), "ar_session_get_progress")
                if prog.state in (a.AR_STATE_CONVERGED, a.AR_STATE_FAILED, a.AR_STATE_CLOSED):
                    break
            self.assertEqual(prog.state, a.AR_STATE_CONVERGED)
            entries = rig.entries_with_subject(session)
            picture = rig._combined(session)
        finally:
            lib.ar_session_destroy(session)
        return (rig.keymap.key(("image", clean.original.session_uid)), rig.keymap.key(("image", bad.original.session_uid)),
                entries, picture)

    def check_refusal(self, texel_value, words):
        clean_key, bad_key, entries, picture = self.render_refusal(texel_value)
        mine = [e for e in entries if e[0] == ABI.AR_REPORT_IMAGE_UNREADABLE]
        print(f"RESULT refusal of {texel_value!r}: {mine}")
        self.assertEqual(len(mine), 1, entries)
        self.assertEqual(mine[0][1], bad_key)  # the bad image only; the clean one has no entry
        self.assertNotEqual(bad_key, clean_key)
        for w in words:
            self.assertIn(w, mine[0][2])
        # The clean image still renders as itself (not magenta, not black).
        want = block_expand(pattern(spike=False)[..., :3])
        self.assertLess((np.abs(picture - want) / want).max(), 5e-3)

    def test_inf_texel_refuses_only_that_image(self):
        self.check_refusal(float("inf"), ["NaN or Inf", "channel R"])

    def test_texel_above_the_limit_refuses_only_that_image(self):
        over = np.nextafter(LIMIT, np.float32(np.inf))
        self.check_refusal(float(over), ["above the limit", "channel R", "index"])


if __name__ == "__main__":
    support.main()
