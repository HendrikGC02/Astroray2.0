# SPDX-License-Identifier: GPL-3.0-or-later
"""State the sync functions keep between depsgraph updates (0007 d16, d19)."""
from collections import Counter

from .keys import KeyMap


class SyncCache:
    def __init__(self):
        self.keys = KeyMap()
        self.settings_sig = None     # render-settings signature at the last sync (0007 d20)
        self.slot_hash = {}          # original object session_uid -> material-slot hash
        self.instance_keys = None    # frozenset of object identities at the last sync
        self.diagnostics = Counter() # host diagnostics for the Diagnostics output (0001 D2.11)

    def remember(self, depsgraph, scene):
        """Record what classification compares against; called after a successful sync."""
        from .classify import settings_signature, slot_signature  # classify never imports cache

        self.settings_sig = settings_signature(scene)
        self.slot_hash = {o.session_uid: slot_signature(o) for o in scene.objects}
        self.instance_keys = frozenset(
            (i.object.original.session_uid, tuple(i.persistent_id) if i.is_instance else ())
            for i in depsgraph.object_instances)
