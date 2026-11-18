# SPDX-License-Identifier: GPL-3.0-or-later
"""Blender images to ar_edit_set_image (0007 d21).

Ref: docs/decisions/0007-blender-integration.md d21 (FILE when file-backed, not packed and not dirty;
MEMORY for packed files; PIXELS for generated or dirty images; colour space by
`colorspace_settings.interop_id`; the engine's cache key carries a reload counter);
docs/decisions/0007-appendix-abi.md (ar_image_desc, AR_COLOR_INTEROP_*). Blender behaviour from its
Python API: Image.filepath_from_user(), Image.packed_file.data, Image.is_dirty, Image.alpha_mode.
Generated, half and float images wait for PIXELS support in the engine (M1b), UDIM and mips for M3.

An image is keyed by its original's session_uid, the same key translate.py gives a TEX_IMAGE node.
Run images before materials: a TEX_IMAGE names its image only by that key.
"""
import ctypes

from . import keys
from .edit import EditError

AR_ERR_UNSUPPORTED = 2

# Blender 5.3 interop ids -> AR_COLOR_INTEROP_* (include/astroray/astroray.h). The engine decodes
# 0, 1 and 6 in M1a and renders the others magenta with one report entry (0007 V19).
INTEROP = {
    "srgb_rec709_display": 0, "srgb_rec709_scene": 0,
    "lin_rec709_scene": 1, "lin_rec2020_scene": 2, "lin_ap1_scene": 3, "lin_ap0_scene": 4,
    "srgb_p3d65_display": 5, "data": 6,
}
UNKNOWN_INTEROP = 0xFFFFFFFF  # an id with no engine constant: the engine reports it and renders magenta
ALPHA = {"STRAIGHT": 0, "PREMUL": 1, "CHANNEL_PACKED": 2, "NONE": 3}  # AR_ALPHA_*
_IMAGE_NODES = frozenset(("ShaderNodeTexImage", "ShaderNodeTexEnvironment"))


class ImageState:
    """Per-job reload counts by original session_uid. Blender has no reload counter of its own, so a
    depsgraph update of an image (which Image.reload() causes) bumps ours; the engine's cache key
    includes it, so a file rewritten within the timestamp resolution still decodes again."""

    def __init__(self):
        self.reloads = {}

    def note_updates(self, actions, kind):
        """`actions` are sync.classify actions; those of `kind` (classify.IMAGES) bump their image."""
        for a in actions:
            if a.kind == kind:
                self.reloads[a.uid] = self.reloads.get(a.uid, 0) + 1


def used_images(trees):
    """Images read by Image Texture and Environment Texture nodes of node trees, groups included,
    each once, in first-use order."""
    seen, out = set(), []

    def walk(tree, visited):
        if tree is None or tree.original.session_uid in visited:
            return
        visited.add(tree.original.session_uid)
        for n in tree.nodes:
            if n.bl_idname in _IMAGE_NODES and n.image is not None:
                uid = n.image.original.session_uid
                if uid not in seen:
                    seen.add(uid)
                    out.append(n.image)
            elif n.bl_idname == "ShaderNodeGroup":
                walk(n.node_tree, visited)

    for t in trees:
        walk(t, set())
    return out


def source_of(image):
    """'FILE', 'MEMORY' or 'PIXELS' for an image, or raises for kinds this milestone cannot send."""
    if image.source in ("TILED", "SEQUENCE", "MOVIE", "VIEWER"):
        raise ValueError(f"image source {image.source} is not supported yet")
    if image.source == "GENERATED" or image.is_dirty:
        return "PIXELS"
    if image.packed_file is not None:
        return "MEMORY"
    return "FILE"


def _desc(abi, key, image, reload_count):
    d = abi.ar_image_desc()
    d.key = key
    iid = image.colorspace_settings.interop_id
    d.color_interop = INTEROP.get(iid, UNKNOWN_INTEROP)
    d.alpha_mode = ALPHA[image.alpha_mode]
    d.reload_count = reload_count
    return d, iid


def sync_image(edit, abi, keymap, image, state):
    """Sends one image. Returns a report line, or None when the engine took it."""
    image = image.original
    key = keys.image_key(keymap, image)
    try:
        src = source_of(image)
    except ValueError as e:
        return f"image '{image.name}': {e}"
    d, iid = _desc(abi, key, image, state.reloads.get(image.session_uid, 0))
    line = None
    if iid not in INTEROP:
        line = f"image '{image.name}': colour space interop id '{iid}' is not supported and renders magenta"
    if src == "PIXELS":
        # The engine refuses PIXELS until M1b; the refusal is expected and reported, so the edit goes on.
        d.source = abi.AR_IMAGE_SOURCE_PIXELS
        d.width, d.height = image.size
        try:
            edit.call("ar_edit_set_image", ctypes.byref(d))
        except EditError as e:
            if f"returned status {AR_ERR_UNSUPPORTED}" not in str(e):
                raise
        return f"image '{image.name}': generated images land in M1b (it renders magenta)"
    if src == "MEMORY":
        data = image.packed_file.data
        d.source = abi.AR_IMAGE_SOURCE_MEMORY
        d.byte_count = len(data)
        d.bytes = ctypes.cast(ctypes.c_char_p(data), ctypes.POINTER(ctypes.c_uint8))
        edit.call("ar_edit_set_image", ctypes.byref(d))
    else:
        path = image.filepath_from_user().encode("utf-8")
        d.source = abi.AR_IMAGE_SOURCE_FILE
        d.path = path
        edit.call("ar_edit_set_image", ctypes.byref(d))
    return line


def sync(edit, abi, keymap, images, state):
    """Every image in `images`; returns the report lines of what the engine cannot take."""
    lines = []
    for im in images:
        line = sync_image(edit, abi, keymap, im, state)
        if line:
            lines.append(line)
    return lines
