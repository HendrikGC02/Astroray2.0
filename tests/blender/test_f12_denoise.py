# SPDX-License-Identifier: Apache-2.0
"""OIDN on and off in F12, through the enabled extension and bpy.ops.render.render (windowless), on CPU and CUDA.

Run through Invoke-ArBlender: -- [--evidence <dir>] [--work <dir>]
A Cornell box (red and green walls, a tall box and a cube, an area lamp in the ceiling) renders at 32 spp
without and with `scene.astroray.use_denoising`. The cases check what lands in Blender's RenderResult (the
passes are read back out of the result the engine filled, not out of the engine) and that the denoise did
what it is for: less pixel noise, the mean left alone to a few percent. The images are the evidence the task
asks to be looked at: out/evidence/198/f12_oidn_{off,on}_{cpu,cuda}.png.
Ref: docs/decisions/0008-film-outputs-denoise.md d10, d16, d17; docs/decisions/0007-blender-integration.md d27.
"""
import importlib
import math
import os
import sys
import unittest

import numpy as np

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "headless"))
import smoke_a1  # noqa: E402  (the extension install helper)
import support  # noqa: E402

MODULE = smoke_a1.MODULE
SIZE = 160
SPP = 32
ARGS = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []


def arg(name, default):
    return ARGS[ARGS.index(name) + 1] if name in ARGS else default


def _quad(name, verts, colour, collection):
    me = bpy.data.meshes.new(name)
    me.from_pydata(verts, [], [(0, 1, 2, 3)])
    ob = bpy.data.objects.new(name, me)
    collection.objects.link(ob)
    _paint(ob, colour)
    return ob


def _box(name, centre, size, yaw, colour, collection):
    sx, sy, sz = size
    corners = [(x * sx / 2, y * sy / 2, z * sz / 2) for z in (-1, 1) for y in (-1, 1) for x in (-1, 1)]
    faces = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    me = bpy.data.meshes.new(name)
    me.from_pydata(corners, [], faces)
    ob = bpy.data.objects.new(name, me)
    collection.objects.link(ob)
    ob.location, ob.rotation_euler = centre, (0.0, 0.0, math.radians(yaw))
    _paint(ob, colour)
    return ob


def _paint(ob, colour):
    mat = bpy.data.materials.new(ob.name)
    mat.use_nodes = True
    p = mat.node_tree.nodes["Principled BSDF"]
    p.inputs["Base Color"].default_value = (*colour, 1.0)
    p.inputs["Roughness"].default_value = 1.0
    p.inputs["Specular IOR Level"].default_value = 0.0
    ob.data.materials.append(mat)


def cornell(scene):
    """The classic box: open at the front, lamp in the ceiling, a black world."""
    c = scene.collection
    white, red, green = (0.73, 0.73, 0.73), (0.65, 0.05, 0.05), (0.12, 0.45, 0.15)
    _quad("floor", [(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)], white, c)
    _quad("ceiling", [(-1, -1, 2), (-1, 1, 2), (1, 1, 2), (1, -1, 2)], white, c)
    _quad("back", [(-1, 1, 0), (1, 1, 0), (1, 1, 2), (-1, 1, 2)], white, c)
    _quad("left", [(-1, -1, 0), (-1, 1, 0), (-1, 1, 2), (-1, -1, 2)], red, c)
    _quad("right", [(1, -1, 0), (1, -1, 2), (1, 1, 2), (1, 1, 0)], green, c)
    _box("tall", (-0.35, 0.35, 0.6), (0.6, 0.6, 1.2), 18.0, white, c)
    _box("cube", (0.4, -0.25, 0.3), (0.6, 0.6, 0.6), -20.0, white, c)
    ld = bpy.data.lights.new("lamp", "AREA")
    ld.energy, ld.size = 40.0, 0.5
    lamp = bpy.data.objects.new("lamp", ld)
    c.objects.link(lamp)
    lamp.location = (0.0, 0.0, 1.98)
    cd = bpy.data.cameras.new("cam")
    cam = bpy.data.objects.new("cam", cd)
    c.objects.link(cam)
    cam.location, cam.rotation_euler = (0.0, -4.4, 1.0), (math.radians(90), 0.0, 0.0)
    scene.camera = cam
    world = bpy.data.worlds.new("world")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0, 0, 0, 1)
    scene.world = world


