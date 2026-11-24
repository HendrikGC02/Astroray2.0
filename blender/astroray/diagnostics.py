# SPDX-License-Identifier: GPL-3.0-or-later
"""The last render's report, kept in plain Python for the Diagnostics panel (0007 d38).

Ref: docs/decisions/0001-product-shape.md D2.11 (report entries are (level, code, subject, detail); the addon
shows each key once per render and lists host-side diagnostics beside the engine's entries without engine
codes); docs/decisions/0007-blender-integration.md d26 (the panel's draw reads this store and calls no ABI
function). Nothing here holds bpy state, so a headless test reads it directly; the bpy lookups sit in
functions that import bpy when they run.

One Report per render kind ("FINAL" for F12, "VIEWPORT"). A Report outlives its session: the panel keeps
showing the last render's entries after the render ends, until the next render of that kind begins.
"""
from collections import namedtuple

ERROR, WARNING, INFO = "ERROR", "WARNING", "INFO"
_RANK = {ERROR: 0, WARNING: 1, INFO: 2}

# code_id orders engine entries as the snapshot does (level, code, subject, detail); host lines carry no code.
Entry = namedtuple("Entry", "level code code_id subject detail host")


def _key(e):
    return (e.level, e.code, e.subject, e.detail)


class Report:
    def __init__(self, kind, device):
        self.kind, self.device = kind, device
        self.entries, self._keys = [], set()

    def add(self, entry):
        """False when the key (level, code, subject, detail) is already listed."""
        key = _key(entry)
        if key in self._keys:
            return False
        self._keys.add(key)
        self.entries.append(entry)
        return True

    def clear_engine(self):
        """A viewport snapshot replaces the engine's entries; the addon's own sync lines stay."""
        self.entries = [e for e in self.entries if e.host]
        self._keys = {_key(e) for e in self.entries}

    def ordered(self):
        """Errors first, then the engine's order within a level; host lines after the engine's entries."""
        return sorted(self.entries, key=lambda e: (_RANK[e.level], e.host, e.code_id, e.subject, e.detail))

    def counts(self):
        return {lv: sum(1 for e in self.entries if e.level == lv) for lv in (ERROR, WARNING, INFO)}


_reports = {}      # kind -> Report
_last = [None]     # the Report written most recently: what the panel shows
_revision = [0]    # bumped by every change, so a caller can tell the panel needs a redraw
_identity = {"build": None, "abi_hash": None}


def begin(kind, device):
    """A new render of `kind` replaces the last one's entries."""
    _reports[kind] = _last[0] = Report(kind, device)
    _revision[0] += 1
    return _reports[kind]


def _current(kind):
    return _reports[kind] if kind in _reports else begin(kind, None)


def _added(kind, entry):
    report = _current(kind)
    if report.add(entry):
        _last[0] = report
        _revision[0] += 1


def add_engine(kind, level, code, code_id, subject, detail):
    _added(kind, Entry(level, code, code_id, subject, detail, False))


def add_host(kind, level, text):
    _added(kind, Entry(level, "", 0, "", text, True))


def clear_engine(kind):
    _current(kind).clear_engine()
    _revision[0] += 1


def latest():
    return _last[0]


def revision():
    return _revision[0]


def reset():
    """Test seam: forgets every report and the identity."""
    _reports.clear()
    _last[0] = None
    _revision[0] += 1
    _identity.update(build=None, abi_hash=None)


def set_identity(build, abi_hash):
    _identity.update(build=build, abi_hash=abi_hash)


def identity():
    return dict(_identity)


# ---- subjects: an entry's subject is a host key (0001 D2.11); the panel shows the datablock's name

def reverse_keys(keymap):
    """ar_key -> identity tuple for a sync.keys.KeyMap."""
    return keymap.reverse()


def uid_names():
    """{(kind, original session_uid): name} for the datablocks a report subject can name. Call it on the
    thread that already reads bpy.data for the sync (the main thread, or the F12 job's update())."""
    import bpy
    out = {}
    for kind, coll in (("material", bpy.data.materials), ("world", bpy.data.worlds),
                       ("image", bpy.data.images), ("object", bpy.data.objects)):
        for idb in coll:
            out[(kind, idb.session_uid)] = idb.name
    return out


def subject_label(reverse, names, key):
    """'' for a scene-wide entry (key 0), else a readable name; an unknown key is shown as the key."""
    if key == 0:
        return ""
    ident = reverse.get(key)
    if ident is None:
        return f"key {key}"
    kind = ident[0]
    if kind == "graph":
        kind, uid = ("material" if ident[1] == "SURFACE" else "world"), ident[2]
    elif kind == "light":
        kind, uid = "object", ident[1]
    else:
        uid = ident[1]
    name = names.get((kind, uid))
    return f"{kind} '{name}'" if name else f"{kind} (removed)"


def tag_properties_redraw():
    """Main thread only (view_draw): the Diagnostics panel lives in a Properties editor."""
    import bpy
    for win in bpy.context.window_manager.windows:
        for area in win.screen.areas:
            if area.type == "PROPERTIES":
                area.tag_redraw()
