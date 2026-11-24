# SPDX-License-Identifier: Apache-2.0
"""Lamp and settings sync (0007 d37, 0003 d11, d15; task 082)."""
import ctypes
import math
import os
import sys
import unittest
from types import SimpleNamespace

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import support  # noqa: E402

support.load_sync()
from ar_sync import keys, lights, settings  # noqa: E402


class _Xform(ctypes.Structure):
    _fields_ = [("m", ctypes.c_float * 12)]


class _Spectrum(ctypes.Structure):
    _fields_ = [("kind", ctypes.c_uint32), ("rgb", ctypes.c_float * 3), ("scale", ctypes.c_float)]


class _LightDesc(ctypes.Structure):
    _fields_ = ([("color", _Spectrum), ("xf", _Xform), ("type", ctypes.c_uint32),
                 ("visibility", ctypes.c_uint32)]
                + [(n, ctypes.c_float) for n in ("size_x", "size_y", "spread", "power")]
                + [(n, ctypes.c_uint32) for n in ("normalize", "cast_shadow", "light_group")])


# Just the structs set_light fills, under the generated binding's names.
FakeAbi = SimpleNamespace(ar_xform=_Xform, ar_light_desc=_LightDesc)


class FakeEdit:
    def __init__(self):
        self.calls = []

    def call(self, name, *args):
        self.calls.append((name, args))


def lamp(kind, name="L", loc=(0, 0, 0), **props):
    light = bpy.data.lights.new(name, kind)
    for k, v in props.items():
        setattr(light, k, v)
    ob = bpy.data.objects.new(name, light)
    bpy.context.scene.collection.objects.link(ob)
    ob.location = loc
    return ob


def build(origin=(0.0, 0.0, 0.0), groups=None):
    km = keys.KeyMap()
    return lights.build(support.depsgraph(), km, origin, groups)


class TestLights(unittest.TestCase):
    def setUp(self):
        support.reset_scene()

    def test_point_radius_energy_unscaled(self):
        ob = lamp("POINT", energy=1000.0, shadow_soft_size=0.3, color=(1.0, 0.5, 0.25), use_soft_falloff=False)
        for attr, _ in lights.instances._VISIBILITY:
            setattr(ob, attr, True)
        out, lines = build()
        self.assertEqual(lines, [])
        (_, f), = out
        self.assertEqual(f["type"], lights.POINT)
        self.assertAlmostEqual(f["size_x"], 0.3, places=6)
        self.assertEqual(f["power"], 1000.0)  # watts pass through, factor 1 (0003 d11)
        self.assertEqual(f["color"], (1.0, 0.5, 0.25))
        self.assertEqual((f["normalize"], f["cast_shadow"], f["visibility"]), (1, 1, 0x3F))

    def test_exposure_and_temperature(self):
        lamp("POINT", energy=10.0, exposure=2.0, use_soft_falloff=False)
        (_, f), = build()[0]
        self.assertAlmostEqual(f["power"], 40.0, places=4)  # exp2(exposure), as Cycles
        support.reset_scene()
        base = (1.0, 0.5, 0.25)
        ob = lamp("POINT", use_temperature=True, temperature=3000.0, color=base, use_soft_falloff=False)
        (_, f), = build()[0]
        for got, c, t in zip(f["color"], base, ob.data.temperature_color):
            self.assertAlmostEqual(got, c * t, places=6)  # multiplied into the colour, not replacing it
        ob.data.use_temperature = False  # the temperature is ignored when the toggle is off
        (_, f), = build()[0]
        self.assertEqual(f["color"], base)

    def test_soft_falloff_is_reported(self):
        lamp("POINT", name="Soft", shadow_soft_size=0.25, use_soft_falloff=True)
        out, lines = build()
        self.assertEqual(len(out), 1)
        self.assertTrue(any("Soft Falloff" in s and "Soft" in s for s in lines), lines)

    def test_area_shapes(self):
        cases = {"SQUARE": (lights.AREA_RECT, 2.0, 2.0), "RECTANGLE": (lights.AREA_RECT, 2.0, 3.0),
                 "DISK": (lights.AREA_ELLIPSE, 2.0, 2.0), "ELLIPSE": (lights.AREA_ELLIPSE, 2.0, 3.0)}
        for shape, (t, sx, sy) in cases.items():
            support.reset_scene()
            lamp("AREA", shape=shape, size=2.0, size_y=3.0, spread=math.radians(60))
            (_, f), = build()[0]
            self.assertEqual((f["type"], f["size_x"], f["size_y"]), (t, sx, sy), shape)
            self.assertAlmostEqual(f["spread"], math.radians(60), places=6)  # radians, not degrees

    def test_spot_and_sun_are_skipped_with_a_line(self):
        lamp("SPOT", name="S1")
        lamp("SUN", name="S2")
        out, lines = build()
        self.assertEqual(out, [])
        self.assertEqual(len(lines), 2)
        self.assertTrue(any("lamp type SPOT lands in M4" in s for s in lines), lines)
        self.assertTrue(any("lamp type SUN lands in M4" in s for s in lines), lines)

    def test_flags_visibility_group_and_origin(self):
        ob = lamp("AREA", loc=(10, 20, 30), normalize=False, use_shadow=False)
        for attr, _ in lights.instances._VISIBILITY:
            setattr(ob, attr, True)
        ob.visible_camera = False
        ob.visible_glossy = False
        ob.lightgroup = "key"
        (_, f), = build(origin=(1.0, 2.0, 3.0), groups={"key": 7})[0]
        self.assertEqual((f["normalize"], f["cast_shadow"]), (0, 0))
        self.assertEqual(f["visibility"], 0x3F & ~1 & ~4)
        self.assertEqual(f["light_group"], 7)
        self.assertEqual((f["xf"][3], f["xf"][7], f["xf"][11]), (9.0, 18.0, 27.0))

    def test_light_node_tree_is_reported(self):
        ob = lamp("POINT", name="Nodes", use_soft_falloff=False)
        ob.data.use_nodes = True
        tree = ob.data.node_tree
        tree.nodes.new("ShaderNodeBlackbody")
        out, lines = build()
        self.assertEqual(len(out), 1)
        self.assertTrue(any("node tree" in s and "M4" in s for s in lines), lines)

    def test_set_light_fills_the_descriptor(self):
        lamp("AREA", shape="RECTANGLE", size=1.5, size_y=0.5, energy=80.0, loc=(1, 2, 3))
        (key, f), = build()[0]
        edit = FakeEdit()
        lights.set_light(edit, FakeAbi, key, f)
        (name, (k, ref)), = edit.calls
        d = ref._obj
        self.assertEqual((name, k), ("ar_edit_set_light", key))
        self.assertEqual((d.type, d.size_x, d.size_y, d.power), (lights.AREA_RECT, 1.5, 0.5, 80.0))
        self.assertEqual((d.color.kind, d.color.scale, d.xf.m[3]), (lights.SPECTRUM_RGB, 1.0, 1.0))


