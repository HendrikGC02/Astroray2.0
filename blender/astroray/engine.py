# SPDX-License-Identifier: GPL-3.0-or-later
"""The Astroray render engine registration (0007 d2). No Python threads.

F12 (0007 d23, d27, d28, d48): update() syncs the view layer into this instance's engine scene on
the job thread, render() runs a session on it. The viewport lands in task 085.
"""
import bpy

from . import render_final

_MSG = "Astroray: not implemented until task {}"
_jobs = {}  # id(engine instance) -> render_final.Job (see __del__)


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

    def __del__(self):
        # The struct is already gone here, so the job is found by identity, not by attribute.
        job = _jobs.pop(id(self), None)
        if job is not None:
            job.close()

    def update(self, data, depsgraph):
        try:
            job = _jobs.get(id(self))
            if job is None:
                job = _jobs[id(self)] = render_final.Job()
            job.update(depsgraph)
        except Exception as e:  # reported; render() then has no synced scene to run
            failed = _jobs.pop(id(self), None)
            if failed is not None:
                failed.close()
            self.report({"ERROR"}, str(e))
            raise

    def render(self, depsgraph):
        try:
            _jobs[id(self)].render(self, depsgraph)
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
