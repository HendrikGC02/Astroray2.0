# SPDX-License-Identifier: Apache-2.0
"""Builds a .blend from the JSON description a scenes/lib/ar.py builder wrote (0009 d19).

Run inside Blender: `Invoke-ArBlender -Script scenes/lib/blend_writer.py -Args <desc.json> <out.blend>`,
or import `build(desc, path)` from a script that already runs there (tests/blender/smoke_a1.py).
The Astroray extension must be enabled: the sampling settings live in `scene.astroray`.
One writer serves every builder, so the addon renders what the builder recorded. It covers what the
M1a addon translates: triangle meshes with per-corner normals and UVs, Principled BSDF materials with
constant inputs or a base colour from a file-backed sRGB Image Texture (a material entry's
"base_color_image"), point and rectangle/ellipse area lamps, a constant world colour and a pinhole camera.
Instances come in three kinds so the addon's sync sees what a Blender user makes (B-5M): "object"
(an object per instance, meshes shared between objects are linked duplicates), "collection" (an empty
instancing a collection that holds the mesh, the collection excluded from the view layer) and
"points" (a geometry-nodes modifier, Instance on Points, over a vertex-only host mesh; translations
only). Anything else in a description raises, never guesses (CLAUDE.md rule 5).
`--no-extension` writes a scene for Cycles (engine Cycles, `scene.cycles.samples`) when the Astroray
extension is not installed in the Blender that runs the writer, as for the Cycles comparisons.
Ref: blender/astroray/materials/translate.py _principled (the input mapping, reversed here: Specular
IOR Level is half the OpenPBR specular_weight, 0005 item 42), blender/astroray/sync/lights.py
(lamp parameters), blender/astroray/sync/camera.py (sensor fit AUTO, as ar.projection_inverse).
"""
import base64
import json
import os
import sys
import zlib

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
    raw = base64.b64decode(d["b64"])
    return np.frombuffer(zlib.decompress(raw) if d.get("z") else raw, dtype=d["dtype"]).reshape(d["shape"]).copy()


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


_IMAGE_KEYS = {"path", "colorspace", "interpolation", "extension"}
_INTERPOLATION = ("Linear", "Closest")
_EXTENSION = ("REPEAT", "EXTEND")


def _image_texture(bpy, tree, d, images, what):
    """An Image Texture node over a file-backed sRGB image, flat projection, its Vector input open
    (the default UV map, as translate.py _tex_image reads it). One image datablock per path: the
    engine refuses generated images until M1b, so the file stays the source."""
    if set(d) != _IMAGE_KEYS:
        raise BlendWriterError(f"{what}: an image entry has keys {sorted(d)}, wants {sorted(_IMAGE_KEYS)}")
    if d["colorspace"] != "sRGB":
        raise BlendWriterError(f"{what}: image colour space {d['colorspace']!r}; the writer sets sRGB only")
    if d["interpolation"] not in _INTERPOLATION or d["extension"] not in _EXTENSION:
        raise BlendWriterError(f"{what}: interpolation {d['interpolation']!r} / extension {d['extension']!r} "
                               f"are not in {_INTERPOLATION} / {_EXTENSION}")
    if not os.path.isfile(d["path"]):
        raise BlendWriterError(f"{what}: image file {d['path']} does not exist")
    if d["path"] not in images:
        img = bpy.data.images.load(d["path"], check_existing=False)
        img.colorspace_settings.name = "sRGB"
        img.alpha_mode = "STRAIGHT"
        images[d["path"]] = img
    node = tree.nodes.new("ShaderNodeTexImage")
    node.image = images[d["path"]]
    node.interpolation, node.extension, node.projection = d["interpolation"], d["extension"], "FLAT"
    return node


def _grating(mat, g, what):
    """Sets material.astroray from a Scene.grating_material entry: the Material panel's kind and rows."""
    if (g["mode"], g["profile"]) != ("reflect", "sinusoidal"):
        raise BlendWriterError(f"{what}: grating mode {g['mode']!r} / profile {g['profile']!r}: the Material panel "
                               "offers only reflect and sinusoidal")
    props = getattr(mat, "astroray", None)
    if props is None:
        raise BlendWriterError(f"{what}: the Astroray add-on is not registered, so material.astroray does not exist")
    props.ext_kind = "GRATING"
    props.grating_pitch_nm, props.grating_depth_nm = g["pitch_nm"], g["depth_nm"]
    props.grating_coating_n, props.grating_coating_k = g["coating"]
    props.grating_m_min, props.grating_m_max = g["m_min"], g["m_max"]


