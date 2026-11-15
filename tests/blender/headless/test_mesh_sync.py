# SPDX-License-Identifier: Apache-2.0
"""Mesh read and topology-cache decisions (0007 d17, d22; V6 is M3)."""
import os
import sys
import time
import unittest
from collections import Counter

import numpy as np

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import support  # noqa: E402

support.load_sync()
from ar_sync import mesh, topology_cache as tc  # noqa: E402


def make(name, verts, faces):
    me = bpy.data.meshes.new(name)
    me.from_pydata(verts, [], faces)
    ob = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(ob)
    return ob


def cube():
    v = [(x, y, z) for z in (0, 1) for y in (0, 1) for x in (0, 1)]
    f = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    return make("cube", v, f)


def read(me, diagnostics=None):
    lay = mesh.layout(me)
    out = mesh.alloc(lay)
    mesh.fill(me, out, diagnostics)
    return lay, out


class NoCornerVert:
    """A mesh whose internal '.corner_vert' attribute is gone."""

    def __init__(self, me):
        self._me = me
        self.attributes = _Attrs(me)

    def __getattr__(self, name):
        return getattr(self._me, name)


class _Attrs:
    def __init__(self, me):
        self._a = me.attributes

    def __getitem__(self, k):
        if k == ".corner_vert":
            raise KeyError(k)
        return self._a[k]

    def __contains__(self, k):
        return k in self._a


