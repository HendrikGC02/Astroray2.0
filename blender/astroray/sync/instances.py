# SPDX-License-Identifier: GPL-3.0-or-later
"""Instance descriptor from one pass over depsgraph.object_instances (0007 d19).

Ref: docs/decisions/0007-blender-integration.md decision 19; ar_instances_desc in
docs/decisions/0007-appendix-abi.md (copy-in arrays, visibility word separate from flags, material
table pool); scene-origin translation folded into every transform per 0003 decision 26.
`build` returns plain NumPy arrays so descriptor building runs without the engine DLL; only
`set_instances` and `sync_transforms` touch the edit wrapper.
"""
import ctypes

import numpy as np

from . import keys

# include/astroray/astroray.h
VIS_CAMERA, VIS_DIFFUSE, VIS_GLOSSY, VIS_TRANSMISSION, VIS_SHADOW, VIS_VOLUME = 1, 2, 4, 8, 16, 32
FLAG_HOLDOUT, FLAG_SHADOW_CATCHER = 1, 2

GEOMETRY_TYPES = frozenset(("MESH", "CURVE", "SURFACE", "META", "FONT"))
_VISIBILITY = (("visible_camera", VIS_CAMERA), ("visible_diffuse", VIS_DIFFUSE),
               ("visible_glossy", VIS_GLOSSY), ("visible_transmission", VIS_TRANSMISSION),
               ("visible_shadow", VIS_SHADOW), ("visible_volume_scatter", VIS_VOLUME))
_FIELDS = (  # name, dtype, width
    ("geom_keys", np.uint64, 1), ("object_keys", np.uint64, 1), ("random_ids", np.uint32, 1),
    ("pass_indices", np.uint32, 1), ("crypto_objects", np.uint32, 1),
    ("crypto_assets", np.uint32, 1), ("colors", np.float32, 4), ("visibility", np.uint32, 1),
    ("flags", np.uint32, 1), ("light_groups", np.uint8, 1), ("material_offsets", np.uint32, 1),
    ("material_counts", np.uint32, 1))


def murmur3_32(data, seed=0):
    """MurmurHash3_x86_32 (Appleby, public domain); cryptomatte ids per 0008 decision 11."""
    c1, c2, mask = 0xCC9E2D51, 0x1B873593, 0xFFFFFFFF
    h = seed
    n = len(data) & ~3
    for i in range(0, n, 4):
        k = int.from_bytes(data[i:i + 4], "little")
        k = (k * c1) & mask
        k = ((k << 15) | (k >> 17)) & mask
        h ^= (k * c2) & mask
        h = ((h << 13) | (h >> 19)) & mask
        h = (h * 5 + 0xE6546B64) & mask
    k = int.from_bytes(data[n:], "little")
    if len(data) & 3:
        k = (k * c1) & mask
        k = ((k << 15) | (k >> 17)) & mask
        h ^= (k * c2) & mask
    h ^= len(data)
    h ^= h >> 16
    h = (h * 0x85EBCA6B) & mask
    h ^= h >> 13
    h = (h * 0xC2B2AE35) & mask
    return h ^ (h >> 16)


def _hash_name(name):
    return murmur3_32(name.encode("utf-8"))


class InstanceTable:
    """The filled arrays plus the maps a later transform-only edit needs."""

    def __init__(self, n, arrays, xf, material_keys, by_object, index_of):
        self.n = n
        self.arrays = arrays              # field name -> contiguous array, length n
        self.xf = xf                      # (n, 12) float32, instance-major (S = 1)
        self.material_keys = material_keys
        self.by_object = by_object        # original object or parent session_uid -> [index]
        self.index_of = index_of          # keys.object_identity tuple -> index


def _top_parent(ob):
    while ob.parent is not None:
        ob = ob.parent
    return ob


def _object_props(ob, lightgroups, material_key, pool, pool_offsets):
    """Per original object: read once and cached by the caller."""
    vis = 0
    for attr, bit in _VISIBILITY:
        if getattr(ob, attr):
            vis |= bit
    flags = (FLAG_HOLDOUT if ob.is_holdout else 0) | \
            (FLAG_SHADOW_CATCHER if ob.is_shadow_catcher else 0)
    table = tuple(material_key(s.material) for s in ob.material_slots)
    if table not in pool_offsets:  # instances sharing a table share its offset
        pool_offsets[table] = len(pool)
        pool.extend(table)
    return {
        "visibility": vis, "flags": flags, "pass_index": ob.pass_index, "color": tuple(ob.color),
        "light_group": lightgroups.get(ob.lightgroup, 0),
        "crypto_object": _hash_name(ob.name), "crypto_asset": _hash_name(_top_parent(ob).name),
        "mat_offset": pool_offsets[table] if table else 0, "mat_count": len(table),
    }


def _matrix_row(inst, origin):
    m = np.asarray(inst.matrix_world, np.float32)[:3, :].reshape(12).copy()
    m[3] -= origin[0]
    m[7] -= origin[1]
    m[11] -= origin[2]
    return m


