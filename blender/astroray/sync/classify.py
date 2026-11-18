# SPDX-License-Identifier: GPL-3.0-or-later
"""Depsgraph update classification (0007 d20).

Ref: Cycles intern/cycles/blender/sync.cpp, BlenderSync::sync_recalc (Apache-2.0,
projects.blender.org/blender/blender, read 2026-11-15): it reacts to a few ID types and
relies on Blender tagging dependent objects, so Material, Light, Mesh, World, Scene, Volume and
Camera entries are handled by type and an Object entry by its transform, geometry and shading
bits. Unlike Cycles, every ID type has an explicit row here and an unlisted one resyncs fully.
"""
from collections import namedtuple

# Update bits of one datablock.
TRANSFORM, GEOMETRY, SHADING = 1, 2, 4

# Action kinds, numbered in the fixed apply order of 0007 d20; FULL_RESYNC supersedes the rest.
(FULL_RESYNC, SETTINGS, WORLD, IMAGES, MATERIALS, LIGHTS, GEOMETRIES, INSTANCES, CAMERA) = range(9)
KIND_NAMES = ("full_resync", "settings", "world", "images", "materials", "lights", "geometry",
              "instances", "camera")

# One action per (kind, ID type, original session_uid); mask is the OR of the update bits.
Action = namedtuple("Action", "kind id_type uid mask")

# ID type (bpy.types.ID.id_type) -> action kind, for the rows that need no context.
_BY_TYPE = {
    "WORLD": WORLD,
    "IMAGE": IMAGES,
    "MATERIAL": MATERIALS,
    "NODETREE": MATERIALS,  # material, world, light trees and groups: their owners are re-inlined
    "LIGHT": LIGHTS,
    "CAMERA": CAMERA,
    "COLLECTION": INSTANCES,
    "MESH": GEOMETRIES, "CURVE": GEOMETRIES, "CURVES": GEOMETRIES, "POINTCLOUD": GEOMETRIES,
    "VOLUME": GEOMETRIES, "META": GEOMETRIES,
}
# Their effect arrives as Object transform or geometry entries, which Blender tags; or the type
# is not scene data. TEXT is the script datablock (text objects use CURVE). FONT, the vector
# font, is in no row of the table and is therefore the unlisted case.
_IGNORED = frozenset((
    "ARMATURE", "ACTION", "KEY", "LATTICE", "PARTICLE", "TEXTURE", "CACHEFILE",
    "WINDOWMANAGER", "SCREEN", "WORKSPACE", "BRUSH", "PALETTE", "PAINTCURVE", "LIBRARY",
    "MASK", "MOVIECLIP", "SOUND", "SPEAKER", "LIGHT_PROBE", "LINESTYLE", "GREASEPENCIL",
    "GREASEPENCIL_V3", "TEXT",
))
_GEOMETRY_OBJECTS = frozenset(("MESH", "CURVE", "SURFACE", "META", "FONT", "CURVES", "POINTCLOUD",
                               "VOLUME"))


def settings_signature(scene):
    """What the engine reads from render settings and from scene.astroray; selection and frame number are
    excluded. Every astroray property counts: settings.build reads them, and a viewport has no other way
    to learn that one changed."""
    r, a = scene.render, scene.astroray
    return (r.resolution_x, r.resolution_y, r.resolution_percentage, r.pixel_aspect_x,
            r.pixel_aspect_y, r.use_border, r.film_transparent,
            tuple(getattr(a, p.identifier) for p in a.bl_rna.properties if p.identifier != "rna_type"))


def slot_signature(ob):
    """Hash of an object's material slots; unchanged means a shading tag is not ours to act on."""
    return hash(tuple((s.link, s.material.session_uid if s.material else 0)
                      for s in ob.material_slots))


def _object_kinds(ob, bits, cache):
    """Action kinds for an Object entry (ob is the original); empty means ignore."""
    t = ob.type
    if t == "LIGHT":
        return (LIGHTS,)
    if t == "CAMERA":
        return (CAMERA,) if bits & (TRANSFORM | GEOMETRY) else ()
    if t in _GEOMETRY_OBJECTS:
        kinds = []
        if bits & GEOMETRY:
            kinds.append(GEOMETRIES)
        # Slots changed: the instance material table is rebuilt; a shading tag alone is not ours.
        if bits & TRANSFORM or (bits & SHADING and
                                cache.slot_hash.get(ob.session_uid) != slot_signature(ob)):
            kinds.append(INSTANCES)
        return tuple(kinds)
    # Empties and the rest: only instancers have anything to re-iterate.
    if bits & TRANSFORM or (bits & SHADING and ob.is_instancer):
        return (INSTANCES,)
    return ()


def classify(depsgraph, cache):
    """Returns the actions to apply, in order, for one batch of depsgraph updates."""
    masks = {}  # (id_type, uid) -> [bits, original ID]
    for u in depsgraph.updates:
        orig = u.id.original
        bits = (TRANSFORM if u.is_updated_transform else 0) | \
               (GEOMETRY if u.is_updated_geometry else 0) | \
               (SHADING if u.is_updated_shading else 0)
        slot = masks.setdefault((orig.id_type, orig.session_uid), [0, orig])
        slot[0] |= bits

    out = {}
    for (id_type, uid), (bits, orig) in masks.items():
        if id_type == "SCENE":
            kinds = (SETTINGS,) if cache.settings_sig != settings_signature(orig) else ()
        elif id_type == "OBJECT":
            kinds = _object_kinds(orig, bits, cache)
        elif id_type in _BY_TYPE:
            kinds = (_BY_TYPE[id_type],)
        elif id_type in _IGNORED:
            kinds = ()
        else:
            cache.diagnostics["unlisted_id_type"] += 1
            print(f"WARNING astroray sync: unlisted ID type {id_type}, full resync")
            kinds = (FULL_RESYNC,)
        for kind in kinds:
            out[(kind, id_type, uid)] = Action(kind, id_type, uid, bits)

    if depsgraph.id_type_updated("OBJECT") and not any(k[0] == INSTANCES for k in out):
        keys = frozenset((i.object.original.session_uid,
                          tuple(i.persistent_id) if i.is_instance else ())
                         for i in depsgraph.object_instances)
        if keys != cache.instance_keys:
            out[(INSTANCES, "OBJECT", 0)] = Action(INSTANCES, "OBJECT", 0, 0)
    return sorted(out.values(), key=lambda a: (a.kind, a.id_type, a.uid))
