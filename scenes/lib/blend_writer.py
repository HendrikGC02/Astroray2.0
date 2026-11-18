# SPDX-License-Identifier: Apache-2.0
"""Builds a .blend from the JSON description a scenes/lib/ar.py builder wrote (0009 d19).

Run inside Blender: `Invoke-ArBlender -Script scenes/lib/blend_writer.py -Args <desc.json> <out.blend>`,
or import `build(desc, path)` from a script that already runs there (tests/blender/smoke_a1.py).
The Astroray extension must be enabled: the sampling settings live in `scene.astroray`.
One writer serves every builder, so the addon renders what the builder recorded. It covers what the
M1a addon translates: triangle meshes with per-corner normals and UVs, Principled BSDF materials with
constant inputs, point and rectangle/ellipse area lamps, a constant world colour and a pinhole camera.
Anything else in a description raises, never guesses (CLAUDE.md rule 5). Tasks 089 and 090 extend it.
Ref: blender/astroray/materials/translate.py _principled (the input mapping, reversed here: Specular
IOR Level is half the OpenPBR specular_weight, 0005 item 42), blender/astroray/sync/lights.py
(lamp parameters), blender/astroray/sync/camera.py (sensor fit AUTO, as ar.projection_inverse).
"""
import base64
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ar import ALL_VISIBILITY  # noqa: E402  (every ray type, the Blender default)

# OpenPBR record input -> (Principled input name, scale into it).
_PRINCIPLED = {"base_color": ("Base Color", 1.0), "specular_roughness": ("Roughness", 1.0),
               "specular_weight": ("Specular IOR Level", 0.5)}
# Inputs that exist only at their Blender default; any other value cannot be written.
_FIXED = {"base_weight": 1.0, "base_metalness": 0.0, "base_diffuse_roughness": 0.0}
_PASS_FLAGS = {"Depth": "use_pass_z", "Position": "use_pass_position", "Normal": "use_pass_normal",
               "UV": "use_pass_uv", "IndexOB": "use_pass_object_index", "IndexMA": "use_pass_material_index",
               "Mist": "use_pass_mist", "Vector": "use_pass_vector"}
_FILTERS = {"box": "BOX", "gaussian": "GAUSSIAN", "blackman_harris": "BLACKMAN_HARRIS"}


class BlendWriterError(ValueError):
    """A description entry the writer cannot apply; never skipped (CLAUDE.md rule 5)."""


def _array(d):
    return np.frombuffer(base64.b64decode(d["b64"]), dtype=d["dtype"]).reshape(d["shape"]).copy()


def _matrix(xf12):
    from mathutils import Matrix
    return Matrix([xf12[0:4], xf12[4:8], xf12[8:12], [0.0, 0.0, 0.0, 1.0]])


def _mesh(bpy, d):
    pos, cv, fs = _array(d["positions"]), _array(d["corner_vert"]), _array(d["face_start"])
    me = bpy.data.meshes.new(d["name"])
    me.vertices.add(len(pos))
    me.vertices.foreach_set("co", pos.reshape(-1))
    me.loops.add(len(cv))
    me.loops.foreach_set("vertex_index", cv)
    me.polygons.add(len(fs))
    me.polygons.foreach_set("loop_start", fs)
    me.polygons.foreach_set("loop_total", np.diff(np.append(fs, len(cv))).astype(np.int32))
    me.update(calc_edges=True)
    me.validate(verbose=True)
    if d["uv"] is not None:
        me.uv_layers.new(name="UVMap").data.foreach_set("uv", _array(d["uv"]).reshape(-1))
    me.normals_split_custom_set(_array(d["normals"]))  # the builder's own corner normals
    return me


def _material(bpy, d):
    mat = bpy.data.materials.new(f"material {d['key']}")
    mat.use_nodes = True
    tree = mat.node_tree
    tree.nodes.remove(next(n for n in tree.nodes if n.type == "BSDF_PRINCIPLED"))
    node = tree.nodes.new("ShaderNodeBsdfPrincipled")
    tree.links.new(node.outputs["BSDF"], next(n for n in tree.nodes if n.type == "OUTPUT_MATERIAL").inputs["Surface"])
    for name, value in d["inputs"].items():
        if name in _FIXED:
            if value != _FIXED[name]:
                raise BlendWriterError(f"material {d['key']}: {name} = {value} has no Principled BSDF equivalent")
            continue
        if name not in _PRINCIPLED:
            raise BlendWriterError(f"material {d['key']}: input '{name}' is not covered by the blend writer")
        socket, scale = _PRINCIPLED[name]
        node.inputs[socket].default_value = (*value, 1.0) if isinstance(value, list) else value * scale
    if d["emission"] is not None:
        node.inputs["Emission Color"].default_value = (*d["emission"]["rgb"], 1.0)
        node.inputs["Emission Strength"].default_value = d["emission"]["strength"]
    return mat


