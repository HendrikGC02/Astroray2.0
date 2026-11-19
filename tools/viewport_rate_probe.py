# SPDX-License-Identifier: Apache-2.0
"""Viewport update rate, present cost and batching on one scene, in one windowed Blender (task 096).

A measurement, not a test, so it is named *_probe.py and `check full` does not run it. Run through
tools/measure_viewport.ps1 (the machine lock, the scene builds and the history lines are its business):
  -- --scene <name> --blend <file> --out <result.json> [--orbit 10] [--rest 9] [--frames 0] [--shot <png>]
The 3D view is maximised (Rendered, no overlays), the viewport job's frame log is switched on
(ASTRORAY_FRAME_LOG=1, blender/astroray viewport.py), and a scripted orbit changes the view at every main
loop iteration for --orbit seconds (or until --frames presented frames, for the B-13 run), then the view rests
for --rest seconds. Everything is read from the frame log: GDI capture cannot see Blender's swapchain on this
machine (task 095), so rates are the engine's presented updates, one record per upload.
Reported: B-14 presented updates per second during the orbit (median and 5th percentile of 1/interval, the
mean) and every run of consecutive intervals above 1/24 s that lasts longer than 0.5 s; B-13 view_draw time
per presented frame (median, p99) and the held rate; V23 passes per update during the orbit and after it
against B-34's schedule (display interval 0.1 s to 1 s of path-trace time since the last view change, 0.25 s
to 2 s, 0.5 s to 4 s, 1 s to 8 s).
Ref: docs/decisions/0009-build-test-process.md B-13, B-14, B-34; docs/decisions/0007-blender-integration.md
d25, d49, d50, V8, V23; docs/spikes/B-blender-end-to-end gui_present.py (paced present timing).
"""
import importlib
import json
import math
import os
import sys
import time
import traceback

import numpy as np

import bpy
import mathutils

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tests", "blender"))
import smoke_a1  # noqa: E402  (the zip install and the argument helpers)

MODULE, arg = smoke_a1.MODULE, smoke_a1.arg
T0 = time.monotonic()
WATCHDOG_S = 240.0
FLOOR_HZ, FLOOR_S = 24.0, 0.5   # 0009 B-14
DEG_PER_TICK = 0.4
SCHEDULE = ((1.0, 0.1), (2.0, 0.25), (4.0, 0.5), (8.0, 1.0))  # 0009 B-34: until (s since the last view change), interval (s)


def finish(code):
    sys.stdout.flush()
    os._exit(code)


def pct(v, q):
    return float(np.percentile(v, q)) if len(v) else None


def analyse(st, t_a, t_b, t_view):
    """The frame log of one run: orbit window [t_a, t_b], last view change t_view."""
    frames, draws = st["frames"], st["draws"]
    shown = [f for f in frames if f["presented"]]
    orb = [f for f in shown if t_a <= f["t"] <= t_b]
    t = np.array([f["t"] for f in orb])
    out = {"window_s": t_b - t_a, "presented": len(orb), "acquired": len([f for f in frames if t_a <= f["t"] <= t_b])}
    if len(t) < 3:
        out["error"] = f"only {len(t)} presented frames in the orbit"
        return out
    dt = np.diff(t)
    inst = 1.0 / dt
    out["rate_mean"] = len(orb) / (t_b - t_a)
    out["pass_ms_median"] = pct([f["pass_ms"] for f in orb], 50)
    out["rate_median"], out["rate_p5"], out["rate_min"] = pct(inst, 50), pct(inst, 5), float(inst.min())
    # B-14 floor: runs of consecutive intervals longer than 1/24 s, their summed length.
    longest = cur = 0.0
    for d in dt:
        cur = cur + d if d > 1.0 / FLOOR_HZ else 0.0
        longest = max(longest, cur)
    out["longest_below_floor_s"] = float(longest)
    out["floor_breach"] = bool(out["longest_below_floor_s"] > FLOOR_S)
    grid = np.arange(t_a + FLOOR_S, t_b, 0.01)
    counts = np.array([np.count_nonzero((t > g - FLOOR_S) & (t <= g)) for g in grid])
    out["min_frames_in_half_second"] = int(counts.min()) if len(counts) else None
    out["series"] = [(float(g - t_a), float(c / FLOOR_S)) for g, c in zip(grid[::25], counts[::25])]
    # B-13: the whole view_draw of every draw that uploaded a frame.
    up = [d for d in draws if t_a <= d["t"] <= t_b and d["upload_s"] > 0]
    if up:
        dur = np.array([d["dur_s"] for d in up]) * 1e3
        out["present"] = {"n": len(up), "view_draw_ms_median": pct(dur, 50), "view_draw_ms_p99": pct(dur, 99),
                          "upload_ms_median": pct([d["upload_s"] * 1e3 for d in up], 50),
                          "draw_ms_median": pct([d["draw_s"] * 1e3 for d in up], 50),
                          "view_draw_ms_max": float(dur.max())}
    # V23, during the orbit: passes per update (the epoch's pass count at acquire).
    spp = np.array([f["spp"] for f in orb])
    out["orbit_passes_per_update"] = {"one": int((spp == 1).sum()), "two": int((spp == 2).sum()), "more": int((spp > 2).sum()),
                                      "max": int(spp.max())}
    # V23, after the last view change: interval, passes per update and the schedule bucket.
    rest = [f for f in shown if f["t"] > t_view]
    rows, prev = [], None
    for f in rest:
        if prev is not None and f["epoch"] == prev["epoch"]:
            rows.append((f["t"] - t_view, f["t"] - prev["t"], f["spp"] - prev["spp"], f["spp"]))
        prev = f
    out["rest"] = {"frames": len(rest), "rows": [[round(a, 3), round(b, 4), int(c), int(d)] for a, b, c, d in rows]}
    buckets, lo = [], 0.0
    for hi, want in SCHEDULE:
        sel = [r for r in rows if lo <= r[0] < hi]
        if sel:
            buckets.append({"from_s": lo, "to_s": hi, "schedule_interval_s": want, "n": len(sel),
                            "interval_s_median": float(np.median([r[1] for r in sel])),
                            "passes_median": float(np.median([r[2] for r in sel])), "spp_end": sel[-1][3]})
        lo = hi
    out["rest"]["buckets"] = buckets
    later = [r for r in rows if r[0] >= 0.5]
    out["rest"]["rate_after_half_second"] = (len(later) / (later[-1][0] - later[0][0])) if len(later) > 2 else None
    if later:
        per_s = np.bincount(np.floor(np.array([r[0] for r in later]) - 0.5).astype(int))
        out["rest"]["max_frames_in_a_second"] = int(per_s.max())
    return out