class FakeColorspace:
    def __init__(self, interop_id):
        self.working_space_interop_id = interop_id


class TestSettings(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        support.acquire_properties()

    @classmethod
    def tearDownClass(cls):
        support.release_properties()

    def setUp(self):
        self.scene = support.reset_scene()

    def test_values(self):
        a, r = self.scene.astroray, self.scene.render
        a.samples, a.viewport_samples, a.seed, a.use_animated_seed = 32, 8, 5, True
        a.max_bounces, a.min_bounces, a.pixel_filter_type, a.film_exposure = 6, 2, "GAUSSIAN", 2.0
        r.resolution_x, r.resolution_y, r.resolution_percentage = 64, 48, 50
        a.pixel_filter_width = 2.5
        a.transparent_bounces = 3
        f, lines = settings.build(self.scene, FakeColorspace("lin_rec2020_scene"), passes=("Position",),
                                  lightgroups={"a": 1, "b": 2})
        self.assertEqual(lines, [])
        want = {"samples": 32, "viewport_samples": 8, "seed": 5, "animated_seed": 1, "max_bounces": 6,
                "min_bounces": 2, "pixel_filter": 1, "pixel_filter_width": 2.5, "film_exposure": 2.0,
                "working_space": 2, "output_width": 64, "output_height": 48, "resolution_percent": 50,
                "passes": ("Position",), "light_group_count": 2, "caustics_reflective": 1,
                "caustics_refractive": 1, "filter_glossy": 1.0, "film_transparent": 0,
                "transparent_bounces": 3}
        for k, v in want.items():
            self.assertEqual(f[k], v, k)

    def test_defaults_match_cycles(self):
        f, _ = settings.build(self.scene, FakeColorspace("lin_rec709_scene"))
        self.assertEqual((f["samples"], f["viewport_samples"], f["max_bounces"], f["pixel_filter"]),
                         (4096, 1024, 12, settings.FILTERS["BLACKMAN_HARRIS"]))
        self.assertEqual(f["pixel_filter_width"], 1.5)

    def test_real_blender_working_space(self):
        f, _ = settings.build(self.scene, bpy.data.colorspace)
        self.assertEqual(f["working_space"], settings.WORKING_SPACES[bpy.data.colorspace.working_space_interop_id])

    def test_other_working_space_refuses(self):
        with self.assertRaisesRegex(settings.UnsupportedWorkingSpace, "srgb_p3d65"):
            settings.build(self.scene, FakeColorspace("srgb_p3d65_display"))

    def test_unsupported_settings_are_reported(self):
        self.scene.render.use_border = True
        self.scene.render.film_transparent = True
        self.scene.render.use_motion_blur = True
        _, lines = settings.build(self.scene, FakeColorspace("lin_rec709_scene"))
        self.assertEqual(len(lines), 3)
        self.assertTrue(any("border" in s for s in lines))
        self.assertTrue(any("Transparent" in s for s in lines))
        self.assertTrue(any("motion blur" in s for s in lines))


if __name__ == "__main__":
    support.main()
