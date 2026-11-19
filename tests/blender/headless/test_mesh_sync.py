# SPDX-License-Identifier: Apache-2.0
"""Mesh read and topology-cache decisions (0007 d17, d22; V6 is M3)."""
import ctypes
import os
import re
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


def read(me):
    lay = mesh.layout(me)
    out = mesh.alloc(lay)
    mesh.fill(me, out)
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

    def test_missing_corner_vert_raises_with_version(self):
        # 0007 d17 as amended by d35: no slow-path fallback, the read fails naming the Blender version.
        me = cube().data
        out = mesh.alloc(mesh.layout(me))
        with self.assertRaisesRegex(mesh.LayoutMismatch, r"'\.corner_vert'.*attribute missing.*" + re.escape(bpy.app.version_string)):
            mesh.fill(NoCornerVert(me), out)

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
        # 0009: budgets are measured on the reference machine, not on hosted runners.
        if os.environ.get("CI") != "true":
            self.assertLess(best, 130.0)


class StagingAbi:
    """ar_mesh_desc / ar_mesh_buffers / ar_positions_buffers with the fields sync_mesh and read_geometry use; the
    staged fields are T** as in astroray.h, here pointing at a c_void_p that holds a NumPy array's address."""

    class ar_mesh_desc(ctypes.Structure):
        _fields_ = [(f, ctypes.c_uint64) for f in ("key", "vertex_count", "corner_count", "face_count",
                                                     "uv_layer_count", "motion_steps", "flags")] + \
                   [("name", ctypes.c_uint8 * 64)]

    _ARRAYS = ("positions", "corner_vert", "face_start", "corner_normals", "uv", "material_index")

    class ar_mesh_buffers(ctypes.Structure):
        _fields_ = [(f + "_count", ctypes.c_uint64) for f in ("positions", "corner_vert", "face_start",
                                                                "corner_normals", "uv", "material_index")] + \
                   [(f, ctypes.POINTER(ctypes.c_void_p)) for f in ("positions", "corner_vert", "face_start",
                                                                     "corner_normals", "uv", "material_index")]

    class ar_positions_buffers(ctypes.Structure):
        _fields_ = [("positions_count", ctypes.c_uint64), ("corner_normals_count", ctypes.c_uint64),
                    ("positions", ctypes.POINTER(ctypes.c_void_p)), ("corner_normals", ctypes.POINTER(ctypes.c_void_p))]


class StagingEdit:
    """The engine side of a staged mesh item: NumPy arrays of the sizes the descriptor asks for."""

    def __init__(self):
        self.arrays, self.commits, self._keep = {}, [], []

    def _stage(self, b, name, n, dtype):
        arr = np.full(n, -7, dtype)
        slot = ctypes.c_void_p(arr.ctypes.data)
        self._keep.append(slot)
        setattr(b, name + "_count", n)
        setattr(b, name, ctypes.pointer(slot))
        self.arrays[name] = arr

    def call(self, name, *args):
        if name == "ar_edit_mesh_begin":
            d, b = args[0]._obj, args[1]._obj
            V, C, F, U = d.vertex_count, d.corner_count, d.face_count, d.uv_layer_count
            for arr, n, dt in (("positions", V * 3, np.float32), ("corner_vert", C, np.int32),
                               ("face_start", F, np.int32), ("corner_normals", C * 3, np.float32),
                               ("uv", U * C * 2, np.float32), ("material_index", F if d.flags & 1 else 0, np.int32)):
                self._stage(b, arr, n, dt)
        elif name == "ar_edit_positions_begin":
            b = args[1]._obj
            self._stage(b, "positions", self.V * 3, np.float32)
            self._stage(b, "corner_normals", self.C * 3, np.float32)

    def item_commit(self, key):
        self.commits.append(key)


class BrokenPolygons:
    """A mesh whose face offsets report a last element outside one contiguous array: a checked read raises,
    a read that skips the check copies the (real, contiguous) array."""

    def __init__(self, me):
        self._me = me

    def __getattr__(self, name):
        if name != "polygons":
            return getattr(self._me, name)
        polys = self._me.polygons

        class Last:
            loop_start = polys[-1].loop_start

            def as_pointer(self):
                return polys[0].as_pointer() + 4096

        class Coll:
            def __len__(self):
                return len(polys)

            def __getitem__(self, i):
                return Last() if i in (-1, len(polys) - 1) else polys[i]
        return Coll()


