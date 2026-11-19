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


_CTYPES = {np.float32: (4, ctypes.c_float), np.int32: (4, ctypes.c_int32)}


class Staged:
    """An engine-staged array as its address: the direct read memmoves into it, so no NumPy view is built
    (B-31: a view per staged array was most of the per-mesh staging cost)."""
    __slots__ = ("address", "size", "itemsize", "_ctype")

    def __init__(self, address, size, dtype):
        self.address, self.size = address, size
        self.itemsize, self._ctype = _CTYPES[dtype]

    def __len__(self):
        return self.size

    def values(self, start, stop):
        return (self._ctype * (stop - start)).from_address(self.address + start * self.itemsize)[:]


def _copy(coll, dst, width, value, what, check=True):
    """memmove len(dst) // width elements of `coll` into dst (a NumPy array or Staged). With `check`, first the
    stride and contiguity to the last element, and afterwards the first and last values against the RNA reads
    (`value` gives one element's components); LayoutChecks decides when a read needs it."""
    n = len(dst) // width
    if n == 0:
        return
    itemsize = dst.itemsize * width
    first = coll[0]
    base = first.as_pointer()
    staged = isinstance(dst, Staged)
    if not check:
        ctypes.memmove(dst.address if staged else dst.ctypes.data, base, n * itemsize)
        return
    last = coll[n - 1]
    if n > 1 and coll[1].as_pointer() - base != itemsize:
        _mismatch(what, f"element stride {coll[1].as_pointer() - base} bytes, expected {itemsize}")
    if last.as_pointer() != base + (n - 1) * itemsize:
        _mismatch(what, f"{n} elements are not one contiguous array")
    ctypes.memmove(dst.address if staged else dst.ctypes.data, base, n * itemsize)
    for i, el in ((0, first), (n - 1, last)):
        a, b = i * width, (i + 1) * width
        got, want = (dst.values(a, b) if staged else dst[a:b].tolist()), value(el)
        exp = list(want) if width > 1 else [want]  # float32 values compare exactly as Python floats; NaN equals NaN
        if got != exp and any(a != b and not (a != a and b != b) for a, b in zip(got, exp)):
            _mismatch(what, f"element {i} reads {got}, the API gives {exp}")


def fill_positions(me, positions, corner_normals, check=True):
    if len(positions):  # an empty mesh has no 'position' attribute at all
        _copy(_attribute(me, "position"), positions, 3, lambda e: e.vector, "'position'", check)
    _copy(me.corner_normals, corner_normals, 3, lambda e: e.vector, "corner_normals", check)


def _part(arr, i, n):
    """Elements [i*n, (i+1)*n) of a NumPy array or a Staged array."""
    if isinstance(arr, Staged):
        return Staged(arr.address + i * n * arr.itemsize, n, np.float32)
    return arr[i * n:(i + 1) * n]


def fill(me, out, check=True):
    """Reads `me` into out (a MeshArrays sized by layout(me), of NumPy or Staged arrays)."""
    fill_positions(me, out.positions, out.corner_normals, check)
    if len(out.corner_vert):  # nor has a mesh without faces a '.corner_vert'
        _copy(_attribute(me, ".corner_vert"), out.corner_vert, 1, lambda e: e.value, "'.corner_vert'", check)
    _copy(me.polygons, out.face_start, 1, lambda e: e.loop_start, "polygons (face offsets)", check)
    C = len(out.corner_vert)
    for i, layer in enumerate(me.uv_layers):
        _copy(_attribute(me, layer.name, "FLOAT2", "CORNER"), _part(out.uv, i, C * 2), 2,
              lambda e: e.vector, f"UV map {layer.name!r}", check)
    if len(out.material_index):
        _copy(_attribute(me, "material_index"), out.material_index, 1, lambda e: e.value, "'material_index'",
              check)


class LayoutChecks:
    """Geometry keys whose direct read passed the layout check, with the Layout it was checked at (caller-owned,
    one per engine scene, like the KeyMap). The check guards the layout (counts, stride, contiguity), not values:
    a read skips the stride, contiguity and value checks for a key already checked at an equal Layout, so a
    positions-only update or a full read with unchanged counts keeps the key's check, and a full read with changed
    counts (a topology change) checks again; the attribute type and domain lookup runs on every read (0007 d35
    note, lead ruling in task 129). The caller forgets a key that leaves the scene.
    """
    def __init__(self):
        self._at = {}

    def needed(self, key, lay):
        return self._at.get(key) != lay

    def passed(self, key, lay):
        self._at[key] = lay

    def forget(self, key):
        self._at.pop(key, None)

    def __contains__(self, key):
        return key in self._at


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


_VOID_PP = ctypes.POINTER(ctypes.c_void_p)


def staged(field, count, dtype, n):
    """One `T**` staged field as a Staged array of n elements; `count` is the engine-written element count. It is
    valid until the item's commit, which every caller makes right after the read."""
    if n != count:
        raise RuntimeError(f"astroray sync: engine staged {count} elements, host expects {n}")
    return Staged(ctypes.cast(field, _VOID_PP)[0] if count else 0, count, dtype)


def sync_object(edit, abi, ob, key, checks):
    """Full read of evaluated object `ob`'s mesh as geometry `key`, the one step the viewport and F12 take for a
    new or updated geometry; `checks` decides whether the layout check runs (LayoutChecks)."""
    with mesh_of(ob) as me:
        return sync_mesh(edit, abi, me, key, ob.original.name, checks=checks)


def sync_mesh(edit, abi, me, key, name, checks=None):
    """Full read of one mesh as geometry `key` (a committed engine item). `checks` (LayoutChecks) skips the
    layout check of a key already checked at this Layout; without it every read is checked."""
    lay = layout(me)
    d = abi.ar_mesh_desc()
    d.key, d.vertex_count, d.corner_count, d.face_count = key, lay.V, lay.C, lay.F
    d.uv_layer_count, d.motion_steps = lay.U, 1
    d.flags = AR_MESH_HAS_MATERIAL_INDEX if lay.has_material_index else 0
    raw = name.encode("utf-8")[:63]
    ctypes.memmove(d.name, raw, len(raw))  # the descriptor is zero-filled, so the name stays terminated
    b = abi.ar_mesh_buffers()
    edit.call("ar_edit_mesh_begin", ctypes.byref(d), ctypes.byref(b))
    out = MeshArrays(
        staged(b.positions, b.positions_count, np.float32, lay.V * 3),
        staged(b.corner_vert, b.corner_vert_count, np.int32, lay.C),
        staged(b.face_start, b.face_start_count, np.int32, lay.F),
        staged(b.corner_normals, b.corner_normals_count, np.float32, lay.C * 3),
        staged(b.uv, b.uv_count, np.float32, lay.U * lay.C * 2),
        staged(b.material_index, b.material_index_count, np.int32, lay.F if lay.has_material_index else 0))
    check = checks is None or checks.needed(key, lay)
    fill(me, out, check)
    edit.item_commit(key)
    if checks is not None and check:
        checks.passed(key, lay)
    return lay
