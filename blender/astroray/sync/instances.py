# SPDX-License-Identifier: GPL-3.0-or-later
"""Instance descriptor from one pass over depsgraph.object_instances (0007 d19).

Ref: docs/decisions/0007-blender-integration.md decision 19; ar_instances_desc in
docs/decisions/0007-appendix-abi.md (copy-in arrays, visibility word separate from flags, material
table pool); scene-origin translation folded into every transform per 0003 decision 26.
`build` returns plain NumPy arrays so descriptor building runs without the engine DLL; only
`set_instances` and `sync_transforms` touch the edit wrapper.
"""
import ctypes
from operator import itemgetter

import numpy as np

from . import classify, keys

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
    """MurmurHash3_x86_32; cryptomatte ids per 0008 decision 11.
    Ref: Austin Appleby, MurmurHash3_x86_32 in SMHasher src/MurmurHash3.cpp (public domain)."""
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


def murmur3_32_many(datas, seed=0):
    """murmur3_32 of every bytes object in `datas` as a uint32 array, vectorised over equal lengths: the same
    MurmurHash3_x86_32 steps on uint32 lanes (B-31: the scalar hash of S2's 7,042 names took 15 ms).
    Ref: Austin Appleby, MurmurHash3_x86_32 in SMHasher src/MurmurHash3.cpp (public domain)."""
    out = np.empty(len(datas), np.uint32)
    by_len = {}
    for i, d in enumerate(datas):
        by_len.setdefault(len(d), []).append(i)
    c1, c2 = np.uint32(0xCC9E2D51), np.uint32(0x1B873593)
    s13, s15, s16, s17, s19 = (np.uint32(s) for s in (13, 15, 16, 17, 19))
    for length, idx in by_len.items():
        buf = np.frombuffer(b"".join(datas[i] for i in idx), np.uint8).reshape(len(idx), length)
        h = np.full(len(idx), seed, np.uint32)
        nb = length // 4
        blocks = np.ascontiguousarray(buf[:, :nb * 4]).view("<u4")
        for j in range(nb):
            k = blocks[:, j] * c1
            h ^= ((k << s15) | (k >> s17)) * c2
            h = ((h << s13) | (h >> s19)) * np.uint32(5) + np.uint32(0xE6546B64)
        if length & 3:
            k = np.zeros(len(idx), np.uint32)
            for b in range(length & 3):
                k |= buf[:, nb * 4 + b].astype(np.uint32) << np.uint32(8 * b)
            k *= c1
            h ^= ((k << s15) | (k >> s17)) * c2
        h ^= np.uint32(length & 0xFFFFFFFF)
        h ^= h >> s16
        h *= np.uint32(0x85EBCA6B)
        h ^= h >> s13
        h *= np.uint32(0xC2B2AE35)
        h ^= h >> s16
        out[idx] = h
    return out


class _RealCache(dict):
    """keys.geometry_identity's (original uid, evaluated data pointer) cache, indexed by uid to forget."""

    def __init__(self):
        super().__init__()
        self.by_uid = {}

    def __setitem__(self, k, v):
        super().__setitem__(k, v)
        self.by_uid.setdefault(k[0], []).append(k)

    def forget(self, uid):
        for k in self.by_uid.pop(uid, ()):
            self.pop(k, None)