class TestMesh(unittest.TestCase):
    def setUp(self):
        support.reset_scene()

    def test_cube(self):
        me = cube().data
        lay, out = read(me)
        self.assertEqual(tuple(lay), (8, 24, 6, 0, False))
        np.testing.assert_array_equal(out.face_start, np.arange(0, 24, 4))
        want = np.empty(24, np.int32)
        me.loops.foreach_get("vertex_index", want)
        np.testing.assert_array_equal(out.corner_vert, want)
        pos = np.array([v.co[:] for v in me.vertices], np.float32).ravel()
        np.testing.assert_array_equal(out.positions, pos)
        self.assertEqual(out.corner_normals.shape, (72,))
        self.assertTrue(np.allclose(np.linalg.norm(out.corner_normals.reshape(-1, 3), axis=1), 1))

    def test_quad_and_ngon(self):
        verts = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0), (2, 0, 0), (2.5, 1, 0), (1, 2, 0)]
        me = make("m", verts, [(0, 1, 2, 3), (1, 4, 5, 6, 2)]).data
        lay, out = read(me)
        self.assertEqual(tuple(lay)[:3], (7, 9, 2))
        np.testing.assert_array_equal(out.face_start, [0, 4])
        np.testing.assert_array_equal(out.corner_vert, [0, 1, 2, 3, 1, 4, 5, 6, 2])

    def test_two_uv_maps(self):
        me = cube().data
        me.uv_layers.new(name="A")
        me.uv_layers.new(name="B")
        for i, name in enumerate(("A", "B")):
            vals = (np.arange(48, dtype=np.float32) + 100 * i)
            me.attributes[name].data.foreach_set("vector", vals)
        lay, out = read(me)
        self.assertEqual(lay.U, 2)
        self.assertEqual(out.uv.shape, (2 * 24 * 2,))
        np.testing.assert_array_equal(out.uv[:48], np.arange(48))
        np.testing.assert_array_equal(out.uv[48:], np.arange(48) + 100)

    def test_material_index_only_when_present(self):
        ob = cube()
        self.assertFalse(mesh.layout(ob.data).has_material_index)
        ob.data.polygons.foreach_set("material_index", [0, 1, 2, 0, 1, 2])
        lay, out = read(ob.data)
        self.assertTrue(lay.has_material_index)
        np.testing.assert_array_equal(out.material_index, [0, 1, 2, 0, 1, 2])

    def test_corner_vert_fallback_warns_and_counts(self):
        me = cube().data
        diag = Counter()
        want = read(me)[1].corner_vert.copy()
        lay = mesh.layout(me)
        out = mesh.alloc(lay)
        mesh.fill(NoCornerVert(me), out, diag)
        self.assertEqual(diag["corner_vert_fallback"], 1)
        np.testing.assert_array_equal(out.corner_vert, want)

    def test_curve_goes_through_to_mesh(self):
        cu = bpy.data.curves.new("c", "CURVE")
        cu.bevel_depth = 0.1
        sp = cu.splines.new("POLY")
        sp.points.add(2)
        for p, co in zip(sp.points, ((0, 0, 0, 1), (1, 0, 0, 1), (2, 1, 0, 1))):
            p.co = co
        ob = bpy.data.objects.new("c", cu)
        bpy.context.scene.collection.objects.link(ob)
        ev = ob.evaluated_get(support.depsgraph())
        with mesh.mesh_of(ev) as me:
            self.assertGreater(len(me.polygons), 0)

    def test_subdivision_takes_full_read(self):
        ob = cube()
        ob.modifiers.new("s", "SUBSURF")
        self.assertFalse(tc.is_deform_only(ob))
        self.assertEqual(tc.decide(True, tc.is_deform_only(ob), False), tc.FULL)

    def test_deform_only_armature_takes_positions(self):
        ob = cube()
        arm = bpy.data.objects.new("arm", bpy.data.armatures.new("arm"))
        bpy.context.scene.collection.objects.link(arm)
        mod = ob.modifiers.new("a", "ARMATURE")
        mod.object = arm
        self.assertTrue(tc.is_deform_only(ob))
        self.assertEqual(tc.decide(True, True, False), tc.POSITIONS)
        # A geometry update on the original Mesh, an empty cache or F12 all force a full read.
        self.assertEqual(tc.decide(True, True, True), tc.FULL)
        self.assertEqual(tc.decide(False, True, False), tc.FULL)
        self.assertEqual(tc.decide(True, True, False, final_render=True), tc.FULL)

    def test_modal_operators(self):
        self.assertEqual(tc.decide(True, False, True, ["TRANSFORM_OT_translate"]), tc.POSITIONS)
        self.assertEqual(tc.decide(True, False, True, ["transform.rotate", "transform.resize"]),
                         tc.POSITIONS)
        self.assertEqual(tc.decide(True, False, True, ["transform.translate", "mesh.bevel"]), tc.FULL)
        self.assertEqual(tc.decide(True, False, True, ["sculpt.brush_stroke"]), tc.POSITIONS)
        self.assertEqual(tc.decide(True, False, True, ["sculpt.brush_stroke"], dynamic_topology=True),
                         tc.FULL)
        self.assertEqual(tc.decide(True, False, True, []), tc.FULL)

    def test_count_mismatch_on_fast_path_does_full_read(self):
        me = cube().data
        cache = tc.TopologyCache()
        cache.counts[7] = (8, 24, 5)  # stale
        diag = Counter()

        class FakeAbi:
            def __getattr__(self, name):
                raise AssertionError("full read reached the engine descriptor: " + name)

        # The full read needs the engine descriptor; reaching it proves the fallback was taken.
        with self.assertRaises(AssertionError):
            tc.read_geometry(None, FakeAbi(), cache, 7, me, "cube", tc.POSITIONS, diag)
        self.assertEqual(diag["topology_cache_mismatch"], 1)

    def test_read_timing_one_million_triangles(self):
        import bmesh

        bm = bmesh.new()
        bmesh.ops.create_grid(bm, x_segments=700, y_segments=700, size=1.0)
        me = bpy.data.meshes.new("big")
        bm.to_mesh(me)
        bm.free()
        lay = mesh.layout(me)
        out = mesh.alloc(lay)
        best = min(_timed(lambda: mesh.fill(me, out)) for _ in range(3))
        tris = lay.C - 2 * lay.F
        print(f"mesh read {tris} triangles: {best:.1f} ms (spike B 65 ms, bound 130 ms)")
        self.assertGreater(tris, 900_000)
        self.assertLess(best, 130.0)


def _timed(fn):
    t = time.perf_counter()
    fn()
    return (time.perf_counter() - t) * 1e3


if __name__ == "__main__":
    support.main()