def _material(bpy, d, images):
    mat = bpy.data.materials.new(f"material {d['key']}")
    mat.use_nodes = True
    tree = mat.node_tree
    tree.nodes.remove(next(n for n in tree.nodes if n.type == "BSDF_PRINCIPLED"))
    node = tree.nodes.new("ShaderNodeBsdfPrincipled")
    tree.links.new(node.outputs["BSDF"], next(n for n in tree.nodes if n.type == "OUTPUT_MATERIAL").inputs["Surface"])
    image = d.get("base_color_image")
    if image is not None:
        if "base_color" in d["inputs"]:
            raise BlendWriterError(f"material {d['key']}: base_color is both a constant and an image")
        tex = _image_texture(bpy, tree, image, images, f"material {d['key']}")
        tree.links.new(tex.outputs["Color"], node.inputs["Base Color"])
    for name, value in d["inputs"].items():
        if name in _FIXED:
            if value != _FIXED[name]:
                raise BlendWriterError(f"material {d['key']}: {name} = {value} has no Principled BSDF equivalent")
            continue
        if name not in _PRINCIPLED:
            raise BlendWriterError(f"material {d['key']}: input '{name}' is not covered by the blend writer")
        socket, scale = _PRINCIPLED[name]
        node.inputs[socket].default_value = (*value, 1.0) if isinstance(value, list) else value * scale
    grating = d.get("grating")
    if grating is not None:
        _grating(mat, grating, f"material {d['key']}")
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
    # ar.Scene.light() records every ray type; a lamp object made by bpy.data.objects.new is
    # invisible to the camera by default, which would hide the Cornell box's lamp.
    for attr in ("visible_camera", "visible_diffuse", "visible_glossy", "visible_transmission", "visible_shadow",
                 "visible_volume_scatter"):
        setattr(ob, attr, True)
    return ob


def _same_source(insts, what):
    """Instances of one group share a source: one mesh and one material list."""
    first = insts[0]
    for i in insts:
        if i["geom"] != first["geom"] or i["materials"] != first["materials"]:
            raise BlendWriterError(f"{what}: the instances of one group must share one mesh and material list")
    return first


def _hide(bpy, scene, coll):
    """Links `coll` to the scene and excludes it from the view layer, so only its instances render."""
    scene.collection.children.link(coll)
    bpy.context.view_layer.layer_collection.children[coll.name].exclude = True


def _collection_instances(bpy, scene, group, insts, meshes):
    me = meshes[_same_source(insts, f"collection group {group}")["geom"]]
    coll = bpy.data.collections.new(f"instance source {group}")
    coll.objects.link(bpy.data.objects.new(f"{me.name} source {group}", me))  # at the collection origin
    _hide(bpy, scene, coll)
    for k, inst in enumerate(insts):
        e = bpy.data.objects.new(f"instance {group} {k}", None)
        e.instance_type, e.instance_collection = "COLLECTION", coll
        e.matrix_world = _matrix(inst["xf"])
        scene.collection.objects.link(e)


def _point_instances(bpy, scene, group, insts, meshes):
    """Instance on Points over a mesh of loose vertices, one host per group (identity transform,
    so each instance sits at its vertex)."""
    me = meshes[_same_source(insts, f"points group {group}")["geom"]]
    pts = np.array([i["xf"] for i in insts], np.float64).reshape(-1, 3, 4)
    if not np.allclose(pts[:, :, :3], np.eye(3), atol=1e-6):
        raise BlendWriterError(f"points group {group}: Instance on Points here carries translations only")
    host_mesh = bpy.data.meshes.new(f"points host {group}")
    host_mesh.vertices.add(len(pts))
    host_mesh.vertices.foreach_set("co", pts[:, :, 3].astype(np.float32).reshape(-1))
    host_mesh.update()
    tmpl = bpy.data.objects.new(f"{me.name} template {group}", me)
    coll = bpy.data.collections.new(f"points source {group}")
    coll.objects.link(tmpl)
    _hide(bpy, scene, coll)
    ng = bpy.data.node_groups.new(f"points {group}", "GeometryNodeTree")
    ng.interface.new_socket("Geometry", in_out="INPUT", socket_type="NodeSocketGeometry")
    ng.interface.new_socket("Geometry", in_out="OUTPUT", socket_type="NodeSocketGeometry")
    gin, gout = ng.nodes.new("NodeGroupInput"), ng.nodes.new("NodeGroupOutput")
    iop, info = ng.nodes.new("GeometryNodeInstanceOnPoints"), ng.nodes.new("GeometryNodeObjectInfo")
    info.inputs["Object"].default_value = tmpl
    info.transform_space = "RELATIVE"
    info.inputs["As Instance"].default_value = True
    ng.links.new(gin.outputs[0], iop.inputs["Points"])
    ng.links.new(info.outputs["Geometry"], iop.inputs["Instance"])
    ng.links.new(iop.outputs["Instances"], gout.inputs[0])
    host = bpy.data.objects.new(f"points host {group}", host_mesh)
    host.modifiers.new("GN", "NODES").node_group = ng
    scene.collection.objects.link(host)


