# SPDX-License-Identifier: GPL-3.0-or-later
"""Material tab: the "Astroray Extension" panel (0007 d37, V17; 0005 items 16, 23).

Ref: docs/decisions/0005-material-model.md item 16 (kind and constants are authored on the Material's Astroray
panel), item 23 (the groove direction comes from the node tree, so no direction property); 0007 d37 V17 (every
property changes a render or is greyed out with an INFO line); Blender 5.3 scripts/addons_core/cycles/ui.py
(`COMPAT_ENGINES`, `poll` on the engine). The draw reads properties only and calls no ABI function (0007 d26).
"""
import bpy

# False until the engine renders a grating: the rows are then drawn disabled under one INFO line. The values
# still sync and the engine's own refusal entry is the report. The routing task flips this and deletes the line.
GRATING_RENDERS = False
NOT_YET = "Diffraction gratings do not render in this build yet"
GRATING_ROWS = ("grating_pitch_nm", "grating_depth_nm", "grating_m_min", "grating_m_max", "grating_coating_n",
                "grating_coating_k")


class ASTRORAY_PT_material_extension(bpy.types.Panel):
    bl_label = "Astroray Extension"
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "material"
    COMPAT_ENGINES = {"ASTRORAY"}

    @classmethod
    def poll(cls, context):
        return context.engine in cls.COMPAT_ENGINES and context.material is not None

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        props = context.material.astroray
        layout.prop(props, "ext_kind")
        if props.ext_kind != "GRATING":
            return
        if not GRATING_RENDERS:
            layout.label(text=NOT_YET, icon="INFO")
        col = layout.column()
        col.enabled = GRATING_RENDERS
        for name in GRATING_ROWS:
            col.prop(props, name)
            if name == "grating_pitch_nm":
                col.label(text=f"{1e6 / props.grating_pitch_nm:.1f} lines/mm")


classes = (ASTRORAY_PT_material_extension,)
