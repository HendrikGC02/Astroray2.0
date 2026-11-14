import sys, os, time, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import sbn

print("clock", sbn.check_clock())
fb = sbn.Framebuffer(1920, 1080, sbn.FMT_F32, 60.0)
t0 = time.perf_counter(); c0 = fb.counter()
time.sleep(2.0)
c1 = fb.counter(); dt = time.perf_counter() - t0
print("f32 1080p @60 target: frames", c1 - c0, "rate", (c1 - c0) / dt, fb.stats())
a = fb.front_array(); print("front", a.shape, a.dtype, a[0, 0], a[0, 100], a.max())
fb.destroy()
for fmt, nm in ((sbn.FMT_F32, "f32"), (sbn.FMT_F16, "f16"), (sbn.FMT_U8, "u8")):
    for w, h in ((1920, 1080), (3840, 2160)):
        fb = sbn.Framebuffer(w, h, fmt, 0.0)  # free-running
        time.sleep(1.0); s = fb.stats()
        print(nm, w, h, "free-run fps", s["frames"] / 1.0, "write ms avg", s["write_ms_sum"] / max(1, s["frames"]))
        fb.destroy()
print("gas init", sbn.gas_init(), sbn.gas_error())
sc = sbn.scene_create()
pos = np.array([[0, 0, 0], [1, 0, 0], [0, 1, 0]], np.float32); tri = np.array([[0, 1, 2]], np.int32)
print("copy ms", sbn.scene_set_mesh(sc, 0, sbn.ptr(pos), 3, sbn.ptr(tri), 1, None, 0, None, 0, None, None))
xf = np.eye(4, dtype=np.float32).reshape(1, 16)
print("objs ms", sbn.scene_set_objects(sc, 1, sbn.ptr(np.array([0], np.int32)), sbn.ptr(xf)))
out = (sbn.D * 10)(); print("gas build", sbn.gas_build(sc, out), sbn.gas_error(), list(out))
