# SPDX-License-Identifier: GPL-3.0-or-later
"""The Astroray render engine registration (0007 d2). No Python threads.

F12 (0007 d23, d27, d28, d48): update() syncs the view layer into this instance's engine scene on
the job thread, render() runs a session on it. The viewport (d23, d25, d26): view_update and
view_draw drive a viewport.ViewportJob stored on the instance like the F12 job.
"""
import bpy

from . import native, render_final, viewport


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
        self._contained = False

    def __del__(self):
        # As Cycles' engine does, the job lives on the instance. The attribute is unreachable only
        # when Blender removed the struct first (unregister), and shutdown() has closed every job then.
        try:
            job = self._job
        except (AttributeError, ReferenceError):
            return
        if isinstance(job, viewport.ViewportJob):
            job.park()  # two-step destruction (d23): close now, a timer destroys
        elif job is not None:
            job.close()

    def update(self, data, depsgraph):
        self._failure = None
        try:
            if native.cuda_lost() and depsgraph.scene.astroray.device != "CPU":
                # 0007 d44: CUDA is gone for this process. A scene saved with CUDA selected matches no item
                # of the CPU-only dropdown; it is refused here, loudly, never run on the CPU unasked.
                raise render_final.RenderFailed(native.cuda_lost())
            if self._job is None:
                self._job = render_final.Job()
            self._job.update(depsgraph)
        except Exception as e:  # reported now; render() raises the same text
            job = self._job
            lost = job is not None and native.note_failure(job.lib, job.abi, str(e))
            self._failure = lost or str(e)
            self.report({"ERROR"}, self._failure)

    def render(self, depsgraph):
        if self._failure is not None:
            raise render_final.RenderFailed(self._failure)
        try:
            self._job.render(self)
        except Exception as e:
            self.report({"ERROR"}, native.note_failure(self._job.lib, self._job.abi, str(e)) or str(e))
            raise

    def view_update(self, context, depsgraph):
        if self._job is None:
            self._job = viewport.ViewportJob()
        self._job.view_update(self, context, depsgraph)
        self._contain()

    def view_draw(self, context, depsgraph):
        if self._job is not None:
            self._job.view_draw(self, context, depsgraph)
            self._contain()

    def _contain(self):
        """A viewport job that failed with a GPU error (0001 D2.5): report the full text once and take
        CUDA off the device list (0007 d44). The job keeps its failure, so Blender stays usable."""
        job = self._job
        if self._contained or not job.failure:
            return
        self._contained = True
        message = native.note_failure(job.lib, job.abi, job.failure, job.session)
        if message is not None:
            self.report({"ERROR"}, message)


def register():
    bpy.utils.register_class(AstrorayRenderEngine)


def unregister():
    bpy.utils.unregister_class(AstrorayRenderEngine)
    render_final.shutdown()
