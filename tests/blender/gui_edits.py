# SPDX-License-Identifier: Apache-2.0
"""The viewport edit matrix (task 111, 0007 V4 GUI part) in a windowed Blender, A1 in Rendered shading.

Run through `Invoke-ArBlender -Gui -Script tests/blender/gui_edits.py` (the land canary adds `-k <case>`).
One timer-driven sequence applies every edit class of 0007 d20's table to a live viewport session, one after
the other, on a scene with an object for each. For each edit it records the viewport job's lifecycle events
(`full` or `delta <action classes>`), the native ar_edit_* calls the job made (a counting spy over its
library) and the frame log; the cases then read what was recorded and print one RESULT line per class.
The sampler is counter-based, so pixels an edit does not touch are identical before and after: "the frame
differs" is a count of changed down-sampled pixels, and an edit that changes nothing must leave it at zero.
A step that fails records its error for its own edit and the run goes on. A watchdog ends the run.
Ref: docs/decisions/0007-blender-integration.md V4, d20 (classification), d25 (generations);
blender/astroray/viewport.py (events, frame log); tests/blender/gui_present.py (the driving pattern).
"""
import collections
import importlib
import os
import subprocess
import sys
import time
import traceback
import unittest

import numpy as np

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
for sub in ("tools", os.path.join("scenes", "lib")):
    sys.path.insert(0, os.path.join(ROOT, sub))
import blend_writer  # noqa: E402
import smoke_a1  # noqa: E402  (the zip install and the argument helpers)

MODULE = smoke_a1.MODULE
ARGS = smoke_a1.ARGS
WATCHDOG_S = 270.0   # land's canary run gives Blender 300 s
STEP_TIMEOUT_S = 12.0
INERT_WAIT_S = 1.5   # an edit that must send nothing is given this long to (wrongly) send something
CHANGED = 1e-3       # a down-sampled pixel counts as changed above this difference
T0 = time.monotonic()
STATE = {"errors": {}, "edits": {}, "meshes": []}


def finish(code):
    print(f"gui_edits exit {code}", flush=True)
    os._exit(code)  # skips Blender's window teardown; shutdown() already destroyed the contexts


class CallSpy:
    """Stands in for a job's ctypes library and counts the edit calls made through it (Edit.call and the
    edit wrapper's own begin and commit go through the job's `lib`)."""

    def __init__(self, lib):
        self._lib, self.counts = lib, collections.Counter()

    def __getattr__(self, name):
        fn = getattr(self._lib, name)
        if not name.startswith(("ar_edit_", "ar_scene_edit_")) or name == "ar_edit_last_error":
            return fn

        def call(*a):
            self.counts[name] += 1
            return fn(*a)
        return call


# ---- the scene: one object per edit class, all inside A1's camera view (x in +-1.4, z in +-0.8 at y = 0)

def material(name, colour):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    mat.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (*colour, 1.0)
    return mat


def mesh_from(name, verts, faces, uvs=None):
    me = bpy.data.meshes.new(name)
    me.from_pydata(verts, [], faces)
    if uvs:
        me.uv_layers.new(name="UVMap").data.foreach_set("uv", uvs)
    return me


def cube_mesh(name, s, z0=None, z1=None):
    z0, z1 = (-s, s) if z0 is None else (z0, z1)
    v = [(x, y, z) for z in (z0, z1) for y in (-s, s) for x in (-s, s)]
    return mesh_from(name, v, [(0, 1, 3, 2), (4, 5, 7, 6), (0, 1, 5, 4), (2, 3, 7, 6), (0, 2, 6, 4), (1, 3, 7, 5)])


def put(name, me, loc, mat=None, collection=None):
    if mat is not None:
        me.materials.append(mat)
    ob = bpy.data.objects.new(name, me)
    ob.location = loc
    (collection or bpy.context.scene.collection).objects.link(ob)
    return ob


