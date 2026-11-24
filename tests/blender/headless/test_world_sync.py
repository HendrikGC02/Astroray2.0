# SPDX-License-Identifier: Apache-2.0
"""World program with an Environment Texture (0005 item 32, task 172): the node lowers to
LOAD_CTX_P -> TEX_ENVIRONMENT -> EMIT_LE, what it cannot lower (Mirror Ball, a linked Vector, no image)
is reported once and renders magenta, and a camera in a world of a 64 x 32 test texture sees the
texture where Blender's equirectangular convention puts it: the position of the one white texel is
measured with numpy against the camera model.

Needs the built engine (tools/build.ps1): graph cases read the emitter's arrays, the render case sends
the real graphs and the image to a CPU session through the generated binding, as
tests/blender/test_materials.py does (this file reuses its session and scene helpers).
Ref: docs/decisions/0005-material-model.md item 32; Cycles scene/shader_nodes.cpp
EnvironmentTextureNode, kernel/camera/projection.h direction_to_equirectangular.
"""
import importlib
import math
import os
import sys
import unittest
from unittest import mock

import numpy as np

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import support  # noqa: E402

sys.path.insert(0, os.path.dirname(HERE))
import test_materials as tm  # noqa: E402  (the module, not its TestCase)
from test_lowering import srgb_png  # noqa: E402  (the evidence image writer)

images = importlib.import_module("ar_addon.sync.images")
EVIDENCE = os.path.join(support.ROOT, "out", "evidence", "172")
UNSUPPORTED = tm.AR_REPORT_NODE_UNSUPPORTED

# The test texture: 64 x 32, four coloured quadrants and one white texel.
TEX_W, TEX_H = 64, 32
QUADRANTS = {  # (left of the middle, above the horizon) -> linear colour
    (True, True): (1.0, 0.1, 0.1), (False, True): (0.1, 1.0, 0.1),
    (True, False): (0.1, 0.1, 1.0), (False, False): (1.0, 1.0, 0.1),
}
WHITE_TEXEL = (35, 17)  # (ix, iy), iy counted from the bottom row as Image.pixels is
WHITE = 4.0
SIZE = (320, 160)


