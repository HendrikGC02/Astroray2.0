# SPDX-License-Identifier: Apache-2.0
"""0007 V5: keys follow Cycles' real-object-data rule (d18)."""
import os
import sys
import unittest

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import support  # noqa: E402

support.load_sync()
from ar_sync import keys  # noqa: E402


def make(name, mesh=None):
    mesh = mesh or bpy.data.meshes.new(name)
    if not mesh.vertices:
        mesh.from_pydata([(0, 0, 0), (1, 0, 0), (0, 1, 0)], [], [(0, 1, 2)])
    ob = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(ob)
    return ob


def keyed(km, dg, pick):
    """(object name, geometry key, object key) per picked instance; instances die with the loop."""
    out = []
    for i in dg.object_instances:
        if pick(i):
            out.append((i.object.name, km.key(keys.geometry_identity(i, dg)),
                        km.key(keys.object_identity(i))))
    return out


class TestKeys(unittest.TestCase):
    def setUp(self):
        support.reset_scene()
        self.km = keys.KeyMap()

    def meshes(self):
        rows = keyed(self.km, support.depsgraph(), lambda i: i.object.type == "MESH")
        return {n: (g, o) for n, g, o in rows}

    def test_linked_duplicates_share_geometry(self):
        a = make("A")
        make("B", a.data)
        m = self.meshes()
        self.assertEqual(m["A"][0], m["B"][0])
        self.assertNotEqual(m["A"][1], m["B"][1])

    def test_modifier_splits_geometry(self):
        a = make("A")
        b = make("B", a.data)
        c = make("C", a.data)
        b.modifiers.new("s", "SUBSURF")
        c.modifiers.new("s", "SUBSURF")
        m = self.meshes()
        self.assertEqual(len({m[n][0] for n in "ABC"}), 3,
                         "shared mesh with differing modifiers is three geometries")

    def test_nested_collection_instances_distinct(self):
        scene = bpy.context.scene
        inner = bpy.data.collections.new("inner")
        outer = bpy.data.collections.new("outer")
        me = bpy.data.meshes.new("M")
        me.from_pydata([(0, 0, 0), (1, 0, 0), (0, 1, 0)], [], [(0, 1, 2)])
        inner.objects.link(bpy.data.objects.new("M", me))
        link = bpy.data.objects.new("link", None)
        link.instance_type = "COLLECTION"
        link.instance_collection = inner
        outer.objects.link(link)
        for n in range(2):
            top = bpy.data.objects.new(f"top{n}", None)
            top.instance_type = "COLLECTION"
            top.instance_collection = outer
            scene.collection.objects.link(top)
        rows = keyed(self.km, support.depsgraph(), lambda i: i.is_instance and i.object.name == "M")
        self.assertEqual(len(rows), 2)
        self.assertEqual(len({o for _, _, o in rows}), 2, "object keys distinct")
        self.assertEqual(len({g for _, g, _ in rows}), 1, "one shared geometry")

    def test_keymap_counter(self):
        k = self.km.key(("x", 1))
        self.assertGreater(k, 0)
        self.assertEqual(self.km.key(("x", 1)), k)
        self.assertNotEqual(self.km.key(("x", 2)), k)


if __name__ == "__main__":
    support.main()