class InstanceCache:
    """Per-object work build() would otherwise repeat on every rebuild (caller-owned, one per KeyMap, 0007 d19:
    per-object properties read once and cached). Name hashes are keyed by the name itself, a pure function, so a
    rename needs nothing; everything else is keyed by original session_uid and dropped by forget() for every ID a
    depsgraph update names (the object, its data, or both): the object record (material table, light group,
    cryptomatte ids), the real-data answer and the keys of the object's own row. A cryptomatte asset id follows
    the top parent's name, which an update to the child does not name, so a parented object's asset id is not
    kept (record asset -1) and build() reads it again."""

    PARENTED = -1

    def __init__(self):
        self.hashes = {}         # object name -> murmur3_32
        self.tids = {}           # original object session_uid -> material table id (one dict per field of the
        self.lights = {}         # object record: light group name, cryptomatte object id and asset id; build()
        self.crypto = {}         # gathers a column with one C-level map over the rows' uids)
        self.assets = {}
        self.users = {}          # original data session_uid -> {object session_uids cached with that data}
        self.real = _RealCache()
        self.plain = {}          # original object session_uid -> (geometry key, object key, identity) of its own
                                 # row (not an instance), or () for a type build() skips
        self.table_ids = {}      # material table (tuple of ar_keys) -> small id; a pure function, never forgotten
        self.table_list = []
        self.light_names = None  # the view layer's light group names at the last build()

    def table_id(self, table):
        i = self.table_ids.get(table)
        if i is None:
            i = self.table_ids[table] = len(self.table_list)
            self.table_list.append(table)
        return i

    def remember(self, uid, data_uid, table, light_group, crypto, asset):
        self.tids[uid] = self.table_id(table)
        self.lights[uid], self.crypto[uid], self.assets[uid] = light_group, crypto, asset
        if data_uid is not None:
            self.users.setdefault(data_uid, set()).add(uid)

    def forget(self, uids):
        """Drops what an update to these original IDs (objects or their data) may have changed."""
        for u in uids:
            for o in (u, *self.users.pop(u, ())):
                for d in (self.tids, self.lights, self.crypto, self.assets, self.plain):
                    d.pop(o, None)
                self.real.forget(o)

    def hash_names(self, names):
        missing = [n for n in set(names) if n not in self.hashes]
        if missing:
            self.hashes.update(zip(missing, murmur3_32_many([n.encode("utf-8") for n in missing]).tolist()))
        return [self.hashes[n] for n in names]


def classify_updates(depsgraph, sync_cache, cache):
    """classify.classify's actions for one batch of depsgraph updates and the evaluated IDs its walk met
    (original session_uid -> ID); every updated ID is forgotten from `cache` (an InstanceCache) first, so no
    later build reuses per-object work an update may have changed."""
    evaluated = {}
    actions = classify.classify(depsgraph, sync_cache, evaluated)
    cache.forget(evaluated)
    return actions, evaluated


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


def _remember_objects(fresh, material_key, cache):
    """Records for the original objects `fresh` (uid -> object) the cache has none for: material table, light
    group name, cryptomatte object id (the name hash) and asset id (the top parent's name hash, or PARENTED)."""
    uids, obs = list(fresh), list(fresh.values())
    hashes = cache.hash_names([ob.name for ob in obs])
    for uid, ob, h in zip(uids, obs, hashes):
        data = ob.data
        table = tuple(material_key(s.material) for s in ob.material_slots)
        cache.remember(uid, data.session_uid if data is not None else None, table, ob.lightgroup, h,
                       h if ob.parent is None else cache.PARENTED)


def _object_scalars(uids):
    """visibility, flags, pass_index and colour of the original objects `uids`, in that order, each read for
    every object in bpy.data.objects with one foreach_get (B-31: per-object getattr was most of the loop). `uids`
    is an int32 array; also the row of each in bpy.data.objects."""
    import bpy
    obs = bpy.data.objects
    n = len(obs)

    def get(prop, dt, w=1):
        a = np.empty(n * w, dt)
        obs.foreach_get(prop, a)
        return a.reshape(n, w) if w > 1 else a
    su = get("session_uid", np.int32)
    order = np.argsort(su)
    pick = order[np.minimum(np.searchsorted(su[order], uids), n - 1)]
    if (su[pick] != uids).any():
        raise RuntimeError("astroray sync: an instanced object is not in bpy.data.objects")
    vis = np.zeros(n, np.uint32)
    for attr, bit in _VISIBILITY:
        vis |= np.where(get(attr, np.bool_), np.uint32(bit), np.uint32(0))
    flags = (np.where(get("is_holdout", np.bool_), np.uint32(FLAG_HOLDOUT), np.uint32(0)) |
             np.where(get("is_shadow_catcher", np.bool_), np.uint32(FLAG_SHADOW_CATCHER), np.uint32(0)))
    return ({"visibility": vis[pick], "flags": flags[pick], "pass_indices": get("pass_index", np.int32)[pick],
             "colors": get("color", np.float32, 4)[pick]}, pick)

def _rows(matrices16, origin):
    """(n, 16) raw float[16] matrix_world values -> (n, 12) row-major 3x4 rows with the origin folded in.
    RNA stores a 4x4 matrix property column by column (float m[4][4], m[column][row]), which is the
    order foreach_get returns; mathutils' Matrix rows are the transpose."""
    xf = np.ascontiguousarray(matrices16.reshape(-1, 4, 4).transpose(0, 2, 1)[:, :3, :]).reshape(-1, 12)
    xf[:, 3::4] -= np.asarray(origin, np.float32)
    return xf


