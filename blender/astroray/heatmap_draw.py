# SPDX-License-Identifier: GPL-3.0-or-later
"""Heat-map display modes: the engine's RGBA8 sRGB frame drawn outside the view transform.

Route (probe P4, task 086): ROUTE is filled in by the probe's result, see below.
Ref: docs/decisions/0007-blender-integration.md d40 (heat maps draw so the user's view transform cannot
distort them; P4 tries `view_draw` without `bind_display_space_shader` first, a `SpaceView3D` POST_PIXEL
handler otherwise), d30 (the single 8-bit exception to RGBA16F, ruling 57), V10 (exact sRGB under AgX).
The engine already encodes sRGB, so the bytes are the pixel values the screen must show.
"""
import ctypes

import numpy as np

ROUTE = "view_draw without bind_display_space_shader"  # P4: chosen route, see Progress of task 086


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
        self.uploads = 0

    def upload(self, frame):
        import gpu
        px = frame_bytes(frame)
        # RGBA8 texture filled from floats k/255: the GPU converts back to exactly k.
        flat = (px.astype(np.float32) * np.float32(1.0 / 255.0)).reshape(-1)
        self._texture = gpu.types.GPUTexture((frame.width, frame.height), format="RGBA8",
                                             data=gpu.types.Buffer("FLOAT", flat.size, flat))
        self.uploads += 1

    def draw(self, width, height):
        if self._texture is None:
            return
        import gpu
        from gpu_extras.presets import draw_texture_2d
        gpu.state.blend_set("NONE")
        draw_texture_2d(self._texture, (0, 0), width, height)

    def free(self):
        self._texture = None
