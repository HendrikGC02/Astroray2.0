# SPDX-License-Identifier: GPL-3.0-or-later
"""Identity tuples and the ar_key map (0007 d18).

Ref: Cycles intern/cycles/blender/util.h, BObjectInfo::is_real_object_data()
(Apache-2.0, https://projects.blender.org/blender/blender, read 2026-11-15): geometry is the
object's own data only when the data the depsgraph iterator yields is the datablock Blender
evaluates for that object's original data; otherwise it is per-object (modifiers, geometry
nodes, instanced geometry sets) and the object identity must be part of its key.
Evaluated IDs report session_uid 0 or per-copy values (spike B), so only originals are used.
"""


class KeyMap:
    """Per-scene map from identity tuples to ar_key. A counter, not a hash: no collisions."""

    def __init__(self):
        self._next = 1  # 0 is never a valid key
        self._map = {}

    def key(self, identity):
        k = self._map.get(identity)
        if k is None:
            k = self._next
            self._next += 1
            self._map[identity] = k
        return k

    def __len__(self):
        return len(self._map)


def image_key(keymap, image):
    """The ar_key of an image: its original's session_uid. translate.py (TEX_IMAGE) and sync/images.py
    both call this, so a texture node and the image the engine holds can never disagree."""
    return keymap.key(("image", image.original.session_uid))


def is_real_object_data(inst, depsgraph):
    """True when inst.object.data is the shared evaluated copy of its original data."""
    data = inst.object.data
    orig_data = inst.object.original.data
    if data is None or orig_data is None:
        return False
    return data.as_pointer() == orig_data.evaluated_get(depsgraph).as_pointer()


def instance_path(inst):
    """(parent original uid, persistent_id): persistent_id is relative to the parent, so two
    instancers of one collection give the same ids and only the parent tells them apart.
    Ref: Cycles ObjectKey(parent, persistent_id, ...) in intern/cycles/blender/object.cpp."""
    if not inst.is_instance:
        return ()
    return (inst.parent.original.session_uid, tuple(inst.persistent_id))


def object_identity(inst, path=None, uid=None):
    """Original object session_uid, plus the instance path (parent, persistent_id) for instances.
    `path` is instance_path(inst) and `uid` the original session_uid when the caller already has them."""
    if uid is None:
        uid = inst.object.original.session_uid
    return (uid,) + (instance_path(inst) if path is None else path)


def geometry_identity(inst, depsgraph, path=None, real=None):
    """None for objects without data; else a tuple that is equal exactly when the geometry is.
    `real`, if given, caches is_real_object_data per (original uid, evaluated data pointer): the answer
    depends only on those, and geometry-nodes instances reuse one temporary object for different data."""
    orig = inst.object.original
    if orig.data is None:
        return None
    if real is None:
        is_real = is_real_object_data(inst, depsgraph)
    else:
        data = inst.object.data
        k = (orig.session_uid, data.as_pointer() if data is not None else 0)
        is_real = real.get(k)
        if is_real is None:
            is_real = real[k] = is_real_object_data(inst, depsgraph)
    if is_real:
        return ("data", orig.data.session_uid)
    return ("object", orig.session_uid) + (instance_path(inst) if path is None else path)