def texture_pixels():
    """Image.pixels for the test texture: float RGBA, rows bottom-up."""
    px = np.ones((TEX_H, TEX_W, 4), np.float32)
    for iy in range(TEX_H):
        for ix in range(TEX_W):
            px[iy, ix, :3] = QUADRANTS[(ix < TEX_W // 2, iy >= TEX_H // 2)]
    px[WHITE_TEXEL[1], WHITE_TEXEL[0], :3] = WHITE
    return px.reshape(-1)


def add_environment(world, image=True, interpolation="Closest", projection="EQUIRECTANGULAR", strength=1.0):
    """An Environment Texture linked to the Background colour; returns (node, image)."""
    t = world.node_tree
    env = t.nodes.new("ShaderNodeTexEnvironment")
    img = None
    if image:
        img = bpy.data.images.new("EnvTest", TEX_W, TEX_H, alpha=False, float_buffer=True)
        img.pixels.foreach_set(texture_pixels())
        env.image = img
    env.interpolation = interpolation
    env.projection = projection
    t.links.new(env.outputs["Color"], t.nodes["Background"].inputs["Color"])
    t.nodes["Background"].inputs["Strength"].default_value = strength
    return env, img


def with_images(real):
    """world_sync.sync plus the images the world tree uses, in the same edit (the compiler resolves image
    keys when the edit activates, whatever the order inside it)."""
    def sync(edit, abi, world, keymap, op_id):
        em = real(edit, abi, world, keymap, op_id)
        if world is not None and world.node_tree is not None:
            images.sync(edit, abi, keymap, images.used_images([world.node_tree]), images.ImageState())
        return em
    return sync


def render(scene, plane, mat, world, size=SIZE, spp=16):
    with mock.patch.object(tm.world_sync, "sync", with_images(tm.world_sync.sync)):
        return tm.render(scene, plane, mat, world, size=size, spp=spp)


def env_scene(size=SIZE):
    """A camera at the origin looking along +x (Blender: x rotation 90 degrees, z rotation -90 degrees), 90
    degrees wide (18 mm lens on a 36 mm sensor); the plane the session needs is far behind it."""
    scene, plane, mat, world = tm.new_scene(size=size)
    cam = scene.camera
    cam.location = (0.0, 0.0, 0.0)
    cam.rotation_euler = (math.pi / 2, 0.0, -math.pi / 2)
    cam.data.lens = 18.0
    cam.data.sensor_width = 36.0
    cam.data.sensor_fit = "AUTO"
    plane.location = (-100.0, 0.0, 0.0)
    bpy.context.view_layer.update()
    return scene, plane, mat, world


def pixel_of_direction(d, size=SIZE):
    """(column, row from the top) of the pixel centre that sees world direction d, for env_scene's camera:
    forward +x, right -y, up +z, tan(half horizontal fov) = 1."""
    w, h = size
    f, r, u = d[0], -d[1], d[2]
    ndc_x = (r / f)
    ndc_y = (u / f) * (w / h)
    return (ndc_x + 1.0) / 2.0 * w - 0.5, (1.0 - ndc_y) / 2.0 * h - 0.5


def texel_direction(ix, iy):
    """The unit direction at the centre of texel (ix, iy), Blender's convention: u = 0.5 is +x, +y is
    u = 0.25, v = 1 is +z (the inverse of Cycles' direction_to_equirectangular)."""
    u, v = (ix + 0.5) / TEX_W, (iy + 0.5) / TEX_H
    phi, theta = math.pi - 2.0 * math.pi * u, math.pi * (1.0 - v)
    return (math.sin(theta) * math.cos(phi), math.sin(theta) * math.sin(phi), math.cos(theta))


class TestWorld(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tm.PROPS.register()

    @classmethod
    def tearDownClass(cls):
        tm.PROPS.unregister()

    def test_environment_texture_lowers(self):
        scene, plane, mat, world = tm.new_scene()
        env, img = add_environment(world, interpolation="Linear", strength=2.0)
        km = tm.keys.KeyMap()
        em = tm.translate.translate_world(world, km, tm.OP)
        self.assertEqual(tm.unsupported_strings(em), [])
        nodes = tm.decode(em)
        self.assertEqual(sorted(n["op"] for n in nodes), ["CONST_F32", "EMIT_LE", "LOAD_CTX_P", "TEX_ENVIRONMENT"])
        tex = next(i for i, n in enumerate(nodes) if n["op"] == "TEX_ENVIRONMENT")
        self.assertEqual(nodes[tex]["i32"], [tm.keys.image_key(km, img), tm.translate.TEX_LINEAR])
        self.assertEqual(nodes[tm.inputs_of(em, tex)[0]]["op"], "LOAD_CTX_P")  # the ray direction
        emit = next(i for i, n in enumerate(nodes) if n["op"] == "EMIT_LE")
        feeds = tm.inputs_of(em, emit)
        self.assertEqual(feeds[0], tex)
        self.assertEqual(nodes[feeds[1]]["op"], "CONST_F32")
        self.assertEqual(nodes[feeds[1]]["f32"], [2.0])
        env.interpolation = "Closest"  # no extension bit exists: environment images always repeat
        em = tm.translate.translate_world(world, tm.keys.KeyMap(), tm.OP)
        self.assertEqual(tm.decode(em)[next(i for i, n in enumerate(tm.decode(em)) if n["op"] == "TEX_ENVIRONMENT")]["i32"][1], 0)

    def test_unlowered_settings_are_reported_once(self):
        cases = (("projection", dict(projection="MIRROR_BALL"), "Environment Texture.projection"),
                 ("no image", dict(image=False), "Environment Texture.image"),
                 ("interpolation", dict(interpolation="Cubic"), "Environment Texture.interpolation"))
        for name, kw, label in cases:
            with self.subTest(name):
                scene, plane, mat, world = tm.new_scene()
                add_environment(world, **kw)
                em = tm.translate.translate_world(world, tm.keys.KeyMap(), tm.OP)
                self.assertEqual(tm.unsupported_strings(em), [label])
        scene, plane, mat, world = tm.new_scene()
        env, _ = add_environment(world)
        t = world.node_tree
        coord = t.nodes.new("ShaderNodeTexCoord")
        t.links.new(coord.outputs["Generated"], env.inputs["Vector"])
        em = tm.translate.translate_world(world, tm.keys.KeyMap(), tm.OP)
        self.assertEqual(tm.unsupported_strings(em), ["Environment Texture.Vector"])

    def test_mirror_ball_world_renders_magenta_with_one_entry(self):
        scene, plane, mat, world = env_scene((32, 16))
        add_environment(world, projection="MIRROR_BALL")
        img, entries, _ = render(scene, plane, mat, world, size=(32, 16), spp=4)
        self.assertEqual([e for e in entries if e[0] == UNSUPPORTED],
                         [(UNSUPPORTED, "Environment Texture.projection")])
        rgb = img[..., :3].reshape(-1, 3).mean(axis=0)
        self.assertTrue(tm.is_magenta(rgb), f"expected the magenta world, got {rgb}")

    def test_environment_world_renders_the_texture_where_the_convention_puts_it(self):
        scene, plane, mat, world = env_scene()
        add_environment(world)
        img, entries, graph = render(scene, plane, mat, world)
        self.assertEqual(tm.unsupported_strings(graph), [])
        self.assertEqual([e for e in entries if e[0] == UNSUPPORTED], [])
        os.makedirs(EVIDENCE, exist_ok=True)
        srgb_png(os.path.join(EVIDENCE, "env_world.png"), np.clip(img[..., :3], 0.0, 1.0))
        h, w = img.shape[:2]
        lum = img[..., :3].mean(axis=2)
        # The white texel: the pixels above any quadrant colour (their mean is at most 0.7), as a
        # luminance-weighted centroid, against the pixel the camera model gives its centre direction.
        mask = lum > 2.0
        self.assertGreater(int(mask.sum()), 50, "the white texel is not in the image")
        ys, xs = np.nonzero(mask)
        weights = lum[mask]
        cx, cy = float((xs * weights).sum() / weights.sum()), float((ys * weights).sum() / weights.sum())
        px, py = pixel_of_direction(texel_direction(*WHITE_TEXEL), SIZE)
        print(f"RESULT white texel centroid ({cx:.2f}, {cy:.2f}) px, predicted ({px:.2f}, {py:.2f}) px, "
              f"{int(mask.sum())} pixels")
        self.assertAlmostEqual(cx, px, delta=1.5)
        self.assertAlmostEqual(cy, py, delta=1.5)
        # Its colour is the texel's, and away from it the quadrants are where their halves of the
        # image put them: left of +x is u < 0.5 (the +y side), above the horizon is v > 0.5.
        self.assertLess(abs(float(lum[mask].mean()) - WHITE), 0.3)
        for (left, up), want in QUADRANTS.items():
            col = int(w * (0.30 if left else 0.70))
            row = int(h * (0.20 if up else 0.80))
            got = img[row, col, :3]
            self.assertTrue(np.allclose(got, want, atol=0.08), f"left={left} up={up}: {got} != {want}")


if __name__ == "__main__":
    support.main()