def lightgroup_indices(view_layer):
    """Blender light group name -> u8 id; 0 is no group (0008 decision 10), at most 255."""
    names = [g.name for g in view_layer.lightgroups]
    if len(names) > 255:
        raise RuntimeError(f"astroray sync: {len(names)} light groups, the limit is 255")
    return {n: i + 1 for i, n in enumerate(names)}


def build(depsgraph, keymap, origin=(0.0, 0.0, 0.0), lightgroups=None, material_key=None):
    """One pass over depsgraph.object_instances; returns an InstanceTable (S = 1).

    material_key maps a Material (or None for an empty slot) to an ar_key; the default keys by
    original session_uid and uses 0, the default material, for an empty slot.
    """
    lightgroups = lightgroups or {}
    if material_key is None:
        def material_key(mat):
            return keymap.key(("material", mat.original.session_uid)) if mat else 0
    cap = 64
    arrays = {name: np.zeros((cap, w) if w > 1 else cap, dt) for name, dt, w in _FIELDS}
    xf = np.zeros((cap, 12), np.float32)
    props, pool, pool_offsets = {}, [], {}
    by_object, index_of = {}, {}
    n = 0
    for inst in depsgraph.object_instances:
        ob = inst.object
        if ob.type not in GEOMETRY_TYPES:
            continue
        orig = ob.original
        p = props.get(orig.session_uid)
        if p is None:
            p = props[orig.session_uid] = _object_props(orig, lightgroups, material_key, pool,
                                                       pool_offsets)
        if n == cap:
            cap *= 2
            arrays = {k: np.resize(v, (cap,) + v.shape[1:]) for k, v in arrays.items()}
            xf = np.resize(xf, (cap, 12))
        ident = keys.object_identity(inst)
        gid = keys.geometry_identity(inst, depsgraph)
        arrays["geom_keys"][n] = keymap.key(gid)
        arrays["object_keys"][n] = keymap.key(ident)
        arrays["random_ids"][n] = inst.random_id & 0xFFFFFFFF
        arrays["pass_indices"][n] = p["pass_index"]
        arrays["crypto_objects"][n] = p["crypto_object"]
        arrays["crypto_assets"][n] = p["crypto_asset"]
        arrays["colors"][n] = p["color"]
        arrays["visibility"][n] = p["visibility"]
        arrays["flags"][n] = p["flags"]
        arrays["light_groups"][n] = p["light_group"]
        arrays["material_offsets"][n] = p["mat_offset"]
        arrays["material_counts"][n] = p["mat_count"]
        xf[n] = _matrix_row(inst, origin)
        by_object.setdefault(orig.session_uid, []).append(n)
        if inst.is_instance:
            by_object.setdefault(inst.parent.original.session_uid, []).append(n)
        index_of[ident] = n
        n += 1
    arrays = {k: np.ascontiguousarray(v[:n]) for k, v in arrays.items()}
    return InstanceTable(n, arrays, np.ascontiguousarray(xf[:n]),
                         np.asarray(pool, np.uint64), by_object, index_of)


def set_instances(edit, abi, table):
    """Full instance replacement through ar_edit_set_instances (all arrays copy-in)."""
    d = abi.ar_instances_desc()
    d.instance_count, d.motion_steps = table.n, 1
    d.transform_count, d.material_key_count = table.n, len(table.material_keys)
    ftypes = dict(d._fields_)
    keep = dict(table.arrays, xf=table.xf, material_keys=table.material_keys)
    for name, arr in keep.items():
        setattr(d, name, arr.ctypes.data_as(ftypes[name]))
    edit.call("ar_edit_set_instances", ctypes.byref(d))


def sync_transforms(edit, abi, table, depsgraph, uids, origin=(0.0, 0.0, 0.0)):
    """A plain move: one ar_edit_update_transforms call for the instances of the objects in `uids`
    (original session_uids, as the object or as the instancer parent).

    Returns False, sending nothing, when an affected instance is not in the table (the set of
    instances changed and the caller must rebuild with `build`).
    """
    wanted = set(uids)
    idx, rows = [], []
    for inst in depsgraph.object_instances:
        if inst.object.original.session_uid not in wanted and not (
                inst.is_instance and inst.parent.original.session_uid in wanted):
            continue
        if inst.object.type not in GEOMETRY_TYPES:
            continue
        i = table.index_of.get(keys.object_identity(inst))
        if i is None:
            return False
        idx.append(i)
        rows.append(_matrix_row(inst, origin))
    if not idx:
        return False
    idx = np.asarray(idx, np.uint32)
    xf = np.ascontiguousarray(rows, np.float32)
    table.xf[idx] = xf
    c_xf = ctypes.POINTER(abi.ar_xform if abi else ctypes.c_float)
    edit.call("ar_edit_update_transforms", len(idx), idx.ctypes.data_as(ctypes.POINTER(ctypes.c_uint32)),
              xf.ctypes.data_as(c_xf))
    return True
