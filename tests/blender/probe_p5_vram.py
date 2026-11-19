# SPDX-License-Identifier: Apache-2.0
"""Probe P5 (task 097): VRAM that Blender and the desktop use, which sets the reserve R of 0007 d14.

Run through `Invoke-ArBlender -Gui -Script tests/blender/probe_p5_vram.py [-- --b5m <blend>]`. A recording, not
a pass/fail test: it prints the outcome and exits 0 unless the harness itself failed. The device's free bytes
come from `cuMemGetInfo` (driver API, through ctypes: the engine has no call that returns them, and Blender
holds no CUDA context of its own), the used bytes are total - free, and `nvidia-smi` is read alongside as a
cross-check. The probe's own CUDA context is measured when it is created and subtracted. Three states, each
sampled once a second:
  idle:     Blender open on its startup scene, Solid viewport
  solid:    the b5m scene (10,000 instances, 2,000 meshes) loaded, Solid viewport
  material: the same, Material Preview (EEVEE), sampled for 40 s while its shaders compile and draw
R (0007 d14) is the headroom Blender can still take after our allocation, because free bytes already include
what it uses at that moment. The probe therefore computes R = (largest Material Preview reading - largest idle
reading) x 1.25, both from cuMemGetInfo with the probe's own context subtracted, rounded up to 0.5 GiB steps;
that is R_bytes in the JSON. The totals (desktop included, nvidia-smi too) are printed as context only.
Ref: docs/decisions/0007-blender-integration.md d14 (reserve rule) and Risks P5;
CUDA driver API cuMemGetInfo_v2 (cuda.h).
"""
import ctypes
import importlib
import json
import math
import os
import subprocess
import sys
import time
import traceback

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
for sub in ("tools", os.path.join("scenes", "lib")):
    sys.path.insert(0, os.path.join(ROOT, sub))
import blend_writer  # noqa: E402
import smoke_a1  # noqa: E402

MARGIN = 1.25
R_STEP = 0.5 * (1 << 30)  # R is rounded up to this
GIB = float(1 << 30)
SAMPLE_S = 1.0
STATE_S = {"idle": 6, "solid": 8, "material": 40}
WATCHDOG_S = 300.0  # the idle, load and 40 s Material Preview phases take about 80 s
T0 = time.monotonic()
STATE = {"samples": {}}
EVIDENCE = os.path.join(ROOT, "out", "evidence", "097")


def finish(code):
    print(f"probe_p5_vram exit {code}", flush=True)
    sys.stdout.flush()
    os._exit(code)


class Cuda:
    """cuMemGetInfo on a context of our own."""

    def __init__(self):
        self.lib = ctypes.WinDLL("nvcuda.dll")
        self.check(self.lib.cuInit(0), "cuInit")
        dev = ctypes.c_int()
        self.check(self.lib.cuDeviceGet(ctypes.byref(dev), 0), "cuDeviceGet")
        self.ctx = ctypes.c_void_p()
        self.check(self.lib.cuCtxCreate_v2(ctypes.byref(self.ctx), 0, dev), "cuCtxCreate")

    @staticmethod
    def check(rc, what):
        if rc != 0:
            raise RuntimeError(f"{what} failed with CUDA error {rc}")

    def mem(self):
        free, total = ctypes.c_size_t(), ctypes.c_size_t()
        self.check(self.lib.cuMemGetInfo_v2(ctypes.byref(free), ctypes.byref(total)), "cuMemGetInfo")
        return free.value, total.value


def smi_used():
    """nvidia-smi's used MiB in bytes (all processes, the probe's context included)."""
    out = subprocess.run(["nvidia-smi", "--query-gpu=memory.used", "--format=csv,noheader,nounits"],
                         capture_output=True, text=True, timeout=30)
    if out.returncode != 0:
        raise RuntimeError("nvidia-smi failed: " + out.stderr)
    return int(out.stdout.strip().splitlines()[0]) * (1 << 20)