def build_scene(scene):
    """A1's sphere, camera and world, plus the objects the edits act on."""
    D = bpy.data
    sphere = next(o for o in scene.objects if o.type == "MESH")
    sphere.name, sphere.scale, sphere.location = "Mover", (0.2, 0.2, 0.2), (-1.1, 0.0, 0.5)
    put("MatCube", cube_mesh("MatCube", 0.15), (-1.1, 0, -0.1), material("MatM", (0.5, 0.5, 0.5)))
    put("VertCube", cube_mesh("VertCube", 0.15), (-0.6, 0, -0.1), material("VertM", (0.5, 0.5, 0.5)))
    # A node group holding a Principled BSDF, used as a material's surface.
    ng = D.node_groups.new("Grp", "ShaderNodeTree")
    ng.interface.new_socket("BSDF", in_out="OUTPUT", socket_type="NodeSocketShader")
    pr, out = ng.nodes.new("ShaderNodeBsdfPrincipled"), ng.nodes.new("NodeGroupOutput")
    pr.name = "GroupBSDF"
    pr.inputs["Base Color"].default_value = (0.2, 0.2, 0.8, 1.0)
    ng.links.new(pr.outputs[0], out.inputs[0])
    gm = material("GroupM", (0.5, 0.5, 0.5))
    gn = gm.node_tree.nodes.new("ShaderNodeGroup")
    gn.node_tree = ng
    gm.node_tree.links.new(gn.outputs[0], gm.node_tree.nodes["Material Output"].inputs["Surface"])
    put("GroupCube", cube_mesh("GroupCube", 0.15), (-0.1, 0, -0.1), gm)
    # A file-backed image on a plane (generated images wait for the engine's PIXELS support).
    path = os.path.join(ROOT, "out", "smoke", "edit_tex.png")
    gen = D.images.new("EditTexGen", 8, 8)
    gen.filepath_raw, gen.file_format = path, "PNG"
    STATE["gen_image"] = gen
    set_image_colour(gen, 0.15)
    img = D.images.load(path)
    im = D.materials.new("ImageM")
    im.use_nodes = True
    tex = im.node_tree.nodes.new("ShaderNodeTexImage")
    tex.image = img
    im.node_tree.links.new(tex.outputs["Color"], im.node_tree.nodes["Principled BSDF"].inputs["Base Color"])
    put("ImagePlane", mesh_from("ImagePlane", [(-.2, 0, -.2), (.2, 0, -.2), (.2, 0, .2), (-.2, 0, .2)], [(0, 1, 2, 3)],
                                (0, 0, 1, 0, 1, 1, 0, 1)), (0.5, 0, -0.1), im)
    lamp = put("Lamp", D.lights.new("Lamp", "POINT"), (0.5, -0.8, -0.1))
    lamp.data.energy = 50.0
    # An instancer: a triangle whose vertices carry the child (instance_type VERTS); the parent is not rendered.
    tri = put("Inst", mesh_from("InstTri", [(-.15, 0, 0), (.15, 0, 0), (0, 0, .25)], [(0, 1, 2)]), (-0.6, 0, 0.45))
    tri.instance_type, tri.show_instancer_for_viewport, tri.show_instancer_for_render = "VERTS", False, False
    child = put("InstChild", cube_mesh("InstChild", 0.06), (0, 0, 0), material("InstM", (0.8, 0.3, 0.1)))
    child.parent = tri
    # Instance on Points (as scenes/lib/blend_writer.py builds it): the instanced mesh is the template's, not the host's.
    tmpl = D.objects.new("GnTemplate", mesh_from("GnTemplate", [(s, 0, 0) for s in (-.06, .06)] + [(0, s, 0) for s in (-.06, .06)]
                                                 + [(0, 0, s) for s in (-.06, .06)],
                                                 [(0, 2, 4), (0, 4, 3), (0, 3, 5), (0, 5, 2), (1, 4, 2), (1, 3, 4), (1, 5, 3), (1, 2, 5)]))
    tmpl.data.materials.append(material("GnM", (0.6, 0.2, 0.7)))
    src = D.collections.new("GnSrc")
    scene.collection.children.link(src)
    src.objects.link(tmpl)
    hv = [(-0.2, 0, -0.5), (0.0, 0, -0.5), (0.2, 0, -0.5)]
    gng = D.node_groups.new("GnPoints", "GeometryNodeTree")
    gng.interface.new_socket("Geometry", in_out="INPUT", socket_type="NodeSocketGeometry")
    gng.interface.new_socket("Geometry", in_out="OUTPUT", socket_type="NodeSocketGeometry")
    gin, gout = gng.nodes.new("NodeGroupInput"), gng.nodes.new("NodeGroupOutput")
    iop, info = gng.nodes.new("GeometryNodeInstanceOnPoints"), gng.nodes.new("GeometryNodeObjectInfo")
    info.inputs["Object"].default_value = tmpl
    info.transform_space = "RELATIVE"
    info.inputs["As Instance"].default_value = True
    gng.links.new(gin.outputs[0], iop.inputs["Points"])
    gng.links.new(info.outputs["Geometry"], iop.inputs["Instance"])
    gng.links.new(iop.outputs["Instances"], gout.inputs[0])
    put("GnHost", mesh_from("GnHost", hv, []), (0, 0, 0)).modifiers.new("GN", "NODES").node_group = gng
    # A bar skinned to one bone, bent by the animation.
    arm_data = D.armatures.new("Arm")
    arm = put("Arm", arm_data, (0, 0, 0))
    bar = put("ArmMesh", cube_mesh("ArmMesh", 0.04, 0.3, 0.6), (0, 0, 0.0), material("ArmM", (0.2, 0.7, 0.3)))
    vg = bar.vertex_groups.new(name="Bone")
    vg.add(list(range(8)), 1.0, "REPLACE")
    bar.modifiers.new("Arm", "ARMATURE").object = arm
    vl = bpy.context.view_layer
    vl.update()  # the new objects join the view layer; only then can one be made active
    vl.objects.active = arm
    bpy.ops.object.mode_set(mode="EDIT")
    bone = arm_data.edit_bones.new("Bone")
    bone.head, bone.tail = (0, 0, 0.3), (0, 0, 0.6)
    bpy.ops.object.mode_set(mode="POSE")
    pb = arm.pose.bones["Bone"]
    pb.rotation_mode = "XYZ"
    pb.keyframe_insert("rotation_euler", frame=1)
    pb.rotation_euler = (0.0, 1.2, 0.0)
    pb.keyframe_insert("rotation_euler", frame=20)
    bpy.ops.object.mode_set(mode="OBJECT")
    scene.frame_set(1)
    # An object of its own in a collection that can be excluded.
    exc = D.collections.new("ExcColl")
    scene.collection.children.link(exc)
    put("ExcCube", cube_mesh("ExcCube", 0.12), (0.6, 0, 0.5), material("ExcM", (0.8, 0.8, 0.1)), exc)
    # The collection a later instancer empty will instance; excluded, so it renders only through the empty.
    ic = D.collections.new("InstColl")
    scene.collection.children.link(ic)
    put("InstCollCube", cube_mesh("InstCollCube", 0.1), (0, 0, 0), material("ICM", (0.1, 0.5, 0.8)), ic)
    vl.layer_collection.children["InstColl"].exclude = True
    vl.layer_collection.children["GnSrc"].exclude = True
    vl.objects.active = None


