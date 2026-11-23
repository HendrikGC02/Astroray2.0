# SPDX-License-Identifier: GPL-3.0-or-later
"""Heat-map display modes: the engine's RGBA8 sRGB frame drawn outside the view transform.

Route (probe P4): drawing in view_draw without bind_display_space_shader was tried first and
Blender still applied the view transform (an AgX screenshot of a uniform viridis(1.0) frame came out as a
desaturated yellow, 99/255 off), so the frame is drawn from a SpaceView3D POST_PIXEL handler, which runs
after the view transform.
Ref: docs/decisions/0007-blender-integration.md d40 (heat maps draw so the user's view transform cannot
distort them; P4 tries `view_draw` without `bind_display_space_shader` first, a `SpaceView3D` POST_PIXEL
handler otherwise), d30 (the single 8-bit exception to RGBA16F, ruling 57), V10 (exact sRGB under AgX).
The engine already encodes sRGB, so the bytes are the pixel values the screen must show. Blender's
overlay step then maps them (decode as sRGB, encode with 1/2.2), which tests/blender/gui_batching.py
checks against its source: gpu_viewport.cc (overlay texture is SRGBA_8_8_8_8) and
imbuf/opencolorio/shaders/gpu_shader_display_transform_frag.glsl (overlay merge).
"""
import ctypes

import numpy as np

ROUTE = "SpaceView3D POST_PIXEL handler"  # P4: the simpler route failed, see the module docstring


def check_layout(frame):
    from . import _abi  # staged by the build, never committed
    if frame.format != _abi.AR_DISPLAY_FORMAT_DIAG_RGBA8:
        raise ValueError(f"astroray: heat-map frame format {frame.format} is not RGBA8")
    if frame.row_bytes != frame.width * 4:
        raise ValueError(f"astroray: heat-map frame row_bytes {frame.row_bytes} is not width*4 {frame.width * 4}")


def frame_bytes(frame):
    """The frame's pinned host memory as (height, width, 4) uint8, no copy; valid until ar_display_release."""
    check_layout(frame)
    n = frame.row_bytes * frame.height
    addr = ctypes.cast(frame.data, ctypes.c_void_p).value
    return np.ctypeslib.as_array((ctypes.c_uint8 * n).from_address(addr)).reshape(frame.height, frame.width, 4)


class HeatmapDraw:
    """One texture of the last heat-map frame. No instance is shared between viewports."""

    def __init__(self):
        self._texture = None
        self._handler = None
        self.region = 0     # as_pointer() of the region view_draw last asked to show the heat map in; 0 = none
        self.size = (0, 0)
        self.uploads = 0

    def upload(self, frame):
        import gpu
        px = frame_bytes(frame)
        # RGBA8 texture filled from floats k/255: the GPU converts back to exactly k.
        flat = (px.astype(np.float32) * np.float32(1.0 / 255.0)).reshape(-1)
        self._texture = gpu.types.GPUTexture((frame.width, frame.height), format="RGBA8",
                                             data=gpu.types.Buffer("FLOAT", flat.size, flat))
        self._texture.filter_mode(False)  # nearest: a heat map's pixels are values, never blended
        self.uploads += 1

    def show(self, region, width, height):
        """view_draw: this region shows the heat map from now on (the handler does the drawing)."""
        import bpy
        self.region, self.size = region.as_pointer(), (width, height)
        if self._handler is None:
            self._handler = bpy.types.SpaceView3D.draw_handler_add(self._draw_post_pixel, (), "WINDOW", "POST_PIXEL")

    def hide(self):
        self.region = 0

    def _draw_post_pixel(self):
        import bpy
        if self._texture is None or bpy.context.region.as_pointer() != self.region:
            return
        import gpu
        from gpu_extras.presets import draw_texture_2d
        gpu.state.blend_set("NONE")
        draw_texture_2d(self._texture, (0, 0), *self.size)

    def free(self):
        if self._handler is not None:
            import bpy
            bpy.types.SpaceView3D.draw_handler_remove(self._handler, "WINDOW")
            self._handler = None
        self.region = 0
        self._texture = None
