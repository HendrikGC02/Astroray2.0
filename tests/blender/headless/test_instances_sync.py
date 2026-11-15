# SPDX-License-Identifier: Apache-2.0
"""Instance arrays from depsgraph.object_instances (0007 d19)."""
import os
import sys
import unittest

import numpy as np

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import support  # noqa: E402

support.load_sync()
from ar_sync import instances, keys  # noqa: E402


class FakeEdit:
    def __init__(self):
        self.calls = []

    def call(self, name, *args):
        self.calls.append((name, args))


def make(name, mesh=None, loc=(0, 0, 0), collection=None):
    mesh = mesh or bpy.data.meshes.new(name)
    if not mesh.vertices:
        mesh.from_pydata([(0, 0, 0), (1, 0, 0), (0, 1, 0)], [], [(0, 1, 2)])
    ob = bpy.data.objects.new(name, mesh)
    (collection or bpy.context.scene.collection).objects.link(ob)
    ob.location = loc
    return ob


def build(origin=(0.0, 0.0, 0.0)):
    km = keys.KeyMap()
    dg = support.depsgraph()
    return km, instances.build(dg, km, origin, instances.lightgroup_indices(bpy.context.view_layer))


class TestInstances(unittest.TestCase):
    def setUp(self):
        support.reset_scene()

    def test_murmur3_vector(self):
        self.assertEqual(instances.murmur3_32(b"hello"), 0x248BFA47)  # 0008 decision 11

    def test_linked_duplicates(self):
        a = make("A", loc=(1, 2, 3))
        make("B", a.data, loc=(4, 0, 0))
        _, t = build()
        self.assertEqual(t.n, 2)
        g, o = t.arrays["geom_keys"], t.arrays["object_keys"]
        self.assertEqual(g[0], g[1])
        self.assertNotEqual(o[0], o[1])
        self.assertEqual(sorted(t.xf[:, 3].tolist()), [1.0, 4.0])
        self.assertEqual(t.xf.dtype, np.float32)
        self.assertEqual(t.arrays["visibility"][0], 0x3F)

    def test_origin_folded_into_translation(self):
        make("A", loc=(10, 20, 30))
        _, t = build(origin=(1.0, 2.0, 3.0))
        np.testing.assert_allclose(t.xf[0][[3, 7, 11]], [9, 18, 27])

    def test_collection_instance(self):
        col = bpy.data.collections.new("c")
        bpy.context.scene.collection.children.link(col)
        make("X", collection=col, loc=(1, 0, 0))
        make("Y", collection=col, loc=(0, 1, 0))
        # Hide the source collection from the view layer so only the instances render.
        bpy.context.view_layer.layer_collection.children["c"].exclude = True
        for loc in ((10, 0, 0), (20, 0, 0)):
            e = bpy.data.objects.new("inst", None)
            e.instance_type = "COLLECTION"
            e.instance_collection = col
            e.location = loc
            bpy.context.scene.collection.objects.link(e)
        _, t = build()
        self.assertEqual(t.n, 4)
        self.assertEqual(len(set(t.arrays["object_keys"].tolist())), 4,
                         "same persistent_id under two parents is two instances")
        self.assertEqual(len(set(t.arrays["geom_keys"].tolist())), 2)
        self.assertEqual(sorted(t.xf[:, 3].tolist()), [10.0, 11.0, 20.0, 21.0])

    def test_visibility_flags_light_group_and_pass_index(self):
        bpy.context.view_layer.lightgroups.add(name="lgA")
        bpy.context.view_layer.lightgroups.add(name="lgB")
        a = make("A")
        a.visible_glossy = False
        a.visible_shadow = False
        a.is_holdout = True
        a.lightgroup = "lgB"
        a.pass_index = 7
        _, t = build()
        self.assertEqual(t.arrays["visibility"][0], 0x3F & ~(instances.VIS_GLOSSY | instances.VIS_SHADOW))
        self.assertEqual(t.arrays["flags"][0], instances.FLAG_HOLDOUT)
        self.assertEqual(t.arrays["light_groups"][0], 2)
        self.assertEqual(t.arrays["pass_indices"][0], 7)

    def test_material_tables_are_shared(self):
        m1, m2 = bpy.data.materials.new("m1"), bpy.data.materials.new("m2")
        a = make("A")
        a.data.materials.append(m1)
        a.data.materials.append(m2)
        b = make("B", a.data)
        c = make("C")
        _, t = build()
        off, cnt = t.arrays["material_offsets"], t.arrays["material_counts"]
        self.assertEqual(sorted(cnt.tolist()), [0, 2, 2])
        self.assertEqual(len(t.material_keys), 2)
        self.assertEqual(len({int(o) for o, n in zip(off, cnt) if n == 2}), 1)
        self.assertEqual(len(set(t.material_keys.tolist())), 2)
        self.assertEqual((b.name, c.name), ("B", "C"))

    def test_move_only_updates_transforms(self):
        a = make("A", loc=(1, 0, 0))
        make("B", a.data, loc=(5, 0, 0))
        _, t = build()
        a.location.x = 3.0
        dg = support.depsgraph()
        edit = FakeEdit()
        self.assertTrue(instances.sync_transforms(edit, None, t, dg, [a.session_uid]))
        self.assertEqual([c[0] for c in edit.calls], ["ar_edit_update_transforms"])
        n = edit.calls[0][1][0]
        self.assertEqual(n, 1)
        moved = [i for i in range(t.n) if t.xf[i][3] == 3.0]
        self.assertEqual(len(moved), 1)
        self.assertEqual(sorted(t.xf[:, 3].tolist()), [3.0, 5.0])

    def test_new_instance_needs_rebuild(self):
        a = make("A")
        _, t = build()
        make("B", a.data)
        edit = FakeEdit()
        b = bpy.data.objects["B"]
        self.assertFalse(instances.sync_transforms(edit, None, t, support.depsgraph(), [b.session_uid]))
        self.assertEqual(edit.calls, [])


if __name__ == "__main__":
    support.main()