def set_image_colour(gen, v):
    px = np.ones((8, 8, 4), np.float32)
    px[:, :, :3] = v
    gen.pixels.foreach_set(px.reshape(-1))
    gen.save()


# ---- the edits

def obj(name):
    return bpy.data.objects[name]


def e_move():
    obj("Mover").location.x += 0.3


def e_move_again():
    obj("Mover").location.z -= 0.2


def e_instancer_move():
    obj("Inst").location.x += 0.2


def e_material():
    bpy.data.materials["MatM"].node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (0.9, 0.1, 0.1, 1)


def e_vertex():
    bpy.data.meshes["VertCube"].vertices[0].co.z += 0.25


def e_add_object():
    put("Added", cube_mesh("Added", 0.12), (1.1, 0, -0.1), material("AddedM", (0.9, 0.5, 0.0)))


def e_delete_object():
    bpy.data.objects.remove(obj("Added"))


def e_collection_instance():
    e = bpy.data.objects.new("CollInst", None)
    e.instance_type, e.instance_collection = "COLLECTION", bpy.data.collections["InstColl"]
    e.location = (1.1, 0, 0.5)
    bpy.context.scene.collection.objects.link(e)


def e_exclude(on):
    def apply():
        bpy.context.view_layer.layer_collection.children["ExcColl"].exclude = on
    return apply