class TestLayoutChecks(unittest.TestCase):
    """The layout check runs on the first read of a key, again after a geometry update (forget) or a changed
    Layout, and is skipped for an unchanged key (0007 d35 note)."""

    def setUp(self):
        support.reset_scene()
        self.me = cube().data
        self.me.uv_layers.new(name="UVMap")
        self.checks = mesh.LayoutChecks()

    def sync(self, me, key):
        edit = StagingEdit()
        mesh.sync_mesh(edit, StagingAbi, me, key, "cube", checks=self.checks)
        return edit

    def test_staged_read_matches_the_numpy_read(self):
        edit = self.sync(self.me, 3)
        _, want = read(self.me)
        for name in StagingAbi._ARRAYS:
            self.assertEqual(edit.arrays[name].tobytes(), getattr(want, name).tobytes(), name)
        self.assertEqual(edit.commits, [3])

    def test_unchanged_key_skips_the_check(self):
        self.sync(self.me, 3)
        edit = self.sync(BrokenPolygons(self.me), 3)  # checked once at this Layout: no second check
        np.testing.assert_array_equal(edit.arrays["face_start"], np.arange(0, 24, 4))

    def test_new_key_is_checked(self):
        self.sync(self.me, 3)
        with self.assertRaisesRegex(mesh.LayoutMismatch, r"polygons \(face offsets\)"):
            self.sync(BrokenPolygons(self.me), 4)

    def test_geometry_update_rechecks(self):
        self.sync(self.me, 3)
        self.checks.forget(3)  # what the viewport does on a geometry update of the key
        with self.assertRaisesRegex(mesh.LayoutMismatch, r"polygons \(face offsets\)"):
            self.sync(BrokenPolygons(self.me), 3)

    def test_changed_topology_rechecks(self):
        self.sync(self.me, 3)
        changed = make("more", [(0, 0, 0), (1, 0, 0), (0, 1, 0), (1, 1, 1)], [(0, 1, 2), (1, 3, 2)]).data
        changed.uv_layers.new(name="UVMap")
        with self.assertRaisesRegex(mesh.LayoutMismatch, r"polygons \(face offsets\)"):
            self.sync(BrokenPolygons(changed), 3)

    def test_positions_path_skips_only_a_checked_key(self):
        cache = tc.TopologyCache()
        cache.counts[3] = (8, 24, 6)

        class Broken:  # corner normals whose last element is not contiguous with the first
            def __init__(self, me):
                self._me = me

            def __getattr__(self, name):
                if name != "corner_normals":
                    return getattr(self._me, name)
                cn = self._me.corner_normals

                class Last:
                    vector = cn[len(cn) - 1].vector

                    def as_pointer(self):
                        return cn[0].as_pointer() + 4096
                return type("C", (), {"__len__": lambda s: len(cn),
                                      "__getitem__": lambda s, i: Last() if i == len(cn) - 1 else cn[i]})()
        edit = StagingEdit()
        edit.V, edit.C = 8, 24
        with self.assertRaisesRegex(mesh.LayoutMismatch, "corner_normals"):
            tc.read_geometry(edit, StagingAbi, cache, 3, Broken(self.me), "cube", tc.POSITIONS, checks=self.checks)
        self.sync(self.me, 3)
        self.assertEqual(tc.read_geometry(edit, StagingAbi, cache, 3, Broken(self.me), "cube", tc.POSITIONS,
                                          checks=self.checks), tc.POSITIONS)
        want = np.empty(72, np.float32)
        self.me.corner_normals.foreach_get("vector", want)
        self.assertEqual(edit.arrays["corner_normals"].tobytes(), want.tobytes())


def _timed(fn):
    t = time.perf_counter()
    fn()
    return (time.perf_counter() - t) * 1e3


if __name__ == "__main__":
    support.main()
