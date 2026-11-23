# SPDX-License-Identifier: GPL-3.0-or-later
"""Blender images to ar_edit_set_image (0007 d21).

Ref: docs/decisions/0007-blender-integration.md d21 (FILE when file-backed, not packed and not dirty;
MEMORY for packed files; PIXELS for generated or dirty images; colour space by
`colorspace_settings.interop_id`; the engine's cache key carries a reload counter);
docs/decisions/0007-appendix-abi.md (ar_image_desc, AR_COLOR_INTEROP_*). Blender behaviour from its
Python API: Image.filepath_from_user(), Image.packed_file.data, Image.is_dirty, Image.alpha_mode.
Generated and edited images go as PIXELS through ar_edit_image_begin (0007 d21); UDIM and mips wait for M3.

An image is keyed by sync.keys.image_key, the same key translate.py gives a TEX_IMAGE node.
Run images after the world and before materials (0007 d20): a TEX_IMAGE names its image only by that key.
Every constant sent to the engine is read from the generated binding `abi`, never copied.
"""
import ctypes

import numpy as np

from . import keys
from .edit import EditError

# Blender 5.3 interop id -> name of the AR_COLOR_INTEROP_* constant (include/astroray/astroray.h). The
# engine decodes sRGB, linear Rec.709 and Data in M1a and renders the others magenta with one report
# entry (0007 V19).
INTEROP = {
    "srgb_rec709_display": "AR_COLOR_INTEROP_SRGB_REC709", "srgb_rec709_scene": "AR_COLOR_INTEROP_SRGB_REC709",
    "lin_rec709_scene": "AR_COLOR_INTEROP_LIN_REC709", "lin_rec2020_scene": "AR_COLOR_INTEROP_LIN_REC2020",
    "lin_ap1_scene": "AR_COLOR_INTEROP_LIN_AP1", "lin_ap0_scene": "AR_COLOR_INTEROP_LIN_AP0",
    "srgb_p3d65_display": "AR_COLOR_INTEROP_SRGB_P3D65", "data": "AR_COLOR_INTEROP_DATA",
}
# An id Blender has and the engine has no constant for: any value outside the AR_COLOR_INTEROP_* range
# makes the engine report it and render magenta.
UNKNOWN_INTEROP = 0xFFFFFFFF
ALPHA = {"STRAIGHT": "AR_ALPHA_STRAIGHT", "PREMUL": "AR_ALPHA_PREMULTIPLIED",
         "CHANNEL_PACKED": "AR_ALPHA_CHANNEL_PACKED", "NONE": "AR_ALPHA_IGNORE"}
_IMAGE_NODES = frozenset(("ShaderNodeTexImage", "ShaderNodeTexEnvironment"))


class ImageState:
    """Per-job reload counts by original session_uid. Blender has no reload counter, so a depsgraph
    update of the Image datablock bumps ours (Image.update_tag(), a changed filepath, colour space or
    alpha mode deliver one; Image.reload() alone delivers updates only for the Image's users, so it
    does not). The engine's cache key includes the count, so a file rewritten within the timestamp
    resolution still decodes again."""

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
    d.color_interop = getattr(abi, INTEROP[iid]) if iid in INTEROP else UNKNOWN_INTEROP
    d.alpha_mode = getattr(abi, ALPHA[image.alpha_mode])
    d.reload_count = reload_count
    return d, iid


def _send_pixels(edit, abi, image, key, d, line):
    """A generated or edited image: Image.pixels (float RGBA, rows bottom-up) copied once into the buffer
    ar_edit_image_begin stages. A byte image's pixels are the encoded values of its colour space, so its
    interop id goes as is and the engine applies the transfer; a float image's are linear values (the
    engine reads half and float images as linear, 0005 item 34), so an sRGB id goes as linear Rec.709."""
    w, h = image.size
    n = w * h * 4
    if n == 0 or len(image.pixels) != n:
        return f"image '{image.name}': has no readable pixels ({w} x {h}, {len(image.pixels)} values) and renders magenta"
    if image.is_float and d.color_interop == abi.AR_COLOR_INTEROP_SRGB_REC709:
        d.color_interop = abi.AR_COLOR_INTEROP_LIN_REC709
    d.source = abi.AR_IMAGE_SOURCE_PIXELS
    d.width, d.height = w, h
    buffers = abi.ar_image_buffers()
    edit.call("ar_edit_image_begin", ctypes.byref(d), ctypes.byref(buffers))
    if buffers.pixels_count != n:
        raise EditError(f"ar_edit_image_begin staged {buffers.pixels_count} floats for a {w} x {h} image")
    # One copy into the staged address; no view of engine memory outlives the commit.
    staged = (ctypes.c_float * n).from_address(ctypes.cast(buffers.pixels[0], ctypes.c_void_p).value)
    view = np.frombuffer(staged, np.float32)
    image.pixels.foreach_get(view)
    del view, staged
    edit.item_commit(key)
    return line


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
        return _send_pixels(edit, abi, image, key, d, line)
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
