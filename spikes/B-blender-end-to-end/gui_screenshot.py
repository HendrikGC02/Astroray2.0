"""Displays one frozen native test frame through the measured present path (A32: per-frame RGBA32F texture) inside
bind_display_space_shader, for ~14 s, so the launcher can screenshot the window. Quits itself.
Pattern (see sb.cpp): red = 1.5*u (HDR above 1.0), green = 0.5+0.5*sin, blue = 1-u+..., checker dims by 0.5."""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import gpu
from gpu_extras.presets import draw_texture_2d
import gui_common as G
import sbn


class Shot:
    done = False
    fb = None
    t0 = None

    def view_update(self, eng, ctx, dg):
        pass

    def view_draw(self, eng, ctx, dg):
        if self.fb is None:
            self.fb = sbn.Framebuffer(1920, 1080, sbn.FMT_F32, 60.0, 3)
            time.sleep(0.3)
            self.fb.pause(True)
            time.sleep(0.1)
            self.t0 = time.perf_counter()
            G.OUT["frame_counter"] = int(self.fb.counter())
            G.save()
        a = self.fb.front_array().reshape(-1)
        tex = gpu.types.GPUTexture((1920, 1080), format='RGBA32F', data=gpu.types.Buffer('FLOAT', a.size, a))
        gpu.state.blend_set('ALPHA_PREMULT')
        eng.bind_display_space_shader(dg.scene)
        draw_texture_2d(tex, (0, 0), ctx.region.width, ctx.region.height)
        eng.unbind_display_space_shader()
        gpu.state.blend_set('NONE')
        G.OUT["region"] = [ctx.region.width, ctx.region.height]
        # pixel values of the source row 540 at a few u for later comparison with the screenshot
        row = self.fb.front_array()[540]
        G.OUT["src_row540_rgb_at_u"] = {str(u): [float(x) for x in row[int(u * 1919)][:3]] for u in (0.1, 0.2, 0.4, 0.6, 0.8, 0.95)}
        if time.perf_counter() - self.t0 > 14.0:
            self.done = True
        eng.tag_redraw()


kv = G.parse_args()
G.WATCHDOG_S = 60.0
G.start(Shot(), os.path.join(G.HERE, "out", "gui_screenshot.json"), kv)