def build(desc, path, extension=True):
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
    for coll in (bpy.data.collections, bpy.data.node_groups, bpy.data.images):
        for datablock in list(coll):
            coll.remove(datablock)
    scene = bpy.context.scene
    if extension and not hasattr(scene, "astroray"):
        raise RuntimeError("the Astroray extension is not enabled: scene.astroray is missing "
                           "(pass --no-extension to write a Cycles scene)")
    st = desc["settings"]
    if st["sampler"] != "sobol":
        # Blender has no sampler setting and the addon always samples in Sobol mode.
        raise BlendWriterError(f"sampler '{st['sampler']}' cannot be set in a .blend; build the description "
                               "with sampler='sobol' (a1_furnace/build.py --sampler sobol)")
    scene.render.resolution_x, scene.render.resolution_y, scene.render.resolution_percentage = st["width"], st["height"], 100
    if extension:
        scene.render.engine = "ASTRORAY"
        a = scene.astroray
        a.samples = a.viewport_samples = st["spp"]
        a.seed, a.max_bounces, a.min_bounces = st["seed"], st["max_bounces"], st["min_bounces"]
        a.pixel_filter_type, a.pixel_filter_width = _FILTERS[st["pixel_filter"]], st["filter_width"]
    else:
        scene.render.engine = "CYCLES"
        cy = scene.cycles
        cy.samples, cy.seed = st["spp"], st["seed"]
        # max_bounces is the total limit (0004); Cycles also caps each lobe type, and the smallest
        # applies, so every cap is set to the same number to make max_bounces the limit that is used.
        cy.max_bounces = cy.diffuse_bounces = cy.glossy_bounces = st["max_bounces"]
        cy.transmission_bounces = cy.volume_bounces = cy.transparent_max_bounces = st["max_bounces"]
        cy.min_light_bounces = cy.min_transparent_bounces = st["min_bounces"]
        cy.pixel_filter_type, cy.filter_width = _FILTERS[st["pixel_filter"]], st["filter_width"]
    for name in st["passes"]:
        if name not in _PASS_FLAGS:
            raise BlendWriterError(f"film pass '{name}' has no view-layer pass in the blend writer")
        setattr(bpy.context.view_layer, _PASS_FLAGS[name], True)

    meshes = {m["key"]: _mesh(bpy, m) for m in desc["meshes"]}
    images = {}
    materials = {m["key"]: _material(bpy, m, images) for m in desc["materials"]}
    slots, groups = {}, {}
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
        kind, group = inst.get("kind", "object"), inst.get("group")
        if kind not in ("object", "collection", "points"):
            raise BlendWriterError(f"instance {i}: kind {kind!r} is not 'object', 'collection' or 'points'")
        if (kind == "object") != (group is None) or (group is not None and (isinstance(group, bool) or not isinstance(group, int))):
            raise BlendWriterError(f"instance {i}: kind {kind!r} with group {group!r}: the collection and "
                                   "points kinds need an integer group, the object kind none")
        if kind != "object":
            groups.setdefault((kind, inst["group"]), []).append(inst)
            continue
        ob = bpy.data.objects.new(f"{me.name} {i}", me)
        ob.matrix_world = _matrix(inst["xf"])
        scene.collection.objects.link(ob)
    for (kind, group), insts in groups.items():
        if kind == "collection":
            _collection_instances(bpy, scene, group, insts, meshes)
        else:
            _point_instances(bpy, scene, group, insts, meshes)
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
    flags = [a for a in args if a.startswith("--")]
    args = [a for a in args if not a.startswith("--")]
    if len(args) != 2 or set(flags) - {"--no-extension"}:
        raise SystemExit("usage: blender --background --python blend_writer.py -- [--no-extension] <desc.json> <out.blend>")
    build(args[0], args[1], extension="--no-extension" not in flags)
    print(f"blend_writer: wrote {args[1]}")
