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
from ar_sync import cache as cache_mod, instances, keys  # noqa: E402


class FakeEdit:
    def __init__(self):
        self.calls = []

    def call(self, name, *args):
        self.calls.append((name, args))
        if name == "ar_edit_update_transforms":  # the index array lives only for the call
            self.indices = [args[1][i] for i in range(args[0])]


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

    def test_bulk_matrices_match_matrix_world(self):
        # The foreach_get matrices equal each instance's matrix_world, rotation and scale included.
        col = bpy.data.collections.new("c")
        bpy.context.scene.collection.children.link(col)
        x = make("X", collection=col, loc=(1, 2, 3))
        x.rotation_euler = (0.3, -0.7, 1.1)
        x.scale = (1.0, 2.0, 0.5)
        bpy.context.view_layer.layer_collection.children["c"].exclude = True
        e = bpy.data.objects.new("inst", None)
        e.instance_type = "COLLECTION"
        e.instance_collection = col
        e.location, e.rotation_euler = (10, 0, 0), (0.0, 0.4, 0.2)
        bpy.context.scene.collection.objects.link(e)
        a = make("A", loc=(-1, 0, 4))
        a.rotation_euler = (1.0, 0.5, -0.25)
        km, t = build(origin=(0.5, -1.0, 2.0))
        dg = support.depsgraph()
        want = {}
        for inst in dg.object_instances:
            if inst.object.type == "MESH":
                m = np.asarray(inst.matrix_world, np.float32)[:3, :].reshape(12).copy()
                m[[3, 7, 11]] -= np.float32([0.5, -1.0, 2.0])
                want[t.index_of[keys.object_identity(inst)]] = m
        self.assertEqual(sorted(want), list(range(t.n)))
        for i, m in want.items():
            self.assertEqual(t.xf[i].tobytes(), m.tobytes(), f"instance {i}")

    def test_plain_move_reads_only_the_moved_object(self):
        # A non-instance object's move comes from depsgraph.updates, never from a walk of all instances.
        objs = [make(f"O{i}", loc=(i, 0, 0)) for i in range(5)]
        _, t = build()
        mover = objs[3]
        before = t.xf.copy()
        moved_row = t.index_of[(mover.session_uid,)]

        class NoWalk:
            def __init__(self, dg):
                self.updates = dg.updates

            @property
            def object_instances(self):
                raise AssertionError("a plain move of a non-instance object walked object_instances")

        def move():
            mover.location.x = 7.5

        edit = FakeEdit()
        ok = support.in_update(move, lambda dg: instances.sync_transforms(edit, None, t, NoWalk(dg),
                                                                            [mover.session_uid]))
        self.assertTrue(ok)
        self.assertEqual([c[0] for c in edit.calls], ["ar_edit_update_transforms"])
        self.assertEqual(edit.indices, [moved_row])
        self.assertEqual(t.xf[moved_row][3], 7.5)
        others = [i for i in range(t.n) if i != moved_row]
        self.assertEqual(t.xf[others].tobytes(), before[others].tobytes())

    def test_move_of_an_object_that_is_also_instanced(self):
        # A has its own row and is also instanced through a collection: the move must reach both rows.
        col = bpy.data.collections.new("c")
        bpy.context.scene.collection.children.link(col)
        a = make("A", loc=(1, 0, 0), collection=col)
        e = bpy.data.objects.new("inst", None)
        e.instance_type = "COLLECTION"
        e.instance_collection = col
        e.location = (10, 0, 0)
        bpy.context.scene.collection.objects.link(e)
        _, t = build()
        self.assertEqual(sorted(t.xf[:, 3].tolist()), [1.0, 11.0])

        def move():
            a.location.x = 2.0

        edit = FakeEdit()
        ok = support.in_update(move, lambda dg: instances.sync_transforms(edit, None, t, dg, [a.session_uid]))
        self.assertTrue(ok)
        self.assertEqual(sorted(edit.indices), [0, 1])
        self.assertEqual(sorted(t.xf[:, 3].tolist()), [2.0, 12.0])

    def test_per_object_fields_follow_their_object(self):
        # bpy.data.objects is sorted by name; the instance loop meets "Z" first, so the bulk reads must be
        # gathered per object, not by position.
        z = make("Z", loc=(0, 0, 0))
        a = make("A", loc=(1, 0, 0))
        m = make("M", loc=(2, 0, 0))
        z.visible_camera, z.is_shadow_catcher, z.pass_index, z.color = False, True, 3, (0.1, 0.2, 0.3, 0.4)
        a.visible_diffuse, a.is_holdout, a.pass_index, a.color = False, True, 5, (0.5, 0.6, 0.7, 0.8)
        m.pass_index, m.color = 9, (0.9, 0.25, 0.125, 1.0)
        _, t = build()
        want = {z: (0x3F & ~instances.VIS_CAMERA, instances.FLAG_SHADOW_CATCHER, 3),
                a: (0x3F & ~instances.VIS_DIFFUSE, instances.FLAG_HOLDOUT, 5),
                m: (0x3F, 0, 9)}
        for ob, (vis, flags, pi) in want.items():
            i = t.index_of[(ob.session_uid,)]
            self.assertEqual((int(t.arrays["visibility"][i]), int(t.arrays["flags"][i]),
                              int(t.arrays["pass_indices"][i])), (vis, flags, pi), ob.name)
            np.testing.assert_array_equal(t.arrays["colors"][i], np.float32(tuple(ob.color)), ob.name)
    def test_instancer_move_walks_its_instances(self):
        col = bpy.data.collections.new("c")
        bpy.context.scene.collection.children.link(col)
        make("X", collection=col, loc=(1, 0, 0))
        make("Y", collection=col, loc=(0, 1, 0))
        bpy.context.view_layer.layer_collection.children["c"].exclude = True
        e = bpy.data.objects.new("inst", None)
        e.instance_type = "COLLECTION"
        e.instance_collection = col
        bpy.context.scene.collection.objects.link(e)
        make("Z", loc=(5, 5, 5))
        _, t = build()

        def move():
            e.location.x = 30.0

        edit = FakeEdit()
        ok = support.in_update(move, lambda dg: instances.sync_transforms(edit, None, t, dg, [e.session_uid]))
        self.assertTrue(ok)
        self.assertEqual(edit.calls[0][1][0], 2)
        self.assertEqual(sorted(t.xf[:, 3].tolist()), [5.0, 30.0, 31.0])

    def test_new_instance_needs_rebuild(self):
        a = make("A")
        _, t = build()
        make("B", a.data)
        edit = FakeEdit()
        b = bpy.data.objects["B"]
        self.assertFalse(instances.sync_transforms(edit, None, t, support.depsgraph(), [b.session_uid]))
        self.assertEqual(edit.calls, [])