class Driver:
    def __init__(self, rf, scene_name, out):
        self.rf, self.scene_name, self.out = rf, scene_name, out
        self.step, self.t_step = "setup", time.monotonic()
        self.job = None
        self.orbit_s, self.rest_s = float(arg("--orbit", "10")), float(arg("--rest", "9"))
        self.want_frames, self.skip = int(arg("--frames", "0")), int(arg("--skip", "0"))
        self.pace = float(arg("--pace", "0"))
        self.t_offer = 0.0
        self.t_a = self.t_b = None
        self.ticks = 0
        self.region = None
        self.errors = []

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

    def tick(self):
        try:
            if time.monotonic() - T0 > WATCHDOG_S:
                raise RuntimeError(f"watchdog in step {self.step}")
            return getattr(self, "step_" + self.step)()
        except Exception:
            self.errors.append(traceback.format_exc())
            print(self.errors[-1], flush=True)
            return self.end()

    def step_setup(self):
        win, area, region = self.area()
        with bpy.context.temp_override(window=win, area=area, region=region):
            bpy.ops.screen.screen_full_area(use_hide_panels=True)
        with bpy.context.temp_override(window=win):  # borderless: the region is the whole 1920x1080 screen
            bpy.ops.wm.window_fullscreen_toggle()
        self.go("render_on")
        return 1.5

    def step_render_on(self):
        win, area, region = self.area()
        space = area.spaces.active
        space.overlay.show_overlays = False
        space.show_gizmo = False
        # The orbit pivots on the scene's mesh centre, starting from the scene camera's pose.
        cam = bpy.context.scene.camera
        pts = [o.matrix_world.translation for o in bpy.context.scene.objects if o.type == "MESH"]
        centre = sum(pts, mathutils.Vector()) / max(1, len(pts))
        rv3d = space.region_3d
        rv3d.view_perspective = "PERSP"
        rv3d.view_rotation = cam.matrix_world.to_quaternion()
        rv3d.view_location = centre
        rv3d.view_distance = (cam.matrix_world.translation - centre).length
        space.clip_end = max(space.clip_end, 10.0 * rv3d.view_distance)
        space.shading.type = "RENDERED"
        area.tag_redraw()
        self.region = (region.width, region.height)
        print(f"viewport_probe: region {self.region[0]}x{self.region[1]}", flush=True)
        self.go("first_frame")
        return 0.2

    def gate_offers(self):
        """B-13's 60 Hz producer: view_draw may take a frame only when one is offered, and an offer is the
        engine's newest frame again, `pace` times a second on average (the engine alone delivers fewer)."""
        take = self.job._present_newest
        self.offered = False

        def gated():
            if not self.offered:
                return False
            self.offered = False
            return take()
        self.job._present_newest = gated

    def find_job(self):
        held = self.rf._contexts.get("CUDA")
        return next((j for j in (held.jobs if held else ()) if getattr(j, "stamps", None) is not None), None)

    def step_first_frame(self):
        self.job = self.job or self.find_job()
        if self.job is None or not any(f["presented"] for f in self.job.stamps["frames"]):
            if self.waited(120):
                raise RuntimeError("no frame presented")
            return 0.1
        if self.pace:
            self.gate_offers()
        self.go("settle")
        return 0.1

    def step_settle(self):
        if not self.waited(2.0):
            return 0.1
        self.t_a = self.t_offer = time.monotonic()
        self.go("orbit")
        return 0.0

    def step_orbit(self):
        _, area, _ = self.area()
        rv3d = area.spaces.active.region_3d
        rv3d.view_rotation = mathutils.Quaternion((0, 0, 1), math.radians(DEG_PER_TICK)) @ rv3d.view_rotation
        area.tag_redraw()
        self.ticks += 1
        if self.pace and time.monotonic() >= self.t_offer:  # B-13's producer, see gate_offers
            self.job._last_id = 0
            self.offered = True
            self.t_offer += 1.0 / self.pace
        shown = sum(1 for f in self.job.stamps["frames"] if f["presented"] and f["t"] >= self.t_a)
        if (self.want_frames and shown >= self.want_frames) or (not self.want_frames and self.waited(self.orbit_s)):
            self.t_b = time.monotonic()
            shot = arg("--shot")
            if shot:  # Blender's own screenshot: GDI cannot see the swapchain; taken as the orbit ends
                win, area, region = self.area()
                os.makedirs(os.path.dirname(os.path.abspath(shot)), exist_ok=True)
                with bpy.context.temp_override(window=win, area=area, region=region):
                    bpy.ops.screen.screenshot_area(filepath=shot)
            self.go("rest")
            return 0.1
        if self.waited(60):
            raise RuntimeError("the orbit did not reach its frame count in 60 s")
        return 0.0

    def step_rest(self):
        return 0.1 if not self.waited(self.rest_s) else self.end()

    def end(self):
        out = {"scene": self.scene_name, "region": self.region, "orbit_ticks": self.ticks, "errors": self.errors,
               "full_1080p": self.region == (1920, 1080), "orbit_s": self.orbit_s if self.t_b else None}
        if self.job is not None and self.t_b is not None:
            st = self.job.stamps
            views = [d["t"] for d in st["draws"] if d["set_view"]]
            if self.skip:  # the warm-up frames are not measured
                self.t_a = [f["t"] for f in st["frames"] if f["presented"] and f["t"] >= self.t_a][self.skip]
            out["stats"] = analyse(st, self.t_a, self.t_b, views[-1])
            out["stats"]["draws_per_s"] = len([d for d in st["draws"] if self.t_a <= d["t"] <= self.t_b]) / (self.t_b - self.t_a)
            out["stats"]["set_views_per_s"] = len([v for v in views if self.t_a <= v <= self.t_b]) / (self.t_b - self.t_a)
            out["presented_t"] = [round(f["t"] - self.t_a, 4) for f in st["frames"] if f["presented"] and self.t_a <= f["t"] <= self.t_b]
            if "error" in out["stats"]:
                out["errors"].append(out["stats"]["error"])
        os.makedirs(os.path.dirname(os.path.abspath(self.out)), exist_ok=True)
        try:
            text = json.dumps(out, indent=1)
        except TypeError:  # never a half-written result and a hung process
            print(traceback.format_exc(), flush=True)
            finish(1)
        with open(self.out, "w") as f:
            f.write(text)
        s = out.get("stats", {})
        print(f"RESULT viewport {self.scene_name}: region {self.region}, rate mean {s.get('rate_mean')}, median {s.get('rate_median')}, "
              f"p5 {s.get('rate_p5')}, longest below floor {s.get('longest_below_floor_s')} s; present {s.get('present')}", flush=True)
        self.rf.shutdown()
        finish(1 if out["errors"] else 0)


def main():
    scene_name, blend, out = arg("--scene"), arg("--blend"), arg("--out")
    os.environ["ASTRORAY_FRAME_LOG"] = "1"
    smoke_a1.install_and_enable()
    rf = importlib.import_module(MODULE + ".render_final")
    bpy.ops.wm.open_mainfile(filepath=blend, load_ui=False)
    scene = bpy.context.scene
    scene.render.engine = "ASTRORAY"
    scene.astroray.device = "CUDA"
    scene.astroray.viewport_samples = 1000000  # never converges during the run
    if scene.camera is not None:
        scene.camera.data.passepartout_alpha = 0.0
    bpy.app.timers.register(Driver(rf, scene_name, out).tick, first_interval=1.5)


if __name__ == "__main__":
    try:
        main()
    except Exception:
        print(traceback.format_exc(), flush=True)
        finish(1)