def e_armature():
    bpy.context.scene.frame_set(12)


def e_selection():
    obj("MatCube").select_set(True)


def e_node_group():
    bpy.data.node_groups["Grp"].nodes["GroupBSDF"].inputs["Base Color"].default_value = (0.9, 0.9, 0.1, 1.0)


def e_image():
    set_image_colour(STATE["gen_image"], 0.85)
    bpy.data.images["edit_tex.png"].reload()
    bpy.data.images["edit_tex.png"].update_tag()


def e_world():
    bpy.context.scene.world.node_tree.nodes["Background"].inputs["Strength"].default_value = 0.4


def e_light():
    bpy.data.lights["Lamp"].energy = 400.0


def e_rename():
    obj("MatCube").name = "MatCubeRenamed"


def e_unlisted():
    """Blender 5.3 tags no ID type that d20's table lacks (a vector font is never reported), so the table is
    trimmed in this process: Camera leaves it, and a lens change is an unlisted ID type's update."""
    cl = importlib.import_module(MODULE + ".sync.classify")
    cl._BY_TYPE = {k: v for k, v in cl._BY_TYPE.items() if k != "CAMERA"}
    bpy.context.scene.camera.data.lens += 5.0


def e_addon_setting():
    bpy.context.scene.astroray.seed += 1


def e_viewport_samples():
    bpy.context.scene.astroray.viewport_samples += 4


# name, apply, the action classes of the delta event ("full": a full resync; None: no event), the native calls
# the edit makes (exactly these, besides begin and commit), whether the image changes (None: not asserted).
# Blender tags more than an edit names (every collection change touches the lamp and the posed bar, a node
# edit touches the mesh of the objects that use it), so the classes and counts are what the sync does with
# those tags: a change to them is a change of cost, and this table is where it shows.
EDITS = [
    # move_first: ours, an inefficiency: viewport._after_full leaves `table` None, so the first move after a full
    # sync re-sends the whole instance table instead of patching transforms.
    ("move_first", e_move, "instances", {"ar_edit_set_instances": 1}, True),
    ("move", e_move_again, "instances", {"ar_edit_update_transforms": 1}, True),
    ("instancer_move", e_instancer_move, "instances", {"ar_edit_update_transforms": 1}, True),
    # Over-sends below (reported to the lead): Blender's extra tag = Blender tags objects the edit did not name;
    # ours = the classifier or the sync turns a tag into work it should not.
    # material: Blender's extra tag (the object using the material gets the geometry bit), so its mesh is re-read.
    ("material", e_material, "materials,geometry", {"ar_edit_set_graph": 1, "ar_edit_set_material": 1, "ar_edit_mesh_begin": 1}, True),
    ("vertex", e_vertex, "geometry", {"ar_edit_mesh_begin": 1}, True),
    # add_object, collection_instance, delete_object, exclude, include: the lamp is re-sent (ours:
    # classify._object_kinds returns LIGHTS for any LIGHT object update, bits or not).
    ("add_object", e_add_object, "materials,lights,geometry,instances",
     {"ar_edit_set_instances": 1, "ar_edit_set_graph": 1, "ar_edit_set_material": 1, "ar_edit_mesh_begin": 1, "ar_edit_set_light": 1}, True),
    ("delete_object", e_delete_object, "lights,instances", {"ar_edit_set_instances": 1, "ar_edit_set_light": 1, "ar_edit_remove": 1}, True),
    ("collection_instance", e_collection_instance, "materials,lights,geometry,instances",
     {"ar_edit_set_instances": 1, "ar_edit_set_graph": 1, "ar_edit_set_material": 1, "ar_edit_mesh_begin": 1, "ar_edit_set_light": 1}, True),
    ("exclude_collection", e_exclude(True), "lights,instances", {"ar_edit_set_instances": 1, "ar_edit_set_light": 1, "ar_edit_remove": 1}, True),
    # include_collection also re-reads the posed bar: Blender's extra tag (Arm re-evaluates with the collection).
    ("include_collection", e_exclude(False), "materials,lights,geometry,instances",  # the cube and the re-posed bar
     {"ar_edit_set_instances": 1, "ar_edit_set_graph": 1, "ar_edit_set_material": 1, "ar_edit_mesh_begin": 2, "ar_edit_set_light": 1}, True),
    ("armature_playback", e_armature, "geometry", {"ar_edit_mesh_begin": 1}, True),
    ("selection", e_selection, None, {}, False),
    # node_group, image: the mesh of the user is re-read (Blender's extra tag, as for the material).
    ("node_group", e_node_group, "materials,geometry", {"ar_edit_set_graph": 1, "ar_edit_set_material": 1, "ar_edit_mesh_begin": 1}, True),
    ("image", e_image, "images,materials,geometry",
     {"ar_edit_set_image": 1, "ar_edit_set_graph": 1, "ar_edit_set_material": 1, "ar_edit_mesh_begin": 1}, True),
    # world, light: the owning node tree is classed MATERIALS (ours: classify maps every NODETREE to MATERIALS); no call follows.
    ("world", e_world, "world,materials", {"ar_edit_set_world": 1, "ar_edit_set_graph": 1}, True),
    ("light", e_light, "materials,lights", {"ar_edit_set_light": 1}, True),
    ("rename", e_rename, None, {}, False),
    # addon_setting, viewport_samples: the lamp (ours, as above) and the whole instance table are re-sent (cause not
    # isolated: the property callback's Scene tag marks the Scene and every object).
    ("addon_setting", e_addon_setting, "settings,lights,instances",
     {"ar_edit_set_settings": 1, "ar_edit_set_light": 1, "ar_edit_set_instances": 1}, True),
    ("viewport_samples", e_viewport_samples, "settings,lights,instances",
     {"ar_edit_set_settings": 1, "ar_edit_set_light": 1, "ar_edit_set_instances": 1}, None),
    ("unlisted_type", e_unlisted, "full", None, None),  # a full resync re-sends everything: its counts are not the point
]
NAMES = [e[0] for e in EDITS]