class ResultCapture:
    """The pixels of every pass the final copy left in the RenderResult, read back from Blender's own rects,
    and how many passes the result holds; installed over Job._copy_passes of the enabled extension."""

    def __init__(self, render_final):
        self.rf, self.passes, self.count = render_final, {}, None
        self._orig = render_final.Job._copy_passes
        capture, orig = self, self._orig

        def copy(job, engine, result, w, h, final=False):
            orig(job, engine, result, w, h, final)
            if final:
                held = result.layers[0].passes
                capture.count = len(held)
                for name in ("Combined",) + tuple(p.name for p in job.wanted):
                    rect = held[name].rect
                    buf = np.zeros(w * h * held[name].channels, np.float32)
                    rect.foreach_get(buf)
                    capture.passes[name] = buf

        render_final.Job._copy_passes = copy

    def restore(self):
        self.rf.Job._copy_passes = self._orig

    def rgb(self, name):
        a = self.passes[name]
        return a.reshape(SIZE, SIZE, a.size // (SIZE * SIZE))[:, :, :3]


def roughness(img):
    """Mean absolute difference from the 3x3 box mean over the interior: the pixel noise of a smooth image."""
    lum = img.mean(axis=2)
    blur = sum(lum[1 + dy:SIZE - 1 + dy, 1 + dx:SIZE - 1 + dx] for dy in (-1, 0, 1) for dx in (-1, 0, 1)) / 9.0
    return float(np.abs(lum[1:-1, 1:-1] - blur).mean())


class TestF12Denoise(unittest.TestCase):
    runs = {}

    @classmethod
    def setUpClass(cls):
        smoke_a1.ensure_extension()
        cls.rf = importlib.import_module(MODULE + ".render_final")
        cls.evidence = arg("--evidence", os.path.join(ROOT, "out", "evidence", "198"))
        os.makedirs(cls.evidence, exist_ok=True)
        # Not read_factory_settings: it would disable the extension.
        for coll in (bpy.data.objects, bpy.data.meshes, bpy.data.materials, bpy.data.lights, bpy.data.cameras,
                     bpy.data.worlds):
            for datablock in list(coll):
                coll.remove(datablock)
        bpy.context.scene.render.engine = "ASTRORAY"
        cornell(bpy.context.scene)
        cls.devices = list(importlib.import_module(MODULE + ".native").devices())

    @classmethod
    def tearDownClass(cls):
        cls.rf.shutdown()

    def f12(self, device, denoise):
        """One F12 on the open scene; returns (ResultCapture, the Combined display image as PNG-path-free floats)."""
        scene = bpy.context.scene
        scene.render.resolution_x = scene.render.resolution_y = SIZE
        scene.render.resolution_percentage = 100
        a = scene.astroray
        a.device, a.samples, a.seed = device, SPP, 3
        a.use_denoising = a.denoising_store_passes = denoise
        bpy.context.view_layer.use_pass_normal = True
        cap = ResultCapture(self.rf)
        try:
            self.assertEqual(bpy.ops.render.render(), {"FINISHED"})
        finally:
            cap.restore()
        self.assertIn(device, self.rf._contexts, f"the render never created a {device} context")
        png = os.path.join(self.evidence, f"f12_oidn_{'on' if denoise else 'off'}_{device.lower()}.png")
        scene.render.image_settings.file_format, scene.render.image_settings.color_depth = "PNG", "8"
        bpy.data.images["Render Result"].save_render(png, scene=scene)
        return cap

    def render_pair(self, device):
        self.assertIn(device, self.devices, f"the engine context offers {self.devices}")
        if device not in self.runs:
            off, on = self.f12(device, False), self.f12(device, True)
            self.runs[device] = (off, on)
            m_off, m_on = off.rgb("Combined").mean(axis=(0, 1)), on.rgb("Combined").mean(axis=(0, 1))
            r_off, r_on = roughness(off.rgb("Combined")), roughness(on.rgb("Combined"))
            print(f"RESULT f12 oidn {device}: mean RGB off {np.round(m_off, 5).tolist()} on {np.round(m_on, 5).tolist()} "
                  f"shift {np.round((m_on / m_off - 1) * 100, 2).tolist()} %; roughness off {r_off:.5f} on {r_on:.5f}")
        return self.runs[device]

    def check_device(self, device):
        off, on = self.render_pair(device)
        self.assertEqual(sorted(off.passes), ["Combined", "Normal"], "Denoise off: Combined and the requested Normal")
        self.assertEqual(off.count, 2)
        self.assertEqual(sorted(on.passes), ["Combined", "Denoising Albedo", "Denoising Normal", "Noisy Image", "Normal"])
        self.assertEqual(on.count, 5, "every pass exists once in the result")
        self.assertGreater(roughness(off.rgb("Combined")), 2.0 * roughness(on.rgb("Combined")),
                           "the denoised image is not visibly smoother than the noisy one")
        m_off, m_on = off.rgb("Combined").mean(), on.rgb("Combined").mean()
        self.assertLess(abs(m_on / m_off - 1.0), 0.05, "the denoise moved the frame mean")
        return off, on

    def test_cpu_oidn_on_and_off(self):
        off, on = self.check_device("CPU")
        # Same seed, same CPU: the noisy image of the denoised render is the undenoised render, bit for bit.
        np.testing.assert_array_equal(on.rgb("Noisy Image"), off.rgb("Combined"))

    def test_cuda_oidn_on_and_off(self):
        off, on = self.check_device("CUDA")
        self.assertGreater(float(np.abs(on.rgb("Combined") - on.rgb("Noisy Image")).max()), 0.0)


if __name__ == "__main__":
    support.main()