def _matrix_row(m):
    """The 12 floats of one mathutils Matrix's top three rows; the origin is subtracted later in float32, as
    _rows does (B-31: a NumPy array per Matrix was a third of the plain move)."""
    return (*m[0], *m[1], *m[2])


def lightgroup_indices(view_layer):
    """Blender light group name -> u8 id; 0 is no group (0008 decision 10), at most 255."""
    names = [g.name for g in view_layer.lightgroups]
    if len(names) > 255:
        raise RuntimeError(f"astroray sync: {len(names)} light groups, the limit is 255")
    return {n: i + 1 for i, n in enumerate(names)}


def build(depsgraph, keymap, origin=(0.0, 0.0, 0.0), lightgroups=None, material_key=None, cache=None):
    """One pass over depsgraph.object_instances; returns an InstanceTable (S = 1).

    material_key maps a Material (or None for an empty slot) to an ar_key; the default keys by
    original session_uid and uses 0, the default material, for an empty slot. `cache` (an InstanceCache for the
    same keymap, kept by the caller) carries per-object work across builds; without one nothing is kept.
    """
    lightgroups = lightgroups or {}
    cache = InstanceCache() if cache is None else cache
    if cache.light_names != tuple(lightgroups):
        # Renaming a light group rewrites every object's group name without naming any object in an update, so
        # the records (which hold the name) and the own-row entries that rely on them are all dropped.
        for d in (cache.tids, cache.lights, cache.crypto, cache.assets, cache.plain):
            d.clear()
        cache.light_names = tuple(lightgroups)
    if material_key is None:
        def material_key(mat):
            return keymap.key(("material", mat.original.session_uid)) if mat else 0
    # Per-instance work is Python lists only; the per-object fields are gathered by uid once at the end and
    # the matrices and random ids come from one foreach_get each over the same iterator (B-31: Python
    # per-instance numpy writes and Matrix conversions were most of the loop's 150 ms per 10k). An object's own
    # row is a cache hit after its first build; only instance rows (path non-empty) and new objects do the
    # identity work.
    real, key, plain, records = cache.real, keymap.key, cache.plain, cache.tids
    fresh = {}      # original uid -> object, those without a record
    new_plain = {}  # original uid -> entry for the plain cache, set once their records exist
    ents, owner, rids, in_path = [], [], [], []  # per kept instance: (geometry key, object key, identity), uid, random id
    skipped = []  # indices of the iterator's instances that are not geometry
    k = -1
    for k, inst in enumerate(depsgraph.object_instances):
        ob = inst.object
        orig = ob.original
        uid = orig.session_uid
        if not inst.is_instance:
            hit = plain.get(uid)
            if hit is not None:
                if hit:
                    ents.append(hit)
                    owner.append(uid)
                    rids.append(inst.random_id)
                else:
                    skipped.append(k)
                continue
        if ob.type not in GEOMETRY_TYPES:
            if not inst.is_instance:
                new_plain[uid] = ()
            skipped.append(k)
            continue
        path = keys.instance_path(inst)
        ident = keys.object_identity(inst, path, uid)
        ent = (key(keys.geometry_identity(inst, depsgraph, path, real, uid)), key(ident), ident)
        if path:
            in_path.append((len(ents), path[0]))
        else:
            new_plain[uid] = ent
        if uid not in records:
            fresh[uid] = orig
        ents.append(ent)
        owner.append(uid)
        rids.append(inst.random_id)
    seen, n = k + 1, len(ents)
    rows = np.delete(np.arange(seen), skipped) if skipped else slice(None)
    if fresh:
        _remember_objects(fresh, material_key, cache)
    plain.update(new_plain)
    m16 = np.empty(seen * 16, np.float32)
    if seen:
        depsgraph.object_instances.foreach_get("matrix_world", m16)
    xf = _rows(m16.reshape(seen, 16)[rows], origin) if n else np.zeros((0, 12), np.float32)
    index_of = dict(zip(map(itemgetter(2), ents), range(n)))
    by_object = {u: [i] for i, u in enumerate(owner)}
    if len(by_object) != n:  # an object with several rows
        by_object = {}
        for i, u in enumerate(owner):
            by_object.setdefault(u, []).append(i)
    for i, parent in in_path:  # a row is also found through its instancer
        by_object.setdefault(parent, []).append(i)
    for parent in {p for _, p in in_path}:
        by_object[parent].sort()
    arrays = {"geom_keys": np.asarray(list(map(itemgetter(0), ents)), np.uint64),
              "object_keys": np.asarray(list(map(itemgetter(1), ents)), np.uint64),
              "random_ids": np.asarray(rids, np.int32).view(np.uint32)}
    pool, per = [], {}
    if n:
        uids = np.asarray(owner, np.int32)
        per, pick = _object_scalars(uids)
        tid, crypto = list(map(records.__getitem__, owner)), list(map(cache.crypto.__getitem__, owner))
        asset = list(map(cache.assets.__getitem__, owner))
        if cache.PARENTED in asset:  # the asset id follows the top parent's name, which can change unnamed
            import bpy
            at = [i for i, a in enumerate(asset) if a == cache.PARENTED]
            tops = cache.hash_names([_top_parent(bpy.data.objects[int(pick[i])]).name for i in at])
            for i, h in zip(at, tops):
                asset[i] = h
        lut = np.zeros((2, len(cache.table_list)), np.uint32)  # offset and count of each table id
        for t in dict.fromkeys(tid):  # pool in first-met order; instances sharing a table share its offset
            table = cache.table_list[t]
            lut[:, t] = (len(pool) if table else 0, len(table))
            pool.extend(table)
        tid = np.asarray(tid, np.intp)
        lights = list(map(cache.lights.__getitem__, owner)) if lightgroups else None
        per.update({"light_groups": np.asarray([lightgroups.get(g, 0) for g in lights], np.uint8) if lights
                    else np.zeros(n, np.uint8),
                    "crypto_objects": np.asarray(crypto, np.uint32), "crypto_assets": np.asarray(asset, np.uint32),
                    "material_offsets": lut[0][tid], "material_counts": lut[1][tid]})
    for name, dt, w in _FIELDS:
        if name not in arrays:
            arrays[name] = np.ascontiguousarray(per[name], dt) if n else np.zeros((0, w) if w > 1 else 0, dt)
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