class Driver:
    """The timer-driven sequence; each step returns the next timer interval or None when finished."""

    def __init__(self, rf):
        self.rf, self.job, self.step_name, self.step_t0 = rf, None, "setup", time.monotonic()
        self.todo = list(EDITS)
        self.cur = None

    def find_area(self):
        for win in bpy.context.window_manager.windows:
            for area in win.screen.areas:
                if area.type == "VIEW_3D":
                    return win, area, next(r for r in area.regions if r.type == "WINDOW")
        raise RuntimeError("no 3D viewport")

    def go(self, name):
        self.step_name, self.step_t0 = name, time.monotonic()

    def timed_out(self, limit=STEP_TIMEOUT_S):
        return time.monotonic() - self.step_t0 > limit

    def tick(self):
        try:
            if time.monotonic() - T0 > WATCHDOG_S:
                STATE["errors"]["watchdog"] = f"watchdog: still in step {self.step_name}"
                print(STATE["errors"]["watchdog"], flush=True)
                return self.end()
            return getattr(self, "step_" + self.step_name)()
        except Exception:
            what = self.cur["name"] if self.cur else "setup"
            STATE["errors"][what] = traceback.format_exc()
            print(f"edit {what} failed:\n{STATE['errors'][what]}", flush=True)
            if what == "setup" or (self.job and self.job.failure):
                return self.end()
            self.cur = None
            self.go("next")
            return 0.1

    # ---- steps

    def step_setup(self):
        mm = importlib.import_module(MODULE + ".sync.mesh")
        orig = mm.sync_mesh

        def logged(*a, **k):
            lay = orig(*a, **k)
            STATE["meshes"].append((a[4], lay.V))  # (the object's name, vertex count) of every mesh sent
            return lay
        mm.sync_mesh = logged
        build_scene(bpy.context.scene)  # here, not at load: the mode switches of the armature need a window context
        win, area, region = self.find_area()
        space = area.spaces.active
        space.shading.type = "RENDERED"
        space.overlay.show_overlays = False
        space.show_gizmo = False
        for name in ("show_region_header", "show_region_toolbar", "show_region_ui", "show_region_tool_header",
                     "show_region_hud"):
            if hasattr(space, name):
                setattr(space, name, False)
        space.region_3d.view_perspective = "CAMERA"
        self.go("first_frame")
        return 0.2

    def find_job(self):
        held = self.rf._contexts.get("CPU")
        return next((j for j in (held.jobs if held else ()) if hasattr(j, "frames")), None)

    def step_first_frame(self):
        self.job = self.job or self.find_job()
        if self.job is None:
            if self.timed_out():
                raise RuntimeError("no viewport job appeared")
            return 0.2
        if self.job.frames is None:
            self.job.frames = []
            self.spy = CallSpy(self.job.lib)
            self.job.lib = self.spy
        if not self.job.frames:
            if self.timed_out():
                raise RuntimeError(f"no frame presented; events {self.job.events}, failure {self.job.failure}")
            return 0.1
        self.go("settle")
        return 0.1

    def progress(self):
        import ctypes
        prog = self.job.abi.ar_progress()
        self.job.lib.ar_session_get_progress(self.job.session, ctypes.byref(prog))
        return prog

    def settled(self):
        """The newest generation is active, its samples are done and its last frame is on screen."""
        p = self.progress()
        return (p.state == self.job.abi.AR_STATE_CONVERGED and p.active_generation == p.pending_generation
                and self.job.frames and self.job.frames[-1]["generation"] >= p.active_generation)

    def step_settle(self):
        # Early passes are not normalised yet, so a frame counts as the pre-edit one once its samples are done.
        if not self.settled():
            if self.timed_out(40.0):
                raise RuntimeError("the viewport never converged")
            return 0.1
        STATE["first_meshes"] = list(STATE["meshes"])  # what the first full sync sent, before any edit
        self.go("next")
        return 0.4

    def screenshot(self, name):
        win, area, region = self.find_area()
        out = os.path.join(smoke_a1.arg("--evidence", os.path.join(ROOT, "out", "evidence", "111")), name)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with bpy.context.temp_override(window=win, area=area, region=region):
            bpy.ops.screen.screenshot_area(filepath=out)

    def step_next(self):
        if self.cur is None and len(self.todo) == len(EDITS):
            self.screenshot("edits_initial.png")
        if self.cur is None and self.todo and self.todo[0][0] in ("image", "world"):
            self.screenshot("before_" + self.todo[0][0] + ".png")
        if not self.todo:
            return self.end()
        name, apply, kinds, calls, image = self.todo.pop(0)
        job = self.job
        self.cur = {"name": name, "kinds": kinds, "calls": calls, "image": image, "pre": job.frames[-1],
                    "events": len(job.events), "counts": collections.Counter(self.spy.counts),
                    "active": self.progress().active_generation, "geoms": len(job.geom_keys)}
        apply()
        self.go("event")
        return 0.1

    def step_event(self):
        c, job = self.cur, self.job
        if c["kinds"] is None:  # the edit must send nothing: wait, then see that it did not
            if not self.timed_out(INERT_WAIT_S):
                return 0.1
            return self.record()
        if len(job.events) == c["events"]:
            if self.timed_out():
                raise RuntimeError(f"no sync event after the edit; events {job.events[-4:]}")
            return 0.1
        self.go("converge")
        return 0.1

    def step_converge(self):
        c = self.cur
        if self.progress().active_generation == c["active"] or not self.settled():
            if self.timed_out(25.0):
                raise RuntimeError(f"the edit never settled; events {self.job.events[c['events']:]}")
            return 0.1
        self.go("record")
        return 0.4

    def step_record(self):
        return self.record()

    def record(self):
        c, job = self.cur, self.job
        pre, post = c["pre"], job.frames[-1]
        diff = np.abs(post["rgb"] - pre["rgb"]).max(axis=2)
        added = collections.Counter(self.spy.counts)
        added.subtract(c["counts"])
        if c["name"] in ("node_group", "image"):
            self.screenshot("after_" + c["name"] + ".png")
        r = {"events": [e for e in job.events[c["events"]:] if e == "full" or e.startswith("delta")], "calls": {k: v for k, v in added.items() if v},
             "changed": int((diff > CHANGED).sum()), "of": int(diff.size), "geoms": (c["geoms"], len(job.geom_keys)),
             "generations": (pre["generation"], post["generation"])}
        STATE["edits"][c["name"]] = r
        print(f"RESULT edit {c['name']}: {r['events']} calls {r['calls']} geometries {r['geoms'][0]}->{r['geoms'][1]} "
              f"changed pixels {r['changed']}/{r['of']} generation {r['generations'][0]}->{r['generations'][1]}",
              flush=True)
        self.cur = None
        self.go("next")
        return 0.1

    def end(self):
        STATE["seconds"] = time.monotonic() - T0
        STATE["full_events"] = self.job.events.count("full") if self.job else None
        try:
            self.rf.shutdown()  # a leftover job is closed here, in order
        except Exception:
            STATE["errors"]["shutdown"] = traceback.format_exc()
            print(STATE["errors"]["shutdown"], flush=True)
        finally:
            run_cases()  # ends the process, whatever happened above
        return None


