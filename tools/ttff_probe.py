# SPDX-License-Identifier: Apache-2.0
"""Time to first frame, one process (0007 d35 V7a as ruling 83 defines it): a .blend opened in a windowed Blender,
Rendered shading switched on from a timer callback, off, and on again. A POST_PIXEL draw handler on the 3D
viewport region timestamps every redraw (the resolution of V7a is one redraw) and, where asked, reads a small
patch of the region's framebuffer after the engine drew. A measurement, not a test, so it is named probe_*.py
and `check full` does not run it.

Run through tools/measure_ttff.ps1 (the machine lock and the Cycles log are its business):
  -- --scene <name> --engine astroray|cycles --blend <file> --out <result.json> [--hold <s>] [--m8]
     [--source read|event|both|none] [--read patch|post_view|timer] [--cell <px>]
The first switch is the first time the file is shown rendered; the second follows a switch back to Solid.
Two ways to find the first redraw that shows engine pixels, per switch:
  read: the patch is read in the handler; the first redraw whose patch is within 35 % of the baseline-to-settled
    distance of the settled image, with a successor that is too, d35's rule (detect below).
  event: the first redraw at or after the engine's own first displayable frame. Astroray: the present's
    first_frame mark (render_final.Timeline, uploaded inside the engine's draw, so the same redraw's handler
    comes after it). Cycles has no such mark in Python: its first `Rendered N samples` log line is placed on
    this clock by tools/measure_ttff.ps1 and matched against the `redraws` list written here.
Writes the times and, for Astroray, the job's marks and phase seconds (blender/astroray render_final.Timeline).
--m8 (Astroray): after the second hold, every material of the scene is sent again with a changed constant in
one edit, and the time from the commit's return to its generation becoming active is the native compile
and upload of all of them (an upper bound: it includes the wait for the next pass boundary).
Ref: docs/decisions/0007-blender-integration.md d35; 0005 Risks M-8; docs/spikes/B-blender-end-to-end
gui_cycles_viewport.py (the switch from a timer callback); ruling 83;
bpy.types.SpaceView3D.draw_handler_add; gpu.state.active_framebuffer_get; GPUFrameBuffer.read_color.
"""
import importlib
import json
import os
import statistics
import sys
import time
import traceback

import numpy as np

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
# A measurement, not a test (it lives in tools/ so check full does not run it); it reuses the smoke
# harness's extension install and argument helpers.
sys.path.insert(0, os.path.join(ROOT, 'tests', 'blender'))
import smoke_a1  # noqa: E402  (the zip install and the argument helpers)

MODULE = smoke_a1.MODULE
arg = smoke_a1.arg
T0 = time.monotonic()
WATCHDOG_S = 540.0
STATE = {"errors": [], "switches": []}
GX, GY = 32, 18   # the patch is GX x GY cells of `--cell` pixels, block-averaged


def finish(code):
    sys.stdout.flush()
    os._exit(code)


def log_marker(text):
    """In the Blender log, between the Cycles lines, so the log parser can tell the switches apart."""
    sys.stderr.write(text + "\n")
    sys.stderr.flush()
    print(text, flush=True)


