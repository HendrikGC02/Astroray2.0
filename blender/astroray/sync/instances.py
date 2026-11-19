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
    """The per-object fields foreach_get cannot read (strings and pointers), once per original object."""
    table = tuple(material_key(s.material) for s in ob.material_slots)
    if table not in pool_offsets:  # instances sharing a table share its offset
        pool_offsets[table] = len(pool)
        pool.extend(table)
    crypto = _hash_name(ob.name)
    top = _top_parent(ob)
    return (lightgroups.get(ob.lightgroup, 0), crypto, crypto if top == ob else _hash_name(top.name),
            pool_offsets[table] if table else 0, len(table))


def _object_scalars(uids):
    """visibility, flags, pass_index and colour of the original objects `uids`, in that order, each read for
    every object in bpy.data.objects with one foreach_get (B-31: per-object getattr was most of the loop)."""
    import bpy
    obs = bpy.data.objects
    n = len(obs)

    def get(prop, dt, w=1):
        a = np.empty(n * w, dt)
        obs.foreach_get(prop, a)
        return a.reshape(n, w) if w > 1 else a
    row = {u: i for i, u in enumerate(get("session_uid", np.int32).tolist())}
    pick = np.fromiter((row[u] for u in uids), np.intp, len(uids))
    vis = np.zeros(n, np.uint32)
    for attr, bit in _VISIBILITY:
        vis |= np.where(get(attr, np.bool_), np.uint32(bit), np.uint32(0))
    flags = (np.where(get("is_holdout", np.bool_), np.uint32(FLAG_HOLDOUT), np.uint32(0)) |
             np.where(get("is_shadow_catcher", np.bool_), np.uint32(FLAG_SHADOW_CATCHER), np.uint32(0)))
    return {"visibility": vis[pick], "flags": flags[pick], "pass_indices": get("pass_index", np.int32)[pick],
            "colors": get("color", np.float32, 4)[pick]}

def _rows(matrices16, origin):
    """(n, 16) raw float[16] matrix_world values -> (n, 12) row-major 3x4 rows with the origin folded in.
    RNA stores a 4x4 matrix property column by column (float m[4][4], m[column][row]), which is the
    order foreach_get returns; mathutils' Matrix rows are the transpose."""
    xf = np.ascontiguousarray(matrices16.reshape(-1, 4, 4).transpose(0, 2, 1)[:, :3, :]).reshape(-1, 12)
    xf[:, 3::4] -= np.asarray(origin, np.float32)
    return xf


def _matrix_row(m, origin):
    return _rows(np.asarray(m, np.float32).T.reshape(1, 16), origin)[0]


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
    # Per-instance work is Python lists only; the per-object fields are gathered by index once at the end and
    # the matrices come from one foreach_get over the same iterator (B-31: Python per-instance numpy writes and
    # Matrix conversions were most of the loop's 150 ms per 10k).
    props, pool, pool_offsets = {}, [], {}
    real = {}  # keys.geometry_identity's cache

    by_object, index_of = {}, {}
    rows, geom, obj, rand, owner = [], [], [], [], []
    seen = 0
    for inst in depsgraph.object_instances:
        k = seen
        seen += 1
        ob = inst.object
        if ob.type not in GEOMETRY_TYPES:
            continue
        orig = ob.original
        uid = orig.session_uid
        if uid not in props:
            props[uid] = _object_props(orig, lightgroups, material_key, pool, pool_offsets)
        n = len(rows)
        path = keys.instance_path(inst)
        ident = keys.object_identity(inst, path)
        if path:
            by_object.setdefault(path[0], []).append(n)
        geom.append(keymap.key(keys.geometry_identity(inst, depsgraph, path, real)))
        obj.append(keymap.key(ident))
        rand.append(inst.random_id & 0xFFFFFFFF)
        owner.append(uid)
        rows.append(k)
        by_object.setdefault(uid, []).append(n)
        index_of[ident] = n
    n = len(rows)
    m16 = np.empty(seen * 16, np.float32)
    if seen:
        depsgraph.object_instances.foreach_get("matrix_world", m16)
    xf = _rows(m16.reshape(seen, 16)[rows], origin) if n else np.zeros((0, 12), np.float32)
    order = list(props)
    slot = {u: i for i, u in enumerate(order)}
    which = np.fromiter((slot[u] for u in owner), np.intp, n)
    per = _object_scalars(order) if order else {}
    for name, column in (("light_groups", 0), ("crypto_objects", 1), ("crypto_assets", 2),
                         ("material_offsets", 3), ("material_counts", 4)):
        per[name] = [props[u][column] for u in order]
    arrays = {"geom_keys": np.asarray(geom, np.uint64), "object_keys": np.asarray(obj, np.uint64),
              "random_ids": np.asarray(rand, np.uint32)}
    for name, dt, w in _FIELDS:
        if name not in arrays:
            empty = np.zeros((0, w) if w > 1 else 0, dt)
            arrays[name] = np.ascontiguousarray(np.asarray(per[name], dt)[which]) if n else empty
    return InstanceTable(n, arrays, xf, np.asarray(pool, np.uint64), by_object, index_of)


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
    An object whose only row is its own (not an instancer, not inside an instance) is read from the
    evaluated object depsgraph.updates names, so the cost follows the moved objects (0007 d19: 0.1 ms);
    instancers and instanced objects walk depsgraph.object_instances for their rows.
    """
    wanted = set(uids)
    evaluated = {u.id.original.session_uid: u.id for u in depsgraph.updates
                 if u.id.original.id_type == "OBJECT" and u.id.original.session_uid in wanted}
    idx, rows, walk = [], [], set()
    for uid in wanted:
        own = table.index_of.get((uid,))
        ob = evaluated.get(uid)
        if own is not None and ob is not None and table.by_object.get(uid) == [own]:
            idx.append(own)
            rows.append(_matrix_row(ob.matrix_world, origin))
        else:
            walk.add(uid)
    if walk:
        for inst in depsgraph.object_instances:
            if inst.object.original.session_uid not in walk and not (
                    inst.is_instance and inst.parent.original.session_uid in walk):
                continue
            if inst.object.type not in GEOMETRY_TYPES:
                continue
            i = table.index_of.get(keys.object_identity(inst))
            if i is None:
                return False
            idx.append(i)
            rows.append(_matrix_row(inst.matrix_world, origin))
    if not idx:
        return False
    idx = np.asarray(idx, np.uint32)
    xf = np.ascontiguousarray(rows, np.float32)
    table.xf[idx] = xf
    c_xf = ctypes.POINTER(abi.ar_xform if abi else ctypes.c_float)
    edit.call("ar_edit_update_transforms", len(idx), idx.ctypes.data_as(ctypes.POINTER(ctypes.c_uint32)),
              xf.ctypes.data_as(c_xf))
    return True