def spec(name):
    return next(e for e in EDITS if e[0] == name)


class TestEdits(unittest.TestCase):
    def setUp(self):
        if "shutdown" in STATE["errors"]:
            self.fail("shutdown failed:\n" + STATE["errors"]["shutdown"])

    def test_instanced_geometry_is_the_instanced_mesh(self):
        """Instance on Points: the mesh sent for the instances is the template's octahedron (6 vertices), under the
        template object's name, and the host's own evaluated mesh is empty (its three vertices became instances); the scan did not
        hold a dead temporary object for the instances."""
        if "setup" in STATE["errors"]:
            self.fail(STATE["errors"]["setup"])
        print(f"RESULT meshes sent by the first sync: {STATE['meshes'][:12]}", flush=True)
        self.assertIn(("GnTemplate", 6), STATE["first_meshes"])
        self.assertIn(("GnHost", 0), STATE["first_meshes"])
        self.assertEqual(sum(1 for m in STATE["first_meshes"] if m == ("GnTemplate", 6)), 1)

    def edit(self, name):
        """What the run recorded for one edit, or the failure that kept it from being recorded."""
        if "watchdog" in STATE["errors"] and name not in STATE["edits"]:
            self.fail(STATE["errors"]["watchdog"])
        if name in STATE["errors"]:
            self.fail(f"edit {name} did not complete:\n" + STATE["errors"][name])
        if name not in STATE["edits"]:
            self.fail(f"edit {name} never ran")
        return STATE["edits"][name]

    def check_calls(self, name):
        """The edit's action classes and exact native calls (calls not named must be absent)."""
        r, (_, _, kinds, calls, _) = self.edit(name), spec(name)
        self.assertEqual(r["events"], [] if kinds is None else ["full" if kinds == "full" else "delta " + kinds],
                         f"{name}: events")
        if calls is not None:
            self.assertEqual({k: v for k, v in r["calls"].items() if k not in COMMIT_CALLS}, calls, f"{name}: calls")
        return r

    def test_move_patches_transforms_only(self):
        """A plain move and the move of an instancer send one ar_edit_update_transforms and no instance table,
        mesh or material (0007 d20: an Object transform bit alone patches the transforms)."""
        for name in ("move", "instancer_move"):
            r = self.check_calls(name)
            self.assertEqual(r["geoms"][0], r["geoms"][1])

    def test_geometry_set_follows_the_scene(self):
        """Geometry edits resend only what changed, and the geometry set shrinks as well as grows: adding an
        object adds a mesh, deleting it (and excluding its collection) sends ar_edit_remove (task 085's finding)."""
        self.check_calls("vertex")
        self.check_calls("armature_playback")
        a = self.check_calls("add_object")
        self.assertEqual(a["geoms"][1], a["geoms"][0] + 1)
        d = self.check_calls("delete_object")
        self.assertEqual(d["geoms"][1], d["geoms"][0] - 1)
        self.check_calls("collection_instance")
        x = self.check_calls("exclude_collection")
        self.assertEqual(x["geoms"][1], x["geoms"][0] - 1)
        i = self.check_calls("include_collection")
        self.assertEqual(i["geoms"][1], i["geoms"][0] + 1)

    def test_data_edits_use_their_own_setter(self):
        """Material, node group, image, world, light and an addon setting each reach the session through the
        setter of their kind (the addon setting through ar_edit_set_settings, which scene.astroray never did)."""
        for name in ("material", "node_group", "image", "world", "light", "addon_setting", "viewport_samples"):
            self.check_calls(name)

    def test_inert_edits_send_nothing(self):
        """Selection and a rename change no image: no action, no native edit call, no new frame."""
        for name in ("selection", "rename"):
            r = self.check_calls(name)
            self.assertEqual(r["generations"][0], r["generations"][1], f"{name}: a new frame was presented")

    def test_only_the_unlisted_type_resyncs_fully(self):
        """Every row of the table is a delta; the one full resync of the run is the unlisted ID type's."""
        self.edit("unlisted_type")
        self.assertEqual(STATE["full_events"], 2, "the first sync plus the unlisted type's")
        for name in NAMES[:-1]:
            if name in STATE["edits"]:
                self.assertNotIn("full", STATE["edits"][name]["events"], name)
        self.assertEqual(self.edit("unlisted_type")["events"], ["full"])

    def test_every_edit_changes_the_frame(self):
        """The frame presented after an edit differs from the one before it (a count of changed pixels of the
        down-sampled frame log), and a newer generation was presented, for every edit that changes the image."""
        for name, _, _, _, image in EDITS:
            if image is not True:
                continue
            r = self.edit(name)
            self.assertGreater(r["changed"], 0, f"{name}: the frame did not change")
            self.assertGreater(r["generations"][1], r["generations"][0], f"{name}: no newer generation")


