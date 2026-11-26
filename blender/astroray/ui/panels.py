# SPDX-License-Identifier: GPL-3.0-or-later
"""Render-properties panels: the Astroray settings and the Diagnostics subpanel (0007 d37, d38).

Ref: docs/decisions/0007-blender-integration.md d37 (Cycles-like layout, tooltips on the properties) and d38
(the report is shown in a Diagnostics subpanel); d26 (draw reads the store and calls no ABI function);
Blender 5.3 scripts/addons_core/cycles/ui.py (a parent panel with child panels in the Render tab,
`COMPAT_ENGINES`, `use_property_split`).
"""
import textwrap

import bpy

from .. import diagnostics, native

# Blender's "ERROR" icon is the warning triangle; the cross is the error mark.
_ICONS = {diagnostics.ERROR: "CANCEL", diagnostics.WARNING: "ERROR", diagnostics.INFO: "INFO"}
_LEVEL_WORD = {diagnostics.ERROR: "Error", diagnostics.WARNING: "Warning", diagnostics.INFO: "Info"}
_DEFAULT_CHARS = 38  # a default-width Properties editor, in characters of the UI font
_PIXELS_PER_CHAR = 7.5


class AstrorayPanel:
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "render"
    COMPAT_ENGINES = {"ASTRORAY"}

    @classmethod
    def poll(cls, context):
        return context.engine in cls.COMPAT_ENGINES


def wrap_width(context):
    """Characters that fit the region; a context without a region (a test) gets the default width."""
    region = getattr(context, "region", None)
    if region is None:
        return _DEFAULT_CHARS
    scale = context.preferences.system.ui_scale if getattr(context, "preferences", None) else 1.0
    return max(16, int(region.width / (_PIXELS_PER_CHAR * scale)) - 6)


def header_text(counts):
    e, w = counts[diagnostics.ERROR], counts[diagnostics.WARNING]
    return f"{e} {'error' if e == 1 else 'errors'}, {w} {'warning' if w == 1 else 'warnings'}"


def wrapped(text, width):
    return textwrap.wrap(text, width, break_long_words=True) or [""]


class ASTRORAY_PT_settings(AstrorayPanel, bpy.types.Panel):
    bl_label = "Astroray"

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        layout.prop(context.scene.astroray, "device")


class ASTRORAY_PT_sampling(AstrorayPanel, bpy.types.Panel):
    bl_label = "Sampling"
    bl_parent_id = "ASTRORAY_PT_settings"

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        a = context.scene.astroray
        col = layout.column(align=True)
        col.prop(a, "samples", text="Render")
        col.prop(a, "viewport_samples", text="Viewport")
        col = layout.column(align=True)
        col.prop(a, "seed")
        col.prop(a, "use_animated_seed", text="Animate Seed")


class ASTRORAY_PT_light_paths(AstrorayPanel, bpy.types.Panel):
    bl_label = "Light Paths"
    bl_parent_id = "ASTRORAY_PT_settings"
    bl_options = {"DEFAULT_CLOSED"}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        a = context.scene.astroray
        col = layout.column(align=True)
        col.prop(a, "max_bounces", text="Max Bounces")
        col.prop(a, "min_bounces", text="Min Bounces")


class ASTRORAY_PT_film(AstrorayPanel, bpy.types.Panel):
    bl_label = "Film"
    bl_parent_id = "ASTRORAY_PT_settings"
    bl_options = {"DEFAULT_CLOSED"}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        a = context.scene.astroray
        layout.prop(a, "film_exposure", text="Exposure")
        col = layout.column(align=True)
        col.prop(a, "pixel_filter_type", text="Filter")
        col.prop(a, "pixel_filter_width", text="Width")
        # The engine reads render.film_transparent (sync/settings.py); EEVEE's own Film panel is not shown for
        # this engine, so the row lives here.
        layout.prop(context.scene.render, "film_transparent", text="Transparent")


class ASTRORAY_PT_denoise(AstrorayPanel, bpy.types.Panel):
    bl_label = "Denoise"
    bl_parent_id = "ASTRORAY_PT_settings"
    bl_options = {"DEFAULT_CLOSED"}

    def draw_header(self, context):
        self.layout.prop(context.scene.astroray, "use_denoising", text="")

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        a = context.scene.astroray
        col = layout.column()
        col.active = a.use_denoising
        col.prop(a, "denoising_store_passes", text="Denoising Data")


class ASTRORAY_PT_viewport(AstrorayPanel, bpy.types.Panel):
    bl_label = "Viewport"
    bl_parent_id = "ASTRORAY_PT_settings"
    bl_options = {"DEFAULT_CLOSED"}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        layout.prop(context.scene.astroray, "display_mode")


def _identity_rows(layout, report, width):
    ident = diagnostics.identity()
    box = layout.box()
    col = box.column(align=True)
    for line in wrapped(ident["build"] or "build not recorded", width):
        col.label(text=line)
    col.label(text=f"ABI hash: 0x{ident['abi_hash']:016x}" if ident["abi_hash"] is not None else "ABI hash: unknown")
    offered = native.devices()
    col.label(text="Devices offered: " + ", ".join(offered))
    used = report.device if report is not None else None
    col.label(text="Device in use: " + (used or "no render yet"))
    if "CUDA" not in offered:
        reason = native.cuda_lost() or "reason not reported by this build"
        col.label(text="CUDA unavailable:")
        for line in wrapped(reason, width):
            col.label(text="  " + line)


def _entry_rows(layout, entry, width):
    col = layout.column(align=True)
    head = " ".join(p for p in (entry.code, entry.subject) if p) or _LEVEL_WORD[entry.level]
    for i, line in enumerate(wrapped(head, width)):
        if i == 0:
            col.label(text=line, icon=_ICONS[entry.level])
        else:
            col.label(text="  " + line)
    for line in wrapped(entry.detail, width - 2):
        col.label(text="  " + line)


class ASTRORAY_PT_diagnostics(AstrorayPanel, bpy.types.Panel):
    bl_label = "Diagnostics"
    bl_parent_id = "ASTRORAY_PT_settings"

    def draw_header(self, context):
        report = diagnostics.latest()
        if report is None:
            return
        self.layout.label(text=header_text(report.counts()))

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        width = wrap_width(context)
        report = diagnostics.latest()
        _identity_rows(layout, report, width)
        if report is None or not report.entries:
            layout.label(text="no entries")
            return
        layout.label(text=f"Last render: {report.kind.lower()}")
        for entry in report.ordered():
            _entry_rows(layout, entry, width)


classes = (ASTRORAY_PT_settings, ASTRORAY_PT_sampling, ASTRORAY_PT_light_paths, ASTRORAY_PT_film, ASTRORAY_PT_denoise,
           ASTRORAY_PT_viewport, ASTRORAY_PT_diagnostics)
