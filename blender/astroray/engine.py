# SPDX-License-Identifier: GPL-3.0-or-later
"""The Astroray render engine registration (0007 d2). No Python threads."""
import bpy

_MSG = "Astroray: not implemented until task {}"


class AstrorayRenderEngine(bpy.types.RenderEngine):
    bl_idname = "ASTRORAY"
    bl_label = "Astroray"
    bl_use_shading_nodes_custom = False
    bl_use_preview = True
    bl_use_eevee_viewport = True
    bl_use_postprocess = True
    bl_use_gpu_context = False
    bl_write_viewport_depth = False

    def update(self, data, depsgraph):
        self.report({"ERROR"}, _MSG.format("084"))

    def render(self, depsgraph):
        self.report({"ERROR"}, _MSG.format("084"))

    def view_update(self, context, depsgraph):
        self.report({"ERROR"}, _MSG.format("085"))

    def view_draw(self, context, depsgraph):
        self.report({"ERROR"}, _MSG.format("085"))


def register():
    bpy.utils.register_class(AstrorayRenderEngine)


def unregister():
    bpy.utils.unregister_class(AstrorayRenderEngine)
