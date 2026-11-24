# SPDX-License-Identifier: GPL-3.0-or-later
"""Built-in panels that gain "ASTRORAY" in COMPAT_ENGINES: the one list 0007 d37 names.

A panel is listed only when the engine already honours every property it draws:
 - camera: DATA_PT_context_camera (the camera selector), DATA_PT_lens (type, focal length, shift, clip;
   sync/camera.py), DATA_PT_camera (sensor size and fit), DATA_PT_camera_dof and DATA_PT_camera_dof_aperture
   (sync/camera.py reads `cam.dof` into dof_enabled, fstop, focus_distance, blades, rotation, ratio);
 - output: RENDER_PT_format (resolution and percentage; sync/settings.py output_width/height);
 - colour management: RENDER_PT_color_management (view transform, look, exposure, gamma; read from the scene).
Film transparency has no panel of its own in Blender 5.3 (it sits in EEVEE's Film panel beside filter
sizes the engine ignores), so ui/panels.py draws that row itself. The curves, working-space and white
balance subpanels are left out: nothing reads them.

Ref: docs/decisions/0007-blender-integration.md d37; Blender 5.3 scripts/addons_core/cycles/ui.py
(`panel.COMPAT_ENGINES.add`, removed again on unregister).
"""
import bpy

ENGINE = "ASTRORAY"

PANELS = (
    "DATA_PT_context_camera",
    "DATA_PT_lens",
    "DATA_PT_camera",
    "DATA_PT_camera_dof",
    "DATA_PT_camera_dof_aperture",
    "RENDER_PT_format",
    "RENDER_PT_color_management",
)

_added = []  # the panel classes this module put the engine into, so unregister removes exactly those


def register():
    for name in PANELS:
        panel = getattr(bpy.types, name, None)
        if panel is None:  # a Blender that renamed one must fail here, never silently lose the panel
            raise RuntimeError(f"astroray: the built-in panel {name} does not exist in this Blender")
        if ENGINE not in panel.COMPAT_ENGINES:
            panel.COMPAT_ENGINES.add(ENGINE)
            _added.append(panel)


def unregister():
    while _added:
        _added.pop().COMPAT_ENGINES.discard(ENGINE)