def detect(t, g, t_switch, hold, min_pre=1, min_post=5):
    """The first read after t_switch that is within 35 % of the baseline-to-settled distance of the settled
    image, and whose successor is too. The baseline is the last reads before the switch (a still Solid view is
    not redrawn, so there may be none within a fixed window). Returns seconds after the switch and the distances."""
    dist = lambda a, b: float(np.abs(a - b).mean())
    before = np.nonzero(t < t_switch)[0][-3:]
    post = (t >= t_switch) & (t <= t_switch + hold)
    if len(before) < min_pre or post.sum() < min_post:
        return {"error": f"too few reads (before {len(before)}, after {int(post.sum())})"}
    base = g[before].mean(axis=0)
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
        self.prefix = os.path.splitext(out)[0] + ".cap"
        self.jobs = []
        self.rf = None
        self.n = 0
        self.m8_result = None
        self.shot = False
        self.src = arg("--source", "both")
        if self.src not in ("read", "event", "both", "none"):
            raise SystemExit("--source read|event|both|none")
        self.read_mode = arg("--read", "patch")
        if self.read_mode not in ("patch", "post_view", "timer"):
            raise SystemExit("--read patch|post_view|timer")
        self.cell = int(arg("--cell", "3"))
        self.fmt = arg("--fmt", "UBYTE")        # read_color's data_format: FLOAT is 4 bytes a channel, UBYTE 1
        self.chans = int(arg("--chans", "4"))
        if self.chans != 4:   # a 3-channel read_color corrupts the heap on Vulkan (127): Blender dies or hangs
            raise SystemExit("--chans must be 4 (3-channel read_color corrupts the heap, see the task 127 PR)")
        self.redraw_t = []              # one epoch stamp per redraw of the region, taken first thing in the handler
        self.read_t, self.read_g = [], []   # the same stamps for the redraws whose patch was read, and the patches
        self.reads_done = 0

    @staticmethod
    def anchor():
        """With `--log context` Blender logs this access of bpy.context.area, with its own clock, at the line
        number printed here: a pair (log time, epoch time) that measure_ttff.ps1 uses to put Cycles' log lines
        on this probe's clock. Called a few times per run; the log parser takes the median offset."""
        ln = sys._getframe().f_lineno + 2
        t = time.time()
        bpy.context.area
        log_marker(f"TTFF_ANCHOR {ln} {t!r}")

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
        self.go("baseline" if self.src == "none" else "start_draw")
        STATE["region"] = [region.x, win.height - region.y - region.height, region.width, region.height]
        return 1.0

    def step_start_draw(self):
        win, area, region = self.area()
        STATE["window"] = [win.width, win.height]
        kind = "POST_VIEW" if self.read_mode == "post_view" else "POST_PIXEL"
        bpy.types.SpaceView3D.draw_handler_add(self.draw_cb, (), "WINDOW", kind)
        if self.read_mode == "timer":   # the read happens off the draw, from a timer
            bpy.app.timers.register(self.timer_read, first_interval=0.02)
        area.tag_redraw()
        self.go("baseline")
        return 0.1

    def draw_cb(self):
        """Every redraw is stamped. Reading the patch here is the whole cost of the `read` method, so the stamp
        comes first and the redraw is not delayed by anything but the read itself."""
        t = time.time()
        self.redraw_t.append(t)
        if self.src in ("read", "both") and self.read_mode != "timer":
            self.read_patch(t)
        # The next redraw must come: a still region is not redrawn, and an engine's frame arrives between redraws.
        # (The engines tag their own redraws; this keeps the stamp series dense enough to be the resolution.)

    def read_patch(self, t):
        import gpu
        region = bpy.context.region
        if region is None:
            raise RuntimeError("no region in context: the read must run inside the draw handler")
        c = self.cell
        cw, ch = min(GX * c, region.width) // GX * GX, min(GY * c, region.height) // GY * GY
        x0, y0 = (region.width - cw) // 2, (region.height - ch) // 2
        buf = gpu.state.active_framebuffer_get().read_color(x0, y0, cw, ch, self.chans, 0, self.fmt)
        a = np.array(buf, dtype=np.float32).reshape(ch, cw, self.chans)[::-1, :, :3]   # first row at the top
        if self.fmt == "UBYTE":
            a = a / 255.0
        self.read_t.append(t)
        self.read_g.append(a.reshape(GY, ch // GY, GX, cw // GX, 3).mean(axis=(1, 3)))

    def timer_read(self):
        try:
            self.read_patch(time.time())
        except Exception:
            STATE["errors"].append(traceback.format_exc())
            return None
        return 0.02

    def step_baseline(self):
        if self.waited(1.0) and not self.waited(1.2):
            _, area, _ = self.area()
            area.tag_redraw()   # one redraw of the Solid view late in the baseline, so the reads have a "before"
        if not self.waited(2.0):
            return 0.05
        self.anchor()
        return self.switch_on()

    def switch_on(self):
        self.n += 1
        self.shot = False
        _, area, _ = self.area()
        STATE["switches"].append({"n": self.n, "t_on": time.time()})
        log_marker(f"TTFF_SWITCH {self.n}")
        area.spaces.active.shading.type = "RENDERED"
        area.tag_redraw()
        self.go("hold")
        return 0.1

    def step_hold(self):
        if "--shot" in smoke_a1.ARGS and not self.shot and self.waited(self.hold - 1.5):
            self.shot = True  # Blender's own view of its area, to look at beside the read patches
            win, area, region = self.area()
            with bpy.context.temp_override(window=win, area=area, region=region):
                bpy.ops.screen.screenshot_area(filepath=self.prefix + f"_blender{self.n}.png")
        if not self.waited(self.hold):
            return 0.1
        _, area, _ = self.area()
        STATE["switches"][-1]["t_off"] = time.time()
        log_marker(f"TTFF_OFF {self.n}")
        self.anchor()
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
        out = {"scene": self.scene_name, "engine": self.engine, "errors": STATE["errors"],
               "region": STATE.get("region"), "window": STATE.get("window"), "hold_s": self.hold,
               "source": self.src, "read_mode": self.read_mode, "cell": self.cell, "fmt": self.fmt, "chans": self.chans}
        try:
            self.watch_jobs()
            rt = np.array(self.redraw_t)
            out["capture"] = {"redraws": int(len(rt)), "seconds": float(rt[-1] - rt[0]) if len(rt) else 0.0,
                              "reads": int(len(self.read_t))}
            t, g = np.array(self.read_t), np.array(self.read_g)
            out["read_stats"] = [[float(a), float(b.mean()), float(b.max())] for a, b in zip(t, g)] if len(t) else []
            out["switches"] = []
            for i, sw in enumerate(STATE["switches"]):
                sw = dict(sw)
                span = (rt >= sw["t_on"] - 0.05) & (rt <= sw["t_off"])
                sw["redraws"] = [float(x - sw["t_on"]) for x in rt[span]]   # seconds after the switch
                if self.src == "none":
                    sw["note"] = "V7a not measured"
                if self.src in ("read", "both"):
                    r = detect(t, g, sw["t_on"], sw["t_off"] - sw["t_on"])
                    sw["read"] = r
                    sw["read_s"] = r.get("first_frame_s")
                    sw["first_change_s"] = r.get("first_change_s")
                if self.engine == "astroray" and self.src in ("event", "both"):
                    marks = self.jobs[i].timeline.marks if i < len(self.jobs) else {}
                    ff = marks.get("first_frame")
                    after = rt[rt >= ff] if ff else rt[:0]
                    sw["engine_first_frame_s"] = float(ff - sw["t_on"]) if ff else None
                    sw["event_s"] = float(after[0] - sw["t_on"]) if len(after) else None
                    if sw["event_s"] is None:
                        sw["error"] = "no redraw at or after the engine's first frame"
                if self.src in ("read",) and sw.get("read_s") is None:
                    sw["error"] = "read: " + str(sw["read"].get("error", "no detection"))
                out["switches"].append(sw)
        except Exception:
            out["errors"].append(traceback.format_exc())
        out["jobs"] = [self.job_record(j) for j in self.jobs]
        out["m8"] = self.m8_result
        json.dump(out, open(self.out, "w"), indent=1)
        print(f"ttff_probe: wrote {self.out}", flush=True)
        for sw in out.get("switches", []):
            print("ttff_probe: switch", sw.get("n"), "read", sw.get("read_s"), "event", sw.get("event_s"), sw.get("error", ""), flush=True)
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
