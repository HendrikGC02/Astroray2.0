"""ctypes wrapper for build/sbnative.dll (spike B, throwaway)."""
import ctypes as C
import os
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
DLL_PATH = os.path.join(HERE, "build", "sbnative.dll")
_d = C.CDLL(DLL_PATH)

P, I32, I64, F, D = C.c_void_p, C.c_int, C.c_int64, C.c_float, C.c_double


def _sig(name, res, *args):
    f = getattr(_d, name)
    f.restype, f.argtypes = res, list(args)
    return f


now_ns = _sig("sb_now_ns", D)
fb_create = _sig("sb_fb_create", P, I32, I32, I32, D, I32)
fb_destroy = _sig("sb_fb_destroy", None, P)
fb_front = _sig("sb_fb_front", P, P)
fb_counter = _sig("sb_fb_counter", C.c_uint64, P)
fb_last_ns = _sig("sb_fb_last_ns", D, P)
fb_set_fps = _sig("sb_fb_set_fps", None, P, D)
fb_pause = _sig("sb_fb_pause", None, P, I32)
fb_bytes = _sig("sb_fb_bytes", I64, P)
fb_stats = _sig("sb_fb_stats", None, P, P, I32)
fb_ring = _sig("sb_fb_ring", I32, P, C.c_uint64, I32, P)
scene_create = _sig("sb_scene_create", P)
scene_destroy = _sig("sb_scene_destroy", None, P)
scene_set_mesh = _sig("sb_scene_set_mesh", D, P, I32, P, I64, P, I64, P, I64, P, I64, P, P)
scene_set_mesh_poly = _sig("sb_scene_set_mesh_poly", D, P, I32, P, I64, P, I64, P, I64, P, P, P)
scene_set_objects = _sig("sb_scene_set_objects", D, P, I64, P, P)
scene_set_xform = _sig("sb_scene_set_xform", D, P, I64, P)
scene_set_material = _sig("sb_scene_set_material", D, P, I32, P, I32)
scene_edit_vertex = _sig("sb_scene_edit_vertex", D, P, I32, I64, F, F, F)
scene_add_object = _sig("sb_scene_add_object", D, P, I32, P)
scene_totals = _sig("sb_scene_totals", None, P, P)
gas_init = _sig("sb_gas_init", I32)
gas_build = _sig("sb_gas_build", I32, P, P)
gas_error = _sig("sb_gas_error", C.c_char_p)

FMT_F32, FMT_F16, FMT_U8, FMT_R11 = 0, 1, 2, 3
_DT = {FMT_F32: np.float32, FMT_F16: np.float16, FMT_U8: np.uint8, FMT_R11: np.uint32}


def ptr(a):
    return a.ctypes.data if a is not None else None


class Framebuffer:
    def __init__(self, w, h, fmt, fps, nbuf=3):
        self.w, self.h, self.fmt = w, h, fmt
        self.h_ = fb_create(w, h, fmt, float(fps), nbuf)
        self.dtype = _DT[fmt]

    def front_array(self):
        """Zero-copy numpy view of the current front buffer, shape (h, w, 4)."""
        nc = 1 if self.fmt == FMT_R11 else 4
        n = self.w * self.h * nc
        buf = (C.c_char * (n * np.dtype(self.dtype).itemsize)).from_address(fb_front(self.h_))
        return np.frombuffer(buf, dtype=self.dtype).reshape(self.h, self.w, nc)

    def counter(self):
        return fb_counter(self.h_)

    def stats(self, reset=False):
        a = (D * 4)()
        fb_stats(self.h_, a, 1 if reset else 0)
        return {"frames": a[0], "write_ms_sum": a[1], "write_ms_max": a[2], "gap_ms_max": a[3]}

    def ring(self, first, n):
        a = np.zeros(n, dtype=np.float64)
        fb_ring(self.h_, first, n, a.ctypes.data)
        return a

    def set_fps(self, fps):
        fb_set_fps(self.h_, float(fps))

    def pause(self, on=True):
        fb_pause(self.h_, 1 if on else 0)

    def destroy(self):
        if self.h_:
            fb_destroy(self.h_)
            self.h_ = None


def check_clock():
    """perf_counter_ns and the DLL's QPC-ns must agree (same clock)."""
    a = time.perf_counter_ns()
    b = now_ns()
    c = time.perf_counter_ns()
    return {"py_before": a, "dll": b, "py_after": c, "inside": a <= b <= c, "delta_ns": b - a}


def stats_summary(v):
    v = np.asarray(v, dtype=np.float64)
    if v.size == 0:
        return {}
    return {"n": int(v.size), "min": float(v.min()), "median": float(np.median(v)), "mean": float(v.mean()),
            "p95": float(np.percentile(v, 95)), "p99": float(np.percentile(v, 99)), "max": float(v.max())}
