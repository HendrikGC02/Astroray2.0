# SPDX-License-Identifier: GPL-3.0-or-later
"""The Astroray render engine registration (0007 d2). No Python threads.

F12 (0007 d23, d27, d28, d48): update() syncs the view layer into this instance's engine scene on
the job thread, render() runs a session on it. The viewport lands in task 085.
"""
import bpy

from . import render_final

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

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._job = None
        self._failure = None

    def __del__(self):
        # As Cycles' engine does, the job lives on the instance. The attribute is unreachable only
        # when Blender removed the struct first (unregister), and shutdown() has closed every job then.
        try:
            job = self._job
        except (AttributeError, ReferenceError):
            return
        if job is not None:
            job.close()

    def update(self, data, depsgraph):
        self._failure = None
        try:
            if self._job is None:
                self._job = render_final.Job()
            self._job.update(depsgraph)
        except Exception as e:  # reported now; render() raises the same text
            self._failure = str(e)
            self.report({"ERROR"}, self._failure)

    def render(self, depsgraph):
        if self._failure is not None:
            raise render_final.RenderFailed(self._failure)
        try:
            self._job.render(self)
        except Exception as e:
            self.report({"ERROR"}, str(e))
            raise

    def view_update(self, context, depsgraph):
        self.report({"ERROR"}, _MSG.format("085"))

    def view_draw(self, context, depsgraph):
        self.report({"ERROR"}, _MSG.format("085"))


def register():
    bpy.utils.register_class(AstrorayRenderEngine)


def unregister():
    bpy.utils.unregister_class(AstrorayRenderEngine)
    render_final.shutdown()