_P_F32 = ctypes.POINTER(ctypes.c_float)


def sync_transforms(edit, abi, table, depsgraph, uids, origin=(0.0, 0.0, 0.0)):
    """A plain move: one ar_edit_update_transforms call for the instances of the objects in `uids`
    (original session_uids, as the object or as the instancer parent).

    Returns False, sending nothing, when an affected instance is not in the table (the set of
    instances changed and the caller must rebuild with `build`).
    An object whose only row is its own (not an instancer, not inside an instance) is read from its
    evaluated object, so the cost follows the moved objects (0007 d19: 0.1 ms); instancers and instanced
    objects walk depsgraph.object_instances for their rows. `uids` may be a dict from original session_uid to
    the evaluated object (classify's `evaluated`), which spares a second walk of depsgraph.updates (B-31: on
    S2 that walk visits every ID node, about 0.1 ms); otherwise the evaluated objects come from that walk.
    """
    if isinstance(uids, dict):
        wanted, evaluated = set(uids), uids
    else:
        wanted = set(uids)
        evaluated = {u.id.original.session_uid: u.id for u in depsgraph.updates
                     if u.id.original.id_type == "OBJECT" and u.id.original.session_uid in wanted}
    idx, rows, walk = [], [], set()
    for uid in wanted:
        own = table.index_of.get((uid,))
        ob = evaluated.get(uid)
        if own is not None and ob is not None and table.by_object.get(uid) == [own]:
            idx.append(own)
            rows.append(_matrix_row(ob.matrix_world))
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
            rows.append(_matrix_row(inst.matrix_world))
    if not idx:
        return False
    xf = np.array(rows, np.float32)
    xf[:, 3::4] -= np.asarray(origin, np.float32)
    table.xf[idx] = xf
    c_idx = (ctypes.c_uint32 * len(idx))(*idx)
    c_xf = ctypes.POINTER(abi.ar_xform) if abi else _P_F32
    edit.call("ar_edit_update_transforms", len(idx), c_idx, ctypes.cast(xf.ctypes.data, c_xf))
    return True
