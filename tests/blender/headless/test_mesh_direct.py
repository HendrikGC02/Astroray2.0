# SPDX-License-Identifier: Apache-2.0
"""Direct (as_pointer) mesh reads against the attribute API (0007 d35 remedy 1, d17).

Byte-identical topology, positions, normals, UVs and material indices on the golden triangulation faces
(tests/data/triangulation/golden.json) and on b1m's grid (scenes/b1m/build.py), and a loud error for every
broken layout assumption."""
import importlib.util
import json
import os
import re
import sys
import unittest

import numpy as np

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import support  # noqa: E402

support.load_sync()
from ar_sync import mesh  # noqa: E402

GOLDEN = os.path.join(support.ROOT, "tests", "data", "triangulation", "golden.json")


def api_read(me):
    """The attribute-API reads the addon made before the direct read, into plain NumPy arrays."""
    V, C, F = len(me.vertices), len(me.loops), len(me.polygons)
    out = {"positions": np.empty(V * 3, np.float32), "corner_vert": np.empty(C, np.int32),
           "face_start": np.empty(F, np.int32), "corner_normals": np.empty(C * 3, np.float32),
           "uv": np.empty(len(me.uv_layers) * C * 2, np.float32)}
    if V:  # an empty mesh has neither attribute, a mesh without faces no '.corner_vert'
        me.attributes["position"].data.foreach_get("vector", out["positions"])
    if C:
        me.attributes[".corner_vert"].data.foreach_get("value", out["corner_vert"])
    loops = np.empty(C, np.int32)
    me.loops.foreach_get("vertex_index", loops)
    assert np.array_equal(loops, out["corner_vert"])
    me.polygons.foreach_get("loop_start", out["face_start"])
    me.corner_normals.foreach_get("vector", out["corner_normals"])
    for i, layer in enumerate(me.uv_layers):
        me.attributes[layer.name].data.foreach_get("vector", out["uv"][i * C * 2:(i + 1) * C * 2])
    if "material_index" in me.attributes:
        out["material_index"] = np.empty(F, np.int32)
        me.attributes["material_index"].data.foreach_get("value", out["material_index"])
    return out


def mesh_from(name, pos, cv, fs, uv=None, normals=None):
    """A Mesh built the way scenes/lib/blend_writer.py builds one."""
    me = bpy.data.meshes.new(name)
    me.vertices.add(len(pos))
    me.vertices.foreach_set("co", np.asarray(pos, np.float32).reshape(-1))
    me.loops.add(len(cv))
    me.loops.foreach_set("vertex_index", np.asarray(cv, np.int32))
    me.polygons.add(len(fs))
    me.polygons.foreach_set("loop_start", np.asarray(fs, np.int32))
    me.polygons.foreach_set("loop_total", np.diff(np.append(fs, len(cv))).astype(np.int32))
    me.update(calc_edges=True)
    if uv is not None:
        me.uv_layers.new(name="UVMap").data.foreach_set("uv", np.asarray(uv, np.float32).reshape(-1))
    if normals is not None:
        me.normals_split_custom_set(np.asarray(normals, np.float32))
    return me


def golden_mesh():
    """Every golden triangulation face in one mesh, two UV maps and per-face material indices."""
    with open(GOLDEN, encoding="utf-8") as f:
        cases = json.load(f)["cases"]
    pos, cv, fs = [], [], []
    for c in cases:
        fs.append(len(cv))
        cv.extend(range(len(pos), len(pos) + len(c["positions"])))
        pos.extend(c["positions"])
    me = mesh_from("golden", pos, cv, fs)
    C = len(cv)
    for name, scale in (("A", 1.0), ("B", -0.5)):
        me.uv_layers.new(name=name).data.foreach_set("uv", (np.arange(2 * C, dtype=np.float32) * scale) / C)
    me.polygons.foreach_set("material_index", np.arange(len(fs), dtype=np.int32) % 3)
    return me, len(cases)


def b1m_mesh():
    spec = importlib.util.spec_from_file_location("b1m_build", os.path.join(support.ROOT, "scenes", "b1m", "build.py"))
    b1m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(b1m)
    pos, cv, fs, nrm, uv = b1m.grid()
    return mesh_from("b1m", pos, cv, fs, uv, nrm)