def _light(bpy, d):
    kinds = {"point": ("POINT", None), "rect": ("AREA", "RECTANGLE"), "ellipse": ("AREA", "ELLIPSE")}
    if d["kind"] not in kinds:
        raise BlendWriterError(f"lamp type '{d['kind']}' is not covered by the blend writer")
    typ, shape = kinds[d["kind"]]
    light = bpy.data.lights.new(f"light {d['kind']}", typ)
    light.color, light.energy = d["rgb"], d["power"]
    light.use_shadow, light.normalize = bool(d["cast_shadow"]), bool(d["normalize"])
    if typ == "POINT":
        light.shadow_soft_size = d["size"][0]
    else:
        light.shape, light.size, light.size_y, light.spread = shape, d["size"][0], d["size"][1], d["spread"]
    ob = bpy.data.objects.new(light.name, light)
    ob.matrix_world = _matrix(d["xf"])
    return ob


def build(desc, path):
    """Writes `path` from the description (a dict, or the path of its JSON file)."""
    import bpy

    if isinstance(desc, str):
        with open(desc, encoding="utf-8") as f:
            desc = json.load(f)
    for k in ("camera", "settings", "world"):
        if desc[k] is None:
            raise BlendWriterError(f"the description has no {k}")
    # Not read_factory_settings: it would disable the extension this scene is written for.
    for coll in (bpy.data.objects, bpy.data.meshes, bpy.data.materials, bpy.data.lights, bpy.data.cameras,
                 bpy.data.worlds):
        for datablock in list(coll):
            coll.remove(datablock)
    scene = bpy.context.scene
    if not hasattr(scene, "astroray"):
        raise RuntimeError("the Astroray extension is not enabled: scene.astroray is missing")
    st = desc["settings"]
    if st["sampler"] != "sobol":
        # Blender has no sampler setting and the addon always samples in Sobol mode.
        raise BlendWriterError(f"sampler '{st['sampler']}' cannot be set in a .blend; build the description "
                               "with sampler='sobol' (a1_furnace/build.py --sampler sobol)")
    scene.render.engine = "ASTRORAY"
    scene.render.resolution_x, scene.render.resolution_y, scene.render.resolution_percentage = st["width"], st["height"], 100
    a = scene.astroray
    a.samples = a.viewport_samples = st["spp"]
    a.seed, a.max_bounces, a.min_bounces = st["seed"], st["max_bounces"], st["min_bounces"]
    a.pixel_filter_type, a.pixel_filter_width = _FILTERS[st["pixel_filter"]], st["filter_width"]
    for name in st["passes"]:
        setattr(bpy.context.view_layer, _PASS_FLAGS[name], True)

    meshes = {m["key"]: _mesh(bpy, m) for m in desc["meshes"]}
    materials = {m["key"]: _material(bpy, m) for m in desc["materials"]}
    slots = {}
    for i, inst in enumerate(desc["instances"]):
        if inst["geom"] not in meshes:
            raise BlendWriterError(f"instance {i}: geometry key {inst['geom']} is not a recorded mesh")
        missing = [mk for mk in inst["materials"] if mk not in materials]
        if missing:
            raise BlendWriterError(f"instance {i}: material keys {missing} were not made by Scene.material() "
                                   "(material_from_graph has no .blend equivalent)")
        if inst["visibility"] != ALL_VISIBILITY:
            raise BlendWriterError(f"instance {i}: visibility {inst['visibility']:#x} is not the default "
                                   f"{ALL_VISIBILITY:#x}; the writer sets no ray visibility")
        me = meshes[inst["geom"]]
        if me.name in slots and slots[me.name] != inst["materials"]:
            raise BlendWriterError("two instances of one mesh with different material lists need object-linked slots")
        if me.name not in slots:
            slots[me.name] = inst["materials"]
            for mk in inst["materials"]:
                me.materials.append(materials[mk])
        ob = bpy.data.objects.new(f"{me.name} {i}", me)
        ob.matrix_world = _matrix(inst["xf"])
        scene.collection.objects.link(ob)
    for d in desc["lights"]:
        scene.collection.objects.link(_light(bpy, d))

    w = desc["world"]
    world = bpy.data.worlds.new("World")
    world.use_nodes = True
    bg = world.node_tree.nodes["Background"]
    bg.inputs["Color"].default_value = (*w["rgb"], 1.0)
    bg.inputs["Strength"].default_value = w["strength"]
    scene.world = world

    c = desc["camera"]
    cam = bpy.data.cameras.new("Camera")
    cam.lens, cam.sensor_width, cam.sensor_fit = c["focal_mm"], c["sensor_mm"], "AUTO"
    cam.clip_start, cam.clip_end = c["near"], c["far"]
    cob = bpy.data.objects.new("Camera", cam)
    cob.matrix_world = _matrix(c["cam_to_world"])
    scene.collection.objects.link(cob)
    scene.camera = cob
    bpy.ops.wm.save_as_mainfile(filepath=path)
    return path


if __name__ == "__main__":
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if len(args) != 2:
        raise SystemExit("usage: blender --background --python blend_writer.py -- <desc.json> <out.blend>")
    build(args[0], args[1])
    print(f"blend_writer: wrote {args[1]}")
