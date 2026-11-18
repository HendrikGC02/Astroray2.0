# SPDX-License-Identifier: GPL-3.0-or-later
"""The scene's World to a WORLD graph and ar_world_desc (0005 items 29, 32; 0007 d20).

The constant case (Background colour x strength) is lowered by materials/translate.py through
the same graph emitter as materials; anything else carries an UNSUPPORTED op, which the engine
reports (067) and renders as constant magenta radiance. Mist settings are not forwarded: the Mist
pass has no consumer yet (0008 d10), so the three mist fields stay 0.
Ref: docs/decisions/0007-appendix-abi.md ar_world_desc; materials/translate.py.
"""
import ctypes

from ..materials import translate


def sync(edit, abi, world, keymap, op_id):
    """Sends an evaluated World; returns its translation, or None when the scene has no world
    (the engine's world is then absent: black)."""
    if world is None:
        return None
    em = translate.translate_world(world, keymap, op_id)
    graph_key = keymap.key(("graph", "WORLD", world.original.session_uid))
    desc, keep = em.to_desc(abi)
    edit.call("ar_edit_set_graph", graph_key, abi.AR_TARGET_WORLD, ctypes.byref(desc))
    w = abi.ar_world_desc()
    w.surface_graph = graph_key
    edit.call("ar_edit_set_world", ctypes.byref(w))
    del keep
    return em
