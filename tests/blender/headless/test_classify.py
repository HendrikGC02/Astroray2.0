# SPDX-License-Identifier: Apache-2.0
"""0007 V4, headless part: one case per row of the d20 table that the bpy wheel can produce."""
import os
import sys
import unittest
from types import SimpleNamespace

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import support  # noqa: E402

support.load_sync()
from ar_sync import cache as cache_mod  # noqa: E402
from ar_sync import classify as C  # noqa: E402


def quad(name):
    me = bpy.data.meshes.new(name)
    me.from_pydata([(0, 0, 0), (1, 0, 0), (0, 1, 0)], [], [(0, 1, 2)])
    return me


class TestClassify(unittest.TestCase):
    def setUp(self):
        self.scene = support.reset_scene()
        sc = self.scene
        self.ob = bpy.data.objects.new("O", quad("O"))
        sc.collection.objects.link(self.ob)
        self.mat = bpy.data.materials.new("M")
        self.ob.data.materials.append(self.mat)
        self.light = bpy.data.objects.new("L", bpy.data.lights.new("L", "POINT"))
        sc.collection.objects.link(self.light)
        self.cam = bpy.data.objects.new("C", bpy.data.cameras.new("C"))
        sc.collection.objects.link(self.cam)
        sc.camera = self.cam
        sc.world = bpy.data.worlds.new("W")
        self.img = bpy.data.images.new("I", 4, 4)
        self.group = bpy.data.node_groups.new("G", "ShaderNodeTree")
        # Only datablocks the scene reaches are in the depsgraph: use both from the material.
        self.mat.use_nodes = True
        self.mat.node_tree.nodes.new("ShaderNodeTexImage").image = self.img
        self.mat.node_tree.nodes.new("ShaderNodeGroup").node_tree = self.group
        self.cache = cache_mod.SyncCache()
        dg = support.depsgraph()
        self.cache.remember(dg, sc)

    def kinds(self, edit):
        acts = support.in_update(edit, lambda dg: C.classify(dg, self.cache))
        return [(a.kind, a.id_type) for a in acts or ()]

    def only(self, edit, kind, id_type=None):
        got = self.kinds(edit)
        self.assertTrue(any(k == kind and (id_type is None or t == id_type) for k, t in got), got)
        return got

    def test_object_move_is_instance_patch(self):
        got = self.only(lambda: setattr(self.ob, "location", (1, 2, 3)), C.INSTANCES)
        self.assertNotIn(C.GEOMETRIES, [k for k, _ in got])

    def test_mesh_vertex_edit(self):
        def edit():
            self.ob.data.vertices[0].co = (0.5, 0, 0)
            self.ob.data.update()
        self.only(edit, C.GEOMETRIES)

    def test_material_edit(self):
        self.only(lambda: setattr(self.mat, "diffuse_color", (1, 0, 0, 1)), C.MATERIALS)

    def test_node_group_edit(self):
        self.group.nodes.new("ShaderNodeMath")
        self.only(lambda: self.group.update_tag(), C.MATERIALS, "NODETREE")

    def test_light_world_image_camera(self):
        self.only(lambda: setattr(self.light.data, "energy", 7.0), C.LIGHTS)
        self.only(lambda: setattr(self.scene.world, "color", (1, 0, 0)), C.WORLD)
        self.only(lambda: self.img.update_tag(), C.IMAGES, "IMAGE")
        self.only(lambda: setattr(self.cam.data, "lens", 85.0), C.CAMERA)

    def test_settings_change_and_frame_change(self):
        self.only(lambda: setattr(self.scene.render, "resolution_x", 321), C.SETTINGS)
        self.cache.remember(support.depsgraph(), self.scene)
        got = self.kinds(lambda: self.scene.frame_set(5))
        self.assertNotIn(C.SETTINGS, [k for k, _ in got], "frame number is not a settings change")

    def test_shading_tag_with_unchanged_slots_is_ignored(self):
        got = self.kinds(lambda: setattr(self.ob, "color", (1, 0, 0, 1)))
        self.assertEqual([k for k, _ in got if k in (C.INSTANCES, C.GEOMETRIES, C.MATERIALS)], [])

    def test_slot_change_rebuilds_instance_material_table(self):
        self.only(lambda: self.ob.data.materials.append(bpy.data.materials.new("M2")), C.INSTANCES)

    def test_add_object_and_collection_instance(self):
        def add():
            self.scene.collection.objects.link(bpy.data.objects.new("O2", quad("O2")))
        self.only(add, C.INSTANCES)

        def inst():
            col = bpy.data.collections.new("col")
            e = bpy.data.objects.new("E", None)
            e.instance_type = "COLLECTION"
            e.instance_collection = col
            self.scene.collection.objects.link(e)
        self.only(inst, C.INSTANCES)

    def test_shape_key_playback_arrives_as_object_geometry(self):
        self.ob.shape_key_add(name="Basis")
        k = self.ob.shape_key_add(name="K")
        self.cache.remember(support.depsgraph(), self.scene)
        self.only(lambda: setattr(k, "value", 0.5), C.GEOMETRIES)

    def test_order_is_fixed(self):
        def edit():
            self.cam.data.lens = 30.0
            self.mat.diffuse_color = (0, 1, 0, 1)
            self.scene.world.color = (0, 0, 1)
            self.light.data.energy = 3.0
            self.ob.location = (4, 0, 0)
        got = [k for k, _ in self.kinds(edit)]
        self.assertEqual(got, sorted(got))
        self.assertTrue({C.WORLD, C.MATERIALS, C.LIGHTS, C.INSTANCES, C.CAMERA} <= set(got), got)

    def test_one_action_per_datablock(self):
        def edit():
            self.ob.location = (1, 0, 0)
            self.ob.rotation_euler = (0, 0, 1)
            self.ob.data.vertices[0].co = (0.1, 0, 0)
            self.ob.data.update()
        acts = support.in_update(edit, lambda dg: C.classify(dg, self.cache))
        self.assertEqual(len(acts), len({(a.kind, a.id_type, a.uid) for a in acts}))

    def test_unlisted_type_full_resync(self):
        # FONT (the vector font) is in no row of 0007 d20; the wheel cannot tag one on its own.
        font = SimpleNamespace(original=SimpleNamespace(id_type="FONT", session_uid=9))
        upd = SimpleNamespace(id=font, is_updated_transform=False, is_updated_geometry=True,
                              is_updated_shading=False)
        dg = SimpleNamespace(updates=[upd], id_type_updated=lambda t: False, object_instances=[])
        acts = C.classify(dg, self.cache)
        self.assertEqual([a.kind for a in acts], [C.FULL_RESYNC])
        self.assertEqual(self.cache.diagnostics["unlisted_id_type"], 1)

    def test_ignored_types_produce_no_action(self):
        for t in ("ARMATURE", "KEY", "TEXT", "SCREEN", "GREASEPENCIL_V3", "WORKSPACE"):
            ident = SimpleNamespace(original=SimpleNamespace(id_type=t, session_uid=3))
            upd = SimpleNamespace(id=ident, is_updated_transform=True, is_updated_geometry=True,
                                  is_updated_shading=True)
            dg = SimpleNamespace(updates=[upd], id_type_updated=lambda t: False, object_instances=[])
            self.assertEqual(C.classify(dg, self.cache), [], t)
        self.assertEqual(self.cache.diagnostics["unlisted_id_type"], 0)


if __name__ == "__main__":
    support.main()
