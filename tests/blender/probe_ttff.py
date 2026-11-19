# SPDX-License-Identifier: Apache-2.0
"""Time to first frame, one process (task 095, 0007 d35 V7a): a .blend opened in a windowed Blender, Rendered
shading switched on from a timer callback, off, and on again, with the external screen poller
(tools/ttff_poller.ps1) timing the first frame that shows engine pixels. A measurement, not a test, so it is
named probe_*.py and `check full` does not run it.

Run through tools/measure_ttff.ps1 (the machine lock, the poller and the Cycles log are its business):
  -- --scene <name> --engine astroray|cycles --blend <file> --out <result.json> [--hold <s>] [--m8]
The first switch is the first time the file is shown rendered; the second follows a switch back to Solid.
The poller records block means of the centre of the viewport region with wall-clock times; this script
finds, per switch, the first capture that is close to the settled image at the end of the hold (a blank or
the solid view is far from it) and stays close for the next capture, and writes the times and, for
Astroray, the job's marks and phase seconds (blender/astroray render_final.Timeline).
--m8 (Astroray): after the second hold, every material of the scene is sent again with a changed constant in
one edit, and the time from the commit's return to its generation becoming active is the native compile
and upload of all of them (an upper bound: it includes the wait for the next pass boundary).
Ref: docs/decisions/0007-blender-integration.md d35; 0005 Risks M-8; docs/spikes/B-blender-end-to-end
gui_cycles_viewport.py (the switch from a timer callback).
"""
import importlib
import json
import os
import statistics
import subprocess
import sys
import time
import traceback

import numpy as np

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import smoke_a1  # noqa: E402  (the zip install and the argument helpers)

MODULE = smoke_a1.MODULE
arg = smoke_a1.arg
T0 = time.monotonic()
WATCHDOG_S = 540.0
STATE = {"errors": [], "switches": []}
GX, GY = 32, 18


def finish(code):
    sys.stdout.flush()
    os._exit(code)


def log_marker(text):
    """In the Blender log, between the Cycles lines, so the log parser can tell the switches apart."""
    sys.stderr.write(text + "\n")
    sys.stderr.flush()
    print(text, flush=True)


def load_capture(prefix):
    with open(prefix + ".json", encoding="utf-8") as f:
        meta = json.load(f)
    raw = np.fromfile(prefix + ".bin", dtype=np.dtype([("t", "<f8"), ("g", "u1", (GY, GX, 3))]))
    return meta, raw["t"], raw["g"].astype(np.float32) / 255.0


def detect(t, g, t_switch, hold):
    """The first capture after t_switch that is within 35 % of the baseline-to-settled distance of the settled
    image, and whose successor is too. Returns a dict of seconds after the switch and the distances."""
    dist = lambda a, b: float(np.abs(a - b).mean())
    pre, post = (t >= t_switch - 0.6) & (t < t_switch), (t >= t_switch) & (t <= t_switch + hold)
    if pre.sum() < 5 or post.sum() < 20:
        return {"error": f"too few captures (pre {int(pre.sum())}, post {int(post.sum())})"}
    base = g[pre].mean(axis=0)
    idx = np.nonzero(post)[0]
    settled = g[idx[-5:]].mean(axis=0)
    d0 = dist(base, settled)
    noise = max(dist(g[i], settled) for i in idx[-5:])
    thr = max(0.35 * d0, 3.0 * noise + 0.004)
    d = np.array([dist(g[i], settled) for i in idx])
    change = np.array([dist(g[i], base) for i in idx])
    out = {"baseline_to_settled": d0, "threshold": thr, "settled_noise": noise, "captures": int(len(idx))}
    if d0 < 0.03:
        out["error"] = f"no contrast between the solid view and the settled frame ({d0:.4f})"
    hits = np.nonzero((d[:-1] < thr) & (d[1:] < thr))[0]
    if not len(hits):
        out["error"] = out.get("error", "") + " no capture came within the threshold"
        return out
    out["first_frame_s"] = float(t[idx[hits[0]]] - t_switch)
    moved = np.nonzero(change > max(0.01, 0.1 * d0))[0]
    out["first_change_s"] = float(t[idx[moved[0]]] - t_switch) if len(moved) else None
    return out


