# SPDX-License-Identifier: GPL-3.0-or-later
"""Mesh read into the staged arrays of ar_edit_mesh_begin (0007 d17).

Ref: spike B syncmod.py extract_mesh_fast (git show spikes-final:spikes/B-blender-end-to-end/
syncmod.py) and docs/spikes/B-blender-end-to-end/RESULTS.md section 2: float32 and int32 attribute
reads are memcpy-class, `loop_triangles`, per-element RNA and float64 destinations are not.
The read (`fill`) writes into plain NumPy arrays, so it runs without the engine; `sync_mesh` is the
only function that touches the edit wrapper.
"""
import contextlib
import ctypes
from collections import namedtuple

import numpy as np

AR_MESH_HAS_MATERIAL_INDEX = 0x1  # include/astroray/astroray.h

Layout = namedtuple("Layout", "V C F U has_material_index")
MeshArrays = namedtuple("MeshArrays", "positions corner_vert face_start corner_normals uv material_index")


def layout(me):
    return Layout(len(me.vertices), len(me.loops), len(me.polygons), len(me.uv_layers),
                  "material_index" in me.attributes)


def alloc(lay):
    return MeshArrays(
        np.empty(lay.V * 3, np.float32), np.empty(lay.C, np.int32), np.empty(lay.F, np.int32),
        np.empty(lay.C * 3, np.float32), np.empty(lay.U * lay.C * 2, np.float32),
        np.empty(lay.F if lay.has_material_index else 0, np.int32))


def fill_positions(me, positions, corner_normals):
    me.attributes["position"].data.foreach_get("vector", positions)
    me.corner_normals.foreach_get("vector", corner_normals)


def _read_corner_vert(me, out, diagnostics):
    try:
        attr = me.attributes[".corner_vert"]
    except KeyError:
        # Internal attribute name: Blender may rename it. The public loops path is slower but right.
        print("WARNING astroray sync: attribute '.corner_vert' missing, reading loops.vertex_index")
        if diagnostics is not None:
            diagnostics["corner_vert_fallback"] += 1
        me.loops.foreach_get("vertex_index", out)
        return
    attr.data.foreach_get("value", out)


def fill(me, out, diagnostics=None):
    """Reads `me` into out (a MeshArrays sized by layout(me))."""
    fill_positions(me, out.positions, out.corner_normals)
    _read_corner_vert(me, out.corner_vert, diagnostics)
    me.polygons.foreach_get("loop_start", out.face_start)
    C = len(out.corner_vert)
    for i, layer in enumerate(me.uv_layers):
        me.attributes[layer.name].data.foreach_get("vector", out.uv[i * C * 2:(i + 1) * C * 2])
    if len(out.material_index):
        me.attributes["material_index"].data.foreach_get("value", out.material_index)


@contextlib.contextmanager
def mesh_of(ob):
    """The mesh to read for an evaluated object; curves, text, surfaces and metaballs convert
    through to_mesh() and are cleared afterwards."""
    if ob.type == "MESH":
        yield ob.data
        return
    me = ob.to_mesh()
    try:
        if me is None:
            raise RuntimeError(f"astroray sync: object {ob.name!r} of type {ob.type} has no mesh")
        yield me
    finally:
        ob.to_mesh_clear()


def staged(edit, field, count, dtype, shape):
    """NumPy view over one `T**` staged field; `count` is the engine-written element count."""
    if int(np.prod(shape)) != count:
        raise RuntimeError(f"astroray sync: engine staged {count} elements, host expects {shape}")
    if count == 0:
        return np.empty(shape, dtype)
    ptr = ctypes.cast(field, ctypes.POINTER(ctypes.c_void_p))[0]
    return edit.staged(ptr, dtype, shape)


def sync_mesh(edit, abi, me, key, name, diagnostics=None):
    """Full read of one mesh as geometry `key` (a committed engine item)."""
    lay = layout(me)
    d = abi.ar_mesh_desc()
    d.key, d.vertex_count, d.corner_count, d.face_count = key, lay.V, lay.C, lay.F
    d.uv_layer_count, d.motion_steps = lay.U, 1
    d.flags = AR_MESH_HAS_MATERIAL_INDEX if lay.has_material_index else 0
    d.name = (ctypes.c_uint8 * 64)(*name.encode("utf-8")[:63].ljust(64, b"\0"))
    b = abi.ar_mesh_buffers()
    edit.call("ar_edit_mesh_begin", ctypes.byref(d), ctypes.byref(b))
    out = MeshArrays(
        staged(edit, b.positions, b.positions_count, np.float32, (lay.V * 3,)),
        staged(edit, b.corner_vert, b.corner_vert_count, np.int32, (lay.C,)),
        staged(edit, b.face_start, b.face_start_count, np.int32, (lay.F,)),
        staged(edit, b.corner_normals, b.corner_normals_count, np.float32, (lay.C * 3,)),
        staged(edit, b.uv, b.uv_count, np.float32, (lay.U * lay.C * 2,)),
        staged(edit, b.material_index, b.material_index_count, np.int32,
               (lay.F if lay.has_material_index else 0,)))
    fill(me, out, diagnostics)
    edit.item_commit(key)
    return lay
