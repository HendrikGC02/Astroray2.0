# SPDX-License-Identifier: GPL-3.0-or-later
"""Viewport present: the engine's RGBA16F display frame onto one persistent texture (0007 d29, d30).

Ref: docs/decisions/0007-blender-integration.md d29 (the path: storage-buffer stripes, a 16x16
compute shader with unpackHalf2x16 into a persistent RGBA16F GPUTexture, draw_texture_2d inside
bind_display_space_shader with ALPHA_PREMULT), d30 (stripes of at most 16 MiB, reallocated only on a
size change, up to two sizes cached), d25 (a frame older than the last presented one is never
shown); spike B gui_present.py, path B16, for the gpu-module calls (docs/spikes/B-blender-end-to-end
RESULTS.md section 1.1: no barrier is needed between the compute write and the draw).
Only Blender's `gpu` module is used, so one path serves the Vulkan default and OpenGL.
"""
import ctypes

import numpy as np

STRIPE_BYTES = 16 * 1024 * 1024  # 0007 d30
CACHED_SIZES = 2                 # 0007 d30: the resolution controller alternates between two

# One work item per pixel: two 32-bit words are four halves (r, g, b, a).
_UNPACK = """
void main() {
  ivec2 p = ivec2(gl_GlobalInvocationID.xy);
  if (p.x >= W || p.y >= SH) return;
  uvec2 s = src[p.y * W + p.x];
  imageStore(dst, ivec2(p.x, p.y + Y0), vec4(unpackHalf2x16(s.x), unpackHalf2x16(s.y)));
}
"""


def _compute_shader():
    import gpu
    info = gpu.types.GPUShaderCreateInfo()
    info.local_group_size(16, 16, 1)
    info.storage_buf(0, "uvec2", "src[]", qualifiers={"READ"})
    info.image(0, "RGBA16F", "FLOAT_2D", "dst", qualifiers={"WRITE"})
    for name in ("W", "SH", "Y0"):
        info.push_constant("INT", name)
    info.compute_source(_UNPACK)
    return gpu.shader.create_from_info(info)


def frame_words(frame):
    """The frame's pinned host memory as uint32 words, no copy; valid until ar_display_release."""
    n = frame.row_bytes * frame.height // 4
    addr = ctypes.cast(frame.data, ctypes.c_void_p).value
    return np.ctypeslib.as_array((ctypes.c_uint32 * n).from_address(addr))


class _Target:
    """The texture and stripe buffers of one frame size."""

    def __init__(self, width, height, row_words):
        import gpu
        self.width, self.height = width, height
        self.texture = gpu.types.GPUTexture((width, height), format="RGBA16F")
        rows = max(1, STRIPE_BYTES // (row_words * 4))
        self.stripes = [(r0, min(height, r0 + rows)) for r0 in range(0, height, rows)]
        self.buffers = None  # created by the first upload, which has the data to size them from


class Present:
    """Uploads display frames and draws the newest one. No instance is shared between viewports."""

    def __init__(self):
        self._shader = None
        self._targets = []   # most recently used last
        self._shown = None   # the target holding the frame on screen
        self.generation = -1  # generation of the frame on screen; -1 before the first
        self.uploads = 0

    def wants(self, frame):
        """d25: only a frame of generation >= the one on screen may replace it."""
        return frame.generation >= self.generation

    def upload(self, frame):
        import gpu
        words = frame_words(frame)
        row_words = frame.row_bytes // 4
        target = self._target(frame.width, frame.height, row_words)
        if self._shader is None:
            self._shader = _compute_shader()
        sh = self._shader
        sh.bind()
        sh.image("dst", target.texture)
        sh.uniform_int("W", frame.width)
        first = target.buffers is None
        if first:
            target.buffers = []
        for i, (r0, r1) in enumerate(target.stripes):
            part = gpu.types.Buffer("UINT", (r1 - r0) * row_words, words[r0 * row_words:r1 * row_words])
            if first:
                target.buffers.append(gpu.types.GPUStorageBuf(part))
            else:
                target.buffers[i].update(part)
            sh.storage_block("src", target.buffers[i])
            sh.uniform_int("SH", r1 - r0)
            sh.uniform_int("Y0", r0)
            gpu.compute.dispatch(sh, (frame.width + 15) // 16, (r1 - r0 + 15) // 16, 1)
        self._shown = target
        self.generation = frame.generation
        self.uploads += 1

    def _target(self, width, height, row_words):
        for t in self._targets:
            if (t.width, t.height) == (width, height):
                self._targets.remove(t)
                self._targets.append(t)
                return t
        t = _Target(width, height, row_words)
        self._targets.append(t)
        del self._targets[:-CACHED_SIZES]
        return t

    def draw(self, engine, scene, width, height):
        """Nothing before the first frame; afterwards always the last frame shown."""
        if self._shown is None:
            return
        import gpu
        from gpu_extras.presets import draw_texture_2d
        gpu.state.blend_set("ALPHA_PREMULT")
        engine.bind_display_space_shader(scene)
        try:
            draw_texture_2d(self._shown.texture, (0, 0), width, height)
        finally:
            engine.unbind_display_space_shader()
            gpu.state.blend_set("NONE")

    def free(self):
        self._targets.clear()
        self._shown = None
        self._shader = None