class Driver:
    def __init__(self, engine, scene_name, out, hold, m8):
        self.engine, self.scene_name, self.out, self.hold, self.m8 = engine, scene_name, out, hold, m8
        self.step, self.t_step = "setup", time.monotonic()
        self.poller = None
        self.prefix = os.path.splitext(out)[0] + ".cap"
        self.stop_file = self.prefix + ".stop"
        self.jobs = []
        self.rf = None
        self.n = 0
        self.m8_result = None

    def go(self, name):
        self.step, self.t_step = name, time.monotonic()

    def waited(self, s):
        return time.monotonic() - self.t_step >= s

    def area(self):
        for win in bpy.context.window_manager.windows:
            for area in win.screen.areas:
                if area.type == "VIEW_3D":
                    return win, area, next(r for r in area.regions if r.type == "WINDOW")
        raise RuntimeError("no 3D viewport")

    def watch_jobs(self):
        held = self.rf._contexts.get("CUDA") if self.rf else None
        for j in (held.jobs if held else ()):
            if hasattr(j, "timeline") and j not in self.jobs and hasattr(j, "frames"):
                self.jobs.append(j)

    def tick(self):
        try:
            if time.monotonic() - T0 > WATCHDOG_S:
                raise RuntimeError(f"watchdog in step {self.step}")
            self.watch_jobs()
            return getattr(self, "step_" + self.step)()
        except Exception:
            STATE["errors"].append(traceback.format_exc())
            print(STATE["errors"][-1], flush=True)
            return self.end()

    def step_setup(self):
        win, area, region = self.area()
        with bpy.context.temp_override(window=win, area=area, region=region):
            bpy.ops.screen.screen_full_area(use_hide_panels=True)
        win, area, region = self.area()
        space = area.spaces.active
        space.overlay.show_overlays = False
        space.show_gizmo = False
        space.shading.type = "SOLID"
        space.region_3d.view_perspective = "CAMERA"
        area.tag_redraw()
        self.go("start_poller")
        return 1.0

    def step_start_poller(self):
        win, area, region = self.area()
        STATE["region"] = [region.x, win.height - region.y - region.height, region.width, region.height]
        STATE["window"] = [win.width, win.height]
        for p in (self.prefix + ".ready", self.prefix + ".bin", self.prefix + ".json", self.stop_file):
            if os.path.exists(p):
                os.remove(p)
        self.poller = subprocess.Popen(
            ["pwsh", "-NoProfile", "-File", os.path.join(ROOT, "tools", "ttff_poller.ps1"),
             "-ProcessId", str(os.getpid()), "-OutPrefix", self.prefix, "-StopFile", self.stop_file,
             "-RegionX", str(STATE["region"][0]), "-RegionY", str(STATE["region"][1]),
             "-RegionW", str(STATE["region"][2]), "-RegionH", str(STATE["region"][3]),
             "-MaxSeconds", str(WATCHDOG_S)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.go("poller_ready")
        return 0.2

    def step_poller_ready(self):
        if os.path.exists(self.prefix + ".ready"):
            self.go("baseline")
            return 0.1
        if self.poller.poll() is not None or self.waited(30):
            raise RuntimeError("the poller did not start: " + (self.poller.communicate()[0] if self.poller.poll() is not None else "timeout"))
        return 0.2

    def step_baseline(self):
        if not self.waited(2.0):
            return 0.05
        return self.switch_on()

    def switch_on(self):
        self.n += 1
        _, area, _ = self.area()
        STATE["switches"].append({"n": self.n, "t_on": time.time()})
        log_marker(f"TTFF_SWITCH {self.n}")
        area.spaces.active.shading.type = "RENDERED"
        area.tag_redraw()
        self.go("hold")
        return 0.1

    def step_hold(self):
        if not self.waited(self.hold):
            return 0.1
        _, area, _ = self.area()
        STATE["switches"][-1]["t_off"] = time.time()
        log_marker(f"TTFF_OFF {self.n}")
        area.spaces.active.shading.type = "SOLID"
        area.tag_redraw()
        self.go("after_off")
        return 0.1

    def step_after_off(self):
        if not self.waited(4.0):
            return 0.1
        if self.n < 2:
            return self.switch_on()
        if self.m8 and self.engine == "astroray":
            return self.switch_m8()
        return self.end()

    def switch_m8(self):
        log_marker("TTFF_M8")
        _, area, _ = self.area()
        area.spaces.active.shading.type = "RENDERED"
        area.tag_redraw()
        self.go("m8_wait")
        return 0.1

    def step_m8_wait(self):
        job = self.jobs[-1] if self.jobs else None
        ready = job is not None and "first_frame" in job.timeline.marks
        if not ready:
            if self.waited(60):
                raise RuntimeError("m8: no first frame")
            return 0.1
        if not self.waited(self.hold * 0.5):
            return 0.1
        self.m8_result = self.resend(job)
        return self.end()

    def resend(self, job):
        """Every translated material again, changed, in one edit; the time to its active generation."""
        import ctypes
        from importlib import import_module
        edit_mod = import_module(MODULE + ".sync.edit")
        translate = import_module(MODULE + ".materials.translate")
        abi, lib = job.abi, job.lib
        items = list(job.tcache._graphs.items())
        for i, (_, em) in enumerate(items):
            em.f32[0] += 1e-3 * (1 + i % 7)  # a different constant: the engine cannot treat the edit as a no-op
        t0 = time.perf_counter()
        with edit_mod.Edit(abi, lib, job.scene_h) as e:
            for uid, em in items:
                translate.set_material(e, abi, job.keymap, uid, em)
            t1 = time.perf_counter()
        t2 = time.perf_counter()
        gen = e.generation
        prog = abi.ar_progress()
        while time.perf_counter() - t2 < 30.0:
            lib.ar_session_get_progress(job.session, ctypes.byref(prog))
            if prog.active_generation >= gen:
                break
            time.sleep(0.0005)
        t3 = time.perf_counter()
        return {"materials": len(items), "send_s": t1 - t0, "commit_s": t2 - t1, "active_after_commit_s": t3 - t2,
                "reached": bool(prog.active_generation >= gen)}

    def end(self):
        open(self.stop_file, "w").close()
        out = {"scene": self.scene_name, "engine": self.engine, "errors": STATE["errors"],
               "region": STATE.get("region"), "window": STATE.get("window"), "hold_s": self.hold}
        try:
            if self.poller is not None:
                text = self.poller.communicate(timeout=60)[0]
                out["poller"] = text.strip()
                meta, t, g = load_capture(self.prefix)
                out["capture"] = meta
                for sw in STATE["switches"]:
                    r = detect(t, g, sw["t_on"], sw["t_off"] - sw["t_on"])
                    r.update(sw)
                    out.setdefault("switches", []).append(r)
        except Exception:
            out["errors"].append(traceback.format_exc())
        out["jobs"] = [self.job_record(j) for j in self.jobs]
        out["m8"] = self.m8_result
        json.dump(out, open(self.out, "w"), indent=1)
        print(f"probe_ttff: wrote {self.out}", flush=True)
        for sw in out.get("switches", []):
            print("probe_ttff: switch", sw.get("n"), "first frame", sw.get("first_frame_s"), sw.get("error", ""), flush=True)
        finish(1 if out["errors"] or any("error" in s for s in out.get("switches", [])) else 0)

    @staticmethod
    def job_record(job):
        tl = job.timeline
        mats = tl.materials
        return {"marks": tl.marks, "phase_s": tl.phase_s, "materials": [list(m) for m in mats]}


def main():
    engine = arg("--engine")
    scene_name, blend, out = arg("--scene"), arg("--blend"), arg("--out")
    hold = float(arg("--hold", "12"))
    if engine not in ("astroray", "cycles"):
        raise SystemExit("--engine astroray|cycles")
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    rf = None
    if engine == "astroray":
        os.environ["ASTRORAY_TTFF_LOG"] = "1"
        smoke_a1.install_and_enable()
        rf = importlib.import_module(MODULE + ".render_final")
    bpy.ops.wm.open_mainfile(filepath=blend, load_ui=False)
    scene = bpy.context.scene
    if engine == "astroray":
        scene.render.engine = "ASTRORAY"
        scene.astroray.device = "CUDA"
        scene.astroray.viewport_samples = 32
    else:
        scene.render.engine = "CYCLES"
        prefs = bpy.context.preferences.addons["cycles"].preferences
        prefs.compute_device_type = "OPTIX"
        prefs.get_devices()
        for d in prefs.devices:
            d.use = d.type == "OPTIX"
        scene.cycles.device = "GPU"
        scene.cycles.preview_samples = 32
        scene.cycles.use_preview_denoising = False
        scene.cycles.use_preview_adaptive_sampling = False
    if scene.camera is not None:
        scene.camera.data.passepartout_alpha = 0.0
    driver = Driver(engine, scene_name, out, hold, "--m8" in smoke_a1.ARGS)
    driver.rf = rf
    bpy.app.timers.register(driver.tick, first_interval=1.5)


if __name__ == "__main__":
    try:
        main()
    except Exception:
        print(traceback.format_exc(), flush=True)
        finish(1)
