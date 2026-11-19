# SPDX-License-Identifier: GPL-3.0-or-later
"""Topology cache (0007 d22): positions only where topology provably cannot have changed.

Ref: docs/decisions/0007-blender-integration.md decision 22. Anything not in the two listed cases
is a full read, which costs time but is never wrong.
"""
import ctypes

import numpy as np

from . import mesh as mesh_mod

FULL, POSITIONS = "full", "positions"

# Blender modifier types that move vertices and never add or remove elements (d22 case a).
DEFORM_ONLY = frozenset((
    "ARMATURE", "CAST", "CURVE", "DISPLACE", "HOOK", "LAPLACIANDEFORM", "LATTICE", "MESH_DEFORM",
    "SHRINKWRAP", "SIMPLE_DEFORM", "SMOOTH", "CORRECTIVE_SMOOTH", "LAPLACIANSMOOTH",
    "SURFACE_DEFORM", "WARP", "WAVE",
))
# Modal operators that cannot change topology (d22 case b): the transform family.
MODAL_ALLOW = frozenset((
    "transform.translate", "transform.rotate", "transform.resize", "transform.transform",
    "transform.trackball", "transform.shear", "transform.bend", "transform.push_pull",
    "transform.tosphere", "transform.shrink_fatten", "transform.edge_slide",
    "transform.vert_slide",
))
SCULPT_STROKE = "sculpt.brush_stroke"  # allowed only with dynamic topology off


def _norm(idname):
    """'TRANSFORM_OT_translate' and 'transform.translate' name the same operator."""
    return idname.lower().replace("_ot_", ".")


def is_deform_only(ob):
    return ob.type == "MESH" and all(m.type in DEFORM_ONLY for m in ob.modifiers)


def mesh_geometry_updated(depsgraph, original_mesh):
    """True when this batch carries a geometry update on the original Mesh datablock."""
    uid = original_mesh.session_uid
    return any(u.is_updated_geometry and u.id.original.session_uid == uid
               for u in depsgraph.updates if u.id.original.id_type == "MESH")


def modal_operator_ids(context):
    return [op.bl_idname for op in context.window.modal_operators] if context.window else []


def decide(known, deform_only, original_mesh_updated, modal_ids=(), dynamic_topology=False,
           final_render=False):
    """FULL or POSITIONS for one geometry update; `known` is whether (V, C, F) is cached."""
    if final_render or not known:
        return FULL
    if deform_only and not original_mesh_updated:
        return POSITIONS
    ids = [_norm(i) for i in modal_ids]
    if ids and all(i in MODAL_ALLOW or (i == SCULPT_STROKE and not dynamic_topology) for i in ids):
        return POSITIONS
    return FULL


class TopologyCache:
    def __init__(self):
        self.counts = {}  # geometry ar_key -> (V, C, F)

    def forget(self, key):
        self.counts.pop(key, None)


def read_geometry(edit, abi, cache, key, me, name, mode, diagnostics=None, checks=None):
    """Sends geometry `key`; a positions-only request whose counts disagree with the cache does
    a full read and records a diagnostic. `checks` (mesh.LayoutChecks): a positions read of a key whose full
    read passed the layout check skips it, since its counts are the cached ones."""
    counts = (len(me.vertices), len(me.loops), len(me.polygons))
    if mode == POSITIONS and cache.counts.get(key) != counts:
        print(f"WARNING astroray sync: topology changed on the positions path for {name!r}, "
              "full read")
        if diagnostics is not None:
            diagnostics["topology_cache_mismatch"] += 1
        mode = FULL
    if mode == FULL:
        mesh_mod.sync_mesh(edit, abi, me, key, name, checks)
        cache.counts[key] = counts
        return FULL
    b = abi.ar_positions_buffers()
    edit.call("ar_edit_positions_begin", key, ctypes.byref(b))
    V, C, _ = counts
    mesh_mod.fill_positions(
        me, mesh_mod.staged(b.positions, b.positions_count, np.float32, V * 3),
        mesh_mod.staged(b.corner_normals, b.corner_normals_count, np.float32, C * 3),
        checks is None or key not in checks)
    edit.item_commit(key)
    return POSITIONS
