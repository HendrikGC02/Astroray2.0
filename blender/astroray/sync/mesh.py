# SPDX-License-Identifier: GPL-3.0-or-later
"""Mesh read into the staged arrays of ar_edit_mesh_begin (0007 d17).

Ref: spike B syncmod.py extract_mesh_fast (git show spikes-final:spikes/B-blender-end-to-end/
syncmod.py) and docs/spikes/B-blender-end-to-end/RESULTS.md section 2 for what is read; how it is read
is 0007 d35 remedy 1 (direct reads below).
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


# Direct reads (0007 d35 remedy 1). foreach_get of '.corner_vert' and loop_start costs ~25 ns per element, and
# every foreach_get into an engine-staged (ctypes-backed) view takes Blender's slow per-element path, so each
# array is copied with one memmove from the address of its element 0 (`as_pointer()`, as Malt reads meshes).
# Assumption (listed in api_surface.txt): the RNA element pointer of each collection below addresses one
# contiguous array of that element type, which is how Blender 5.3 stores them: Mesh.vert_data / corner_data /
# face_data layers and Mesh.attribute_storage (DNA_mesh_types.h; '.corner_vert' int32 per corner, 'position'
# float3 per vertex, UV maps float2 per corner, 'material_index' int32 per face), Mesh.face_offset_indices
# (int32 per face, MeshPolygon) and the runtime corner-normal cache (float3 per corner, MeshNormalValue).
# Every read checks the assumption against the attribute API and raises on a mismatch (_copy); there is no
# fallback to the slow path.
# Ref: Malt's as_pointer() mesh reads, as 0007 d35 and its rejected-alternatives list cite them;
# Blender 5.3 source/blender/makesdna/DNA_mesh_types.h (struct Mesh fields named above, layout only).
_ATTR_TYPES = {".corner_vert": ("INT", "CORNER"), "position": ("FLOAT_VECTOR", "POINT"),
               "material_index": ("INT", "FACE")}


class LayoutMismatch(RuntimeError):
    """The direct read disagrees with the attribute API: Blender changed a layout api_surface.txt lists."""


def _mismatch(what, why):
    import bpy
    raise LayoutMismatch(f"astroray sync: direct read of {what} does not match the attribute API ({why}) in "
                         f"Blender {bpy.app.version_string}; the layout listed in api_surface.txt changed")


def _attribute(me, name, data_type=None, domain=None):
    try:
        attr = me.attributes[name]
    except KeyError:
        _mismatch(f"attribute {name!r}", "attribute missing")
    want = (data_type, domain) if data_type else _ATTR_TYPES[name]
    if (attr.data_type, attr.domain) != want:
        _mismatch(f"attribute {name!r}", f"type {attr.data_type} on {attr.domain}, expected {want[0]} on {want[1]}")
    return attr.data


def _copy(coll, dst, width, value, what):
    """memmove len(dst) // width elements of `coll` into dst after checking stride, contiguity to the last
    element and the first and last values against the RNA reads (`value` gives one element's components)."""
    n = len(dst) // width
    if n == 0:
        return
    itemsize = dst.itemsize * width
    first, last = coll[0], coll[n - 1]
    base = first.as_pointer()
    if n > 1 and coll[1].as_pointer() - base != itemsize:
        _mismatch(what, f"element stride {coll[1].as_pointer() - base} bytes, expected {itemsize}")
    if last.as_pointer() != base + (n - 1) * itemsize:
        _mismatch(what, f"{n} elements are not one contiguous array")
    ctypes.memmove(dst.ctypes.data, base, n * itemsize)
    rows = dst.reshape(n, width)
    for i, el in ((0, first), (n - 1, last)):
        if not np.array_equal(rows[i], np.asarray(value(el), dst.dtype).reshape(width)):
            _mismatch(what, f"element {i} reads {rows[i].tolist()}, the API gives {value(el)}")


def fill_positions(me, positions, corner_normals):
    _copy(_attribute(me, "position"), positions, 3, lambda e: e.vector, "'position'")
    _copy(me.corner_normals, corner_normals, 3, lambda e: e.vector, "corner_normals")


def fill(me, out):
    """Reads `me` into out (a MeshArrays sized by layout(me))."""
    fill_positions(me, out.positions, out.corner_normals)
    _copy(_attribute(me, ".corner_vert"), out.corner_vert, 1, lambda e: e.value, "'.corner_vert'")
    _copy(me.polygons, out.face_start, 1, lambda e: e.loop_start, "polygons (face offsets)")
    C = len(out.corner_vert)
    for i, layer in enumerate(me.uv_layers):
        _copy(_attribute(me, layer.name, "FLOAT2", "CORNER"), out.uv[i * C * 2:(i + 1) * C * 2], 2,
              lambda e: e.vector, f"UV map {layer.name!r}")
    if len(out.material_index):
        _copy(_attribute(me, "material_index"), out.material_index, 1, lambda e: e.value, "'material_index'")


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


def sync_mesh(edit, abi, me, key, name):
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
    fill(me, out)
    edit.item_commit(key)
    return lay