class TestDirectEqualsApi(unittest.TestCase):
    def setUp(self):
        support.reset_scene()

    def assert_same(self, me):
        want = api_read(me)
        lay = mesh.layout(me)
        out = mesh.alloc(lay)
        mesh.fill(me, out)
        for name, arr in want.items():
            got = getattr(out, name)
            self.assertEqual(got.dtype, arr.dtype, name)
            self.assertEqual(got.tobytes(), arr.tobytes(), f"{name} differs from the attribute API")
        self.assertEqual(len(out.material_index), len(want.get("material_index", ())))
        return lay

    def test_golden_triangulation_faces(self):
        me, n = golden_mesh()
        lay = self.assert_same(me)
        self.assertEqual((lay.F, lay.U, lay.has_material_index), (n, 2, True))

    def test_b1m(self):
        lay = self.assert_same(b1m_mesh())
        self.assertGreater(lay.C - 2 * lay.F, 990_000)

    def test_empty_and_vertex_only_meshes(self):
        empty = bpy.data.meshes.new("empty")
        self.assertEqual(tuple(self.assert_same(empty))[:3], (0, 0, 0))
        points = bpy.data.meshes.new("points")
        points.from_pydata([(0, 0, 0), (1, 2, 3), (4, 5, 6)], [], [])
        self.assertEqual(tuple(self.assert_same(points))[:3], (3, 0, 0))
    def test_positions_path(self):
        me, _ = golden_mesh()
        want = api_read(me)
        pos, nrm = np.empty_like(want["positions"]), np.empty_like(want["corner_normals"])
        mesh.fill_positions(me, pos, nrm)
        self.assertEqual(pos.tobytes(), want["positions"].tobytes())
        self.assertEqual(nrm.tobytes(), want["corner_normals"].tobytes())


class _El:
    def __init__(self, ptr, value):
        self._ptr, self.value = ptr, value

    def as_pointer(self):
        return self._ptr


class TestLayoutCheck(unittest.TestCase):
    """Each broken assumption raises LayoutMismatch naming the array and the Blender version."""

    def setUp(self):
        self.src = np.arange(10, dtype=np.int32) * 7
        self.base = self.src.ctypes.data

    def coll(self, stride=4, last_ptr=None, values=None):
        vals = self.src if values is None else values
        els = [_El(self.base + i * stride, int(vals[i])) for i in range(len(vals))]
        if last_ptr is not None:
            els[-1] = _El(last_ptr, int(vals[-1]))
        return els

    def check(self, coll, why):
        dst = np.zeros(10, np.int32)
        pattern = r"direct read of test array .*" + why + r".*Blender " + re.escape(bpy.app.version_string)
        with self.assertRaisesRegex(mesh.LayoutMismatch, pattern):
            mesh._copy(coll, dst, 1, lambda e: e.value, "test array")

    def test_good_layout_copies(self):
        dst = np.zeros(10, np.int32)
        mesh._copy(self.coll(), dst, 1, lambda e: e.value, "test array")
        np.testing.assert_array_equal(dst, self.src)

    def test_stride(self):
        self.check(self.coll(stride=8), "element stride 8 bytes, expected 4")

    def test_not_contiguous(self):
        self.check(self.coll(last_ptr=self.base + 4 * 20), "not one contiguous array")

    def test_value(self):
        wrong = self.src.copy()
        wrong[-1] += 1
        self.check(self.coll(values=wrong), "element 9 reads")

    def test_attribute_type(self):
        me = bpy.data.meshes.new("t")
        me.from_pydata([(0, 0, 0), (1, 0, 0), (0, 1, 0)], [], [(0, 1, 2)])
        me.attributes.new("UVMap", "FLOAT_VECTOR", "CORNER")  # a uv_layers entry would be FLOAT2
        with self.assertRaisesRegex(mesh.LayoutMismatch, "type FLOAT_VECTOR on CORNER, expected FLOAT2 on CORNER"):
            mesh._attribute(me, "UVMap", "FLOAT2", "CORNER")


if __name__ == "__main__":
    support.main()