class TestMurmur3Many(unittest.TestCase):
    def test_bit_identical_to_murmur3_32(self):
        names = ["", "a", "ab", "abc", "abcd", "abcde", "Cube", "Cube.001", "U0000 0", "é", "名前", "🌌 nebula",
                 "x" * 63, "long " * 60, "\0nul\0", "tab\tand\nnewline"] + [f"Object.{i:05d}" for i in range(50)]
        datas = [n.encode("utf-8") for n in names] + [bytes(range(256)), bytes([255]) * 7]
        got = instances.murmur3_32_many(datas)
        self.assertEqual(got.dtype, np.uint32)
        self.assertEqual(got.tolist(), [instances.murmur3_32(d) for d in datas])
        self.assertEqual(int(instances.murmur3_32_many([b"hello"])[0]), 0x248BFA47)  # 0008 decision 11


def updated(edit):
    """Original session_uids depsgraph.updates names after edit(), as classify's `evaluated` collects them."""
    return support.in_update(edit, lambda dg: {u.id.original.session_uid for u in dg.updates}) or set()


class TestInstanceCache(unittest.TestCase):
    """build() with a caller-owned InstanceCache equals a build without one after every kind of change, once
    the IDs the depsgraph names are forgotten."""

    def setUp(self):
        support.reset_scene()
        self.km, self.cache = keys.KeyMap(), instances.InstanceCache()

    def build(self):
        return instances.build(support.depsgraph(), self.km, cache=self.cache)

    def assert_fresh(self, t):
        fresh = instances.build(support.depsgraph(), self.km)
        self.assertEqual(t.n, fresh.n)
        for name, arr in fresh.arrays.items():
            self.assertEqual(t.arrays[name].tobytes(), arr.tobytes(), name)
        self.assertEqual(t.material_keys.tolist(), fresh.material_keys.tolist())

    def test_rename(self):
        a = make("A")
        self.build()
        self.cache.forget(updated(lambda: setattr(a, "name", "Renamed")))
        t = self.build()
        self.assertEqual(int(t.arrays["crypto_objects"][0]), instances.murmur3_32(b"Renamed"))
        self.assert_fresh(t)

    def test_relink_data(self):
        m1, m2 = bpy.data.materials.new("m1"), bpy.data.materials.new("m2")
        a = make("A")
        a.data.materials.append(m1)
        other = bpy.data.meshes.new("other")
        other.from_pydata([(0, 0, 0), (1, 0, 0), (0, 1, 0), (1, 1, 0)], [], [(0, 1, 2), (1, 3, 2)])
        other.materials.append(m2)
        other.materials.append(m1)
        before = self.build()
        self.cache.forget(updated(lambda: setattr(a, "data", other)))
        t = self.build()
        self.assertNotEqual(t.arrays["geom_keys"][0], before.arrays["geom_keys"][0])
        self.assertEqual(int(t.arrays["material_counts"][0]), 2)
        self.assert_fresh(t)

    def test_data_linked_slot_change(self):
        m1, m2 = bpy.data.materials.new("m1"), bpy.data.materials.new("m2")
        a = make("A")
        a.data.materials.append(m1)
        make("B", a.data)
        self.build()

        a.data.materials[0] = m2
        self.cache.forget({a.data.session_uid})  # only the mesh: both objects cached with it are dropped
        t = self.build()
        self.assertEqual(t.material_keys.tolist(), [self.km.key(("material", m2.session_uid))])
        self.assert_fresh(t)

    def test_modifier_added_splits_geometry(self):
        # Not forgotten on purpose: the real-data answer is keyed by the evaluated data pointer too, which a
        # modifier (or a geometry-nodes instance reusing one temporary object) changes.
        a = make("A")
        make("B", a.data)
        self.build()
        a.modifiers.new("w", "WAVE")
        t = self.build()
        self.assertNotEqual(t.arrays["geom_keys"][0], t.arrays["geom_keys"][1])
        self.assert_fresh(t)

    def test_new_object(self):
        a = make("A")
        self.build()
        self.cache.forget(updated(lambda: make("C", a.data, loc=(2, 0, 0))))
        t = self.build()
        self.assertEqual(t.n, 2)
        self.assert_fresh(t)

    def test_random_ids_follow_their_instances(self):
        col = bpy.data.collections.new("c")
        bpy.context.scene.collection.children.link(col)
        make("X", collection=col)
        bpy.context.view_layer.layer_collection.children["c"].exclude = True
        for loc in ((10, 0, 0), (20, 0, 0)):
            e = bpy.data.objects.new("inst", None)
            e.instance_type, e.instance_collection, e.location = "COLLECTION", col, loc
            bpy.context.scene.collection.objects.link(e)
        make("Z")
        t = self.build()
        dg = support.depsgraph()
        want = {t.index_of[keys.object_identity(i)]: i.random_id & 0xFFFFFFFF
                for i in dg.object_instances if i.object.type == "MESH"}
        self.assertEqual(sorted(want), list(range(t.n)))
        self.assertEqual([int(t.arrays["random_ids"][i]) for i in range(t.n)], [want[i] for i in range(t.n)])


