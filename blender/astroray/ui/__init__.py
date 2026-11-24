# SPDX-License-Identifier: GPL-3.0-or-later
"""Addon UI: the Astroray panels and the built-in panels that gain this engine (0007 d37)."""
import bpy

from . import compat_panels, panels


def register():
    compat_panels.register()
    for c in panels.classes:
        bpy.utils.register_class(c)


def unregister():
    for c in reversed(panels.classes):
        bpy.utils.unregister_class(c)
    compat_panels.unregister()