# The edit wrapper's own begin and commit calls and the per-item commits: every edit makes them.
COMMIT_CALLS = {"ar_scene_edit_begin", "ar_edit_commit", "ar_edit_item_commit"}


def run_cases():
    loader = unittest.defaultTestLoader
    if "-k" in ARGS:  # land's canary runs one named case
        pat = ARGS[ARGS.index("-k") + 1]
        loader.testNamePatterns = [pat if "*" in pat else f"*{pat}*"]
    res = unittest.TextTestRunner(stream=sys.stdout, verbosity=2).run(loader.loadTestsFromTestCase(TestEdits))
    finish(0 if res.wasSuccessful() else 1)


def main():
    work = smoke_a1.arg("--work", os.path.join(ROOT, "out", "smoke"))
    os.makedirs(work, exist_ok=True)
    blend = os.path.join(work, "a1_edits.blend")
    p = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "scenes", "a1_furnace", "build.py"),
                        "--out", os.path.join(work, "a1_edits.arrec"), "--blend", blend, "--sampler", "sobol"],
                       capture_output=True, text=True, timeout=300)
    if p.returncode != 0:
        raise RuntimeError("scenes/a1_furnace/build.py failed: " + p.stdout + p.stderr)
    smoke_a1.install_and_enable()
    rf = importlib.import_module(MODULE + ".render_final")
    blend_writer.build(blend + ".json", blend)
    bpy.ops.wm.open_mainfile(filepath=blend, load_ui=False)
    scene = bpy.context.scene
    scene.render.engine = "ASTRORAY"
    scene.astroray.device = "CPU"
    scene.astroray.viewport_samples = 16
    scene.camera.data.passepartout_alpha = 0.0
    bpy.app.timers.register(Driver(rf).tick, first_interval=1.0)


if __name__ == "__main__":
    try:
        main()
    except Exception:
        STATE["errors"]["setup"] = traceback.format_exc()
        print(STATE["errors"]["setup"], flush=True)
        run_cases()