class TestClassifyUpdates(unittest.TestCase):
    """The step the viewport and F12 take per batch: classify, then forget every ID its walk met."""

    @classmethod
    def setUpClass(cls):
        support.acquire_properties()  # classify's settings signature reads scene.astroray

    @classmethod
    def tearDownClass(cls):
        support.release_properties()

    def test_forgets_what_the_batch_names(self):
        support.reset_scene()
        m1, m2 = bpy.data.materials.new("m1"), bpy.data.materials.new("m2")
        a = make("A")
        a.data.materials.append(m1)
        a.material_slots[0].link = "OBJECT"
        a.material_slots[0].material = m1
        km, icache, sc = keys.KeyMap(), instances.InstanceCache(), cache_mod.SyncCache()
        instances.build(support.depsgraph(), km, cache=icache)
        sc.remember(support.depsgraph(), bpy.context.scene)

        def swap():
            a.material_slots[0].material = m2
        actions, evaluated = support.in_update(swap, lambda dg: instances.classify_updates(dg, sc, icache))
        self.assertIn(a.session_uid, evaluated)
        self.assertTrue(actions)
        t = instances.build(support.depsgraph(), km, cache=icache)
        self.assertEqual(t.material_keys.tolist(), [km.key(("material", m2.session_uid))])


class TestPlainMoveFromClassify(unittest.TestCase):
    def setUp(self):
        support.reset_scene()

    def test_evaluated_map_spares_the_updates_walk(self):
        objs = [make(f"O{i}", loc=(i, 0, 0)) for i in range(3)]
        _, t = build(origin=(0.5, 0.0, 0.0))
        mover = objs[1]

        class NoWalk:
            @property
            def updates(self):
                raise AssertionError("sync_transforms walked depsgraph.updates although it was given the objects")

            @property
            def object_instances(self):
                raise AssertionError("a plain move of a non-instance object walked object_instances")

        def move():
            mover.location.x = 7.5

        def patch(dg):
            ev = {u.id.original.session_uid: u.id for u in dg.updates}
            return instances.sync_transforms(edit, None, t, NoWalk(), {mover.session_uid: ev[mover.session_uid]},
                                             origin=(0.5, 0.0, 0.0))
        edit = FakeEdit()
        self.assertTrue(support.in_update(move, patch))
        row = t.index_of[(mover.session_uid,)]
        self.assertEqual(edit.indices, [row])
        self.assertEqual(t.xf[row][3], np.float32(7.5) - np.float32(0.5))


if __name__ == "__main__":
    support.main()