class Driver:
    def __init__(self, b5m_blend):
        self.cuda = None
        self.b5m = b5m_blend
        self.step, self.t0, self.last = "ctx", time.monotonic(), 0.0

    def go(self, step):
        self.step, self.t0, self.last = step, time.monotonic(), 0.0

    def sample(self, name):
        free, total = self.cuda.mem()
        STATE["samples"].setdefault(name, []).append(
            {"t": round(time.monotonic() - T0, 1), "cuda_used": total - free - STATE["ctx_bytes"],
             "smi_used": smi_used() - STATE["ctx_bytes"], "free": free, "total": total})

    def tick(self):
        try:
            if time.monotonic() - T0 > WATCHDOG_S:
                print(f"watchdog: still in step {self.step}", flush=True)
                finish(1)
            return getattr(self, "step_" + self.step)()
        except Exception:
            print("probe failed:\n" + traceback.format_exc(), flush=True)
            finish(1)

    def step_ctx(self):
        smi0 = smi_used()
        self.cuda = Cuda()
        time.sleep(0.5)
        free, total = self.cuda.mem()
        smi1 = smi_used()
        STATE["ctx_bytes"] = max(smi1 - smi0, 0)
        STATE["total"] = total
        print(f"PROBE P5 own context: nvidia-smi used {smi0 / GIB:.3f} -> {smi1 / GIB:.3f} GiB "
              f"(subtracted {STATE['ctx_bytes'] / GIB:.3f}); total {total / GIB:.2f} GiB", flush=True)
        self.go("idle")
        return 0.1

    def sampling(self, name, nxt, after=None):
        if self.last == 0.0:
            self.last = time.monotonic()
        self.sample(name)
        if time.monotonic() - self.t0 >= STATE_S[name]:
            if after:
                after()
            self.go(nxt)
            return 0.1
        return SAMPLE_S

    def step_idle(self):
        return self.sampling("idle", "load")

    def step_load(self):
        bpy.ops.wm.open_mainfile(filepath=self.b5m, load_ui=False)
        for win in bpy.context.window_manager.windows:
            for area in win.screen.areas:
                if area.type == "VIEW_3D":
                    area.spaces.active.shading.type = "SOLID"
        self.go("solid")
        return 1.0

    def step_solid(self):
        return self.sampling("solid", "material", after=self.to_material)

    def to_material(self):
        print("PROBE P5 Material Preview on", flush=True)
        for win in bpy.context.window_manager.windows:
            for area in win.screen.areas:
                if area.type == "VIEW_3D":
                    area.spaces.active.shading.type = "MATERIAL"
                    area.tag_redraw()

    def step_material(self):
        for win in bpy.context.window_manager.windows:
            for area in win.screen.areas:
                if area.type == "VIEW_3D":
                    area.tag_redraw()
        return self.sampling("material", "done")

    def step_done(self):
        used = {}
        for name, rows in STATE["samples"].items():
            used[name] = {"cuda_max": max(r["cuda_used"] for r in rows), "smi_max": max(r["smi_used"] for r in rows),
                          "cuda_final": rows[-1]["cuda_used"], "n": len(rows)}
        peak = max(max(v["cuda_max"], v["smi_max"]) for v in used.values())
        growth = used["material"]["cuda_max"] - used["idle"]["cuda_max"]
        r = math.ceil(growth * MARGIN / R_STEP) * R_STEP
        for name, v in used.items():
            print(f"PROBE P5 {name}: used by Blender and the desktop, max cuMemGetInfo {v['cuda_max'] / GIB:.3f} GiB, "
                  f"nvidia-smi {v['smi_max'] / GIB:.3f} GiB, final {v['cuda_final'] / GIB:.3f} GiB ({v['n']} samples)")
        print(f"PROBE P5 context only (desktop included): largest total {peak / GIB:.3f} GiB, "
              f"largest + 25% = {peak * MARGIN / GIB:.3f} GiB; card {STATE['total'] / GIB:.2f} GiB")
        print(f"PROBE P5 result: growth = Material Preview max - idle max (cuMemGetInfo, own context subtracted) "
              f"= {growth / GIB:.3f} GiB; x 1.25 = {growth * MARGIN / GIB:.3f} GiB; R rounded up to 0.5 GiB = "
              f"{r / GIB:.2f} GiB", flush=True)
        os.makedirs(EVIDENCE, exist_ok=True)
        json.dump({"used": used, "growth_bytes": int(growth), "R_bytes": int(r), "samples": STATE["samples"]},
                  open(os.path.join(EVIDENCE, "p5_vram.json"), "w"), indent=1)
        finish(0)


def main():
    work = smoke_a1.arg("--work", os.path.join(ROOT, "out", "smoke"))
    os.makedirs(work, exist_ok=True)
    blend = smoke_a1.arg("--b5m", os.path.join(work, "b5m_p5.blend"))
    if not os.path.exists(blend):
        p = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "scenes", "b5m", "build.py"),
                            "--out", os.path.join(work, "b5m_p5.arrec"), "--blend", blend],
                           capture_output=True, text=True, timeout=900)
        if p.returncode != 0:
            raise RuntimeError("scenes/b5m/build.py failed: " + p.stdout + p.stderr)
        smoke_a1.install_and_enable()
        blend_writer.build(blend + ".json", blend)
    else:
        smoke_a1.install_and_enable()  # the scene's engine is the addon's: Blender must know it
    driver = Driver(blend)
    bpy.app.timers.register(driver.tick, first_interval=3.0, persistent=True)  # a file load drops other timers


if __name__ == "__main__":
    try:
        main()
    except Exception:
        print(traceback.format_exc(), flush=True)
        finish(1)
