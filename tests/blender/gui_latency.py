# SPDX-License-Identifier: Apache-2.0
"""Latency in a windowed Blender (task 096, 0007 V12, 0009 B-32): F12 cancel, one view change, a continuous orbit.

Run through `Invoke-ArBlender -Gui -Script tests/blender/gui_latency.py` (the land canary adds `-k <case>`);
`-- --scene a1_furnace|b1m|b5m` picks the scene (default a1_furnace, which `check full` runs; the task's numbers
come from b1m and b5m). Timers drive everything; the script ends the process itself, exit code 1 on a failed case.
  cancel: F12 at 1080p on the GPU, the user's Escape stood in for by render_final.break_requested after 1 s.
          A thread polls the session's state: the time from the break to the engine's PAUSED is the cancel's
          latency (the F12 loop itself only notices PAUSED at its 100 ms wait timeout, 0009 B-37).
  view:   8 single view changes in a maximised, fullscreen Rendered viewport, each after the previous frame
          settled; then a 3 s orbit with a view change at every main-loop iteration. Everything is read from the
          viewport job's frame log (ASTRORAY_FRAME_LOG=1): one record per view_draw and per acquired frame.
A phase that fails records its error and the run goes on, so a case fails only for its own phase.
Bound of one view change (0007 d25): measured from the view_draw that latched the change (set_view) to the end of
the view_draw that presented the first frame of a newer generation; allowed are the rest of the pass in flight
plus one pass (2 x the pass time), the host picking the frame up at its next redraw (one redraw interval) and the
present (the upload and the draw of that view_draw, not its other work).
Ref: docs/decisions/0007-blender-integration.md d25, d27, V12; docs/decisions/0009-build-test-process.md B-17
(target 20 ms, fails above 100 ms), B-32, B-37.
"""
import ctypes
import importlib
import json
import math
import os
import subprocess
import sys
import threading
import time
import traceback
import unittest

import numpy as np

import bpy
import mathutils

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
for sub in ("tools", os.path.join("scenes", "lib")):
    sys.path.insert(0, os.path.join(ROOT, sub))
import blend_writer  # noqa: E402
import smoke_a1  # noqa: E402

MODULE, ARGS, arg = smoke_a1.MODULE, smoke_a1.ARGS, smoke_a1.arg
WATCHDOG_S = 270.0
CANCEL_AFTER_S = 1.0
CANCEL_LIMIT_MS = 100.0   # 0009 B-17: fails above 100 ms (target 20 ms)
CHANGES, ORBIT_S, DEG = 8, 3.0, 0.4
T0 = time.monotonic()
STATE = {"errors": {}}
EVIDENCE = os.path.join(ROOT, "out", "evidence", "096")
SCENE = arg("--scene", "a1_furnace")


def finish(code):
    print(f"gui_latency exit {code}", flush=True)
    sys.stdout.flush()
    os._exit(code)


class Driver:
    def __init__(self, rf):
        self.rf, self.job, self.step_name, self.step_t0 = rf, None, "cancel", time.monotonic()
        self.n = 0

    def go(self, name):
        self.step_name, self.step_t0 = name, time.monotonic()

    def waited(self, s):
        return time.monotonic() - self.step_t0 >= s

    def area(self):
        for win in bpy.context.window_manager.windows:
            for area in win.screen.areas:
                if area.type == "VIEW_3D":
                    return win, area, next(r for r in area.regions if r.type == "WINDOW")
        raise RuntimeError("no 3D viewport")

    def tick(self):
        try:
            if time.monotonic() - T0 > WATCHDOG_S:
                raise RuntimeError(f"watchdog in step {self.step_name}")
            return getattr(self, "step_" + self.step_name)()
        except Exception:
            phase = "cancel" if self.step_name == "cancel" else "view"
            STATE["errors"][phase] = traceback.format_exc()
            print(f"phase {phase} failed:\n{STATE['errors'][phase]}", flush=True)
            if phase == "cancel":
                self.go("view_setup")
                return 0.1
            return self.end()

    def end(self):
        self.rf.shutdown()
        run_cases()

    # ---- cancel

    def step_cancel(self):
        scene = bpy.context.scene
        scene.render.resolution_x, scene.render.resolution_y, scene.render.resolution_percentage = 1920, 1080, 100
        scene.astroray.samples = 100000
        scene.render.image_settings.file_format = "OPEN_EXR"
        rec = {}
        stop_at = time.monotonic() + CANCEL_AFTER_S
        original = self.rf.break_requested

        def passes_done():
            held = self.rf._contexts.get("CUDA")
            job = next((j for j in (held.jobs if held else ()) if j.session.value), None)
            if job is None:
                return 0
            prog = job.abi.ar_progress()
            job.lib.ar_session_get_progress(job.session, ctypes.byref(prog))
            return prog.samples_per_pixel

        def escape(engine):
            # After the first pass: the launch boundary is what is under test, not the scene upload and
            # acceleration structure build before it (reported as rec["first_pass_s"]).
            if time.monotonic() > stop_at and "t_break" not in rec and passes_done() >= 1:
                rec["t_break"] = time.monotonic()
                rec["first_pass_s"] = rec["t_break"] - t_start
            return "t_break" in rec
        done = threading.Event()

        def poll():
            while not done.is_set() and time.monotonic() < stop_at + 60.0:
                held = self.rf._contexts.get("CUDA")
                job = next((j for j in (held.jobs if held else ()) if j.session.value), None) if "t_break" in rec else None
                if job is not None:
                    prog = job.abi.ar_progress()
                    job.lib.ar_session_get_progress(job.session, ctypes.byref(prog))
                    if prog.state in (job.abi.AR_STATE_PAUSED, job.abi.AR_STATE_TRIMMED, job.abi.AR_STATE_CONVERGED,
                                      job.abi.AR_STATE_CLOSED, job.abi.AR_STATE_FAILED):
                        rec["t_stop"], rec["state"] = time.monotonic(), prog.state
                        rec["abi"] = {k: getattr(job.abi, k) for k in ("AR_STATE_PAUSED", "AR_STATE_TRIMMED")}
                        return
                time.sleep(0.0002)
        t_start = time.monotonic()
        thread = threading.Thread(target=poll, daemon=True)
        self.rf.break_requested = escape
        thread.start()
        try:
            bpy.ops.render.render(write_still=False)
        finally:
            self.rf.break_requested = original
            done.set()
            thread.join(5.0)
            self.rf.shutdown()
        rec["t_return"] = time.monotonic()
        STATE["cancel"] = rec
        self.go("view_setup")
        return 0.5

    # ---- view

    def step_view_setup(self):
        win, area, region = self.area()
        with bpy.context.temp_override(window=win, area=area, region=region):
            bpy.ops.screen.screen_full_area(use_hide_panels=True)
        with bpy.context.temp_override(window=win):
            bpy.ops.wm.window_fullscreen_toggle()
        self.go("view_on")
        return 1.5

    def step_view_on(self):
        _, area, region = self.area()
        space = area.spaces.active
        space.overlay.show_overlays = False
        space.show_gizmo = False
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
        STATE["region"] = (region.width, region.height)
        self.go("first_frame")
        return 0.2

    def stamps(self):
        held = self.rf._contexts.get("CUDA")
        self.job = self.job or next((j for j in (held.jobs if held else ()) if getattr(j, "stamps", None) is not None), None)
        return self.job.stamps if self.job else None

    def rotate(self, deg):
        _, area, _ = self.area()
        rv3d = area.spaces.active.region_3d
        rv3d.view_rotation = mathutils.Quaternion((0, 0, 1), math.radians(deg)) @ rv3d.view_rotation
        area.tag_redraw()

    def step_first_frame(self):
        st = self.stamps()
        if st is None or not any(f["presented"] for f in st["frames"]):
            if self.waited(120):
                raise RuntimeError("no frame presented")
            return 0.1
        self.go("settle")
        return 1.5

    def step_settle(self):
        self.go("change")
        return 0.0

    def step_change(self):
        st = self.stamps()
        self.t_change = time.monotonic()
        self.epoch0 = max(f["epoch"] for f in st["frames"] if f["presented"])
        self.rotate(5.0)
        self.go("change_wait")
        return 0.0

    def step_change_wait(self):
        st = self.stamps()
        sv = next((d for d in st["draws"] if d["t"] >= self.t_change and d["set_view"]), None)
        # Ruling 69: a view-only latch keeps the generation number, so the new view's frame is told apart by its
        # epoch, and by a pass resolved in it (a frame read just after the latch but published before it has 0).
        fr = next((f for f in st["frames"] if sv and f["t"] >= sv["t"] and f["presented"] and f["epoch"] > self.epoch0 and f["spp"] >= 1), None)
        draw = next((d for d in st["draws"] if fr and d["frame"] and d["t"] <= fr["t"] <= d["t"] + d["dur_s"]), None)
        if draw is None:
            if self.waited(10):
                after = [d for d in st["draws"] if d["t"] >= self.t_change]
                raise RuntimeError(f"no frame of a newer epoch 10 s after view change {self.n + 1}: {len(after)} draws, "
                                   f"{sum(d['set_view'] for d in after)} set_view, epoch0 {self.epoch0}, frames after "
                                   f"{[(f['epoch'], f['spp']) for f in st['frames'] if f['t'] >= self.t_change][:5]}")
            return 0.0
        near = [f["pass_ms"] for f in st["frames"] if self.t_change - 1.0 <= f["t"] <= fr["t"]]
        ts = [d["t"] for d in st["draws"] if self.t_change - 0.5 <= d["t"] <= fr["t"]]
        STATE.setdefault("changes", []).append({
            "to_screen_ms": (draw["t"] + draw["dur_s"] - sv["t"]) * 1e3, "from_change_ms": (draw["t"] + draw["dur_s"] - self.t_change) * 1e3,
            "pass_ms": max(near), "redraw_ms": float(np.median(np.diff(ts))) * 1e3 if len(ts) > 2 else 0.0,
            "present_ms": (draw["upload_s"] + draw["draw_s"]) * 1e3})
        self.n += 1
        self.go("change" if self.n < CHANGES else "orbit_start")
        return 1.0

    def step_orbit_start(self):
        self.t_a = time.monotonic()
        self.go("orbit")
        return 0.0

    def step_orbit(self):
        self.rotate(DEG)
        if not self.waited(ORBIT_S):
            return 0.0
        STATE["orbit_window"] = (self.t_a, time.monotonic())
        STATE["stamps"] = {k: list(v) for k, v in self.stamps().items()}
        return self.end()


class Case(unittest.TestCase):
    def phase_ok(self, phase):
        if phase in STATE["errors"]:
            self.fail(f"phase {phase} failed:\n{STATE['errors'][phase]}")

    def orbit_frames(self):
        self.phase_ok("view")
        a, b = STATE["orbit_window"]
        fr = [f for f in STATE["stamps"]["frames"] if a <= f["t"] <= b]
        self.assertGreaterEqual(len(fr), 10, "the orbit starved the display: fewer than 10 frames in 3 s")
        return fr


class TestLatency(Case):
    def test_cancel_stops_at_next_launch_boundary(self):
        """0007 d27, 0009 B-32/B-17: from the break to the engine's PAUSED within B-17's 100 ms (target 20 ms)."""
        self.phase_ok("cancel")
        rec = STATE["cancel"]
        self.assertIn("t_stop", rec, "the session never left RENDERING after the break")
        ms, loop_ms = (rec["t_stop"] - rec["t_break"]) * 1e3, (rec["t_return"] - rec["t_break"]) * 1e3
        print(f"RESULT latency {SCENE} cancel: engine PAUSED {ms:.1f} ms after the break (state {rec['state']}), "
              f"F12 returned {loop_ms:.0f} ms after it, break after {rec['first_pass_s']:.1f} s; B-17 target 20 ms, limit {CANCEL_LIMIT_MS:.0f} ms")
        self.assertIn(rec["state"], tuple(rec["abi"].values()), "a cancel ends PAUSED or TRIMMED")
        self.assertLessEqual(ms, CANCEL_LIMIT_MS)

    @unittest.skip("task 130: kInteractionPublishGapS is waited before each interaction pass")
    def test_view_change_latency_within_b32(self):
        """0007 d25 / B-32: a view change reaches the screen (the first frame of a newer epoch, with a pass of it
        resolved) within the rest of the pass in flight + one pass + present, in every one of the single changes."""
        self.phase_ok("view")
        ch = STATE["changes"]
        self.assertEqual(len(ch), CHANGES)
        bound = [2 * c["pass_ms"] + c["redraw_ms"] + c["present_ms"] for c in ch]
        print(f"RESULT latency {SCENE} view change: region {STATE['region']}, to screen from set_view "
              f"{[round(c['to_screen_ms'], 1) for c in ch]} ms (median {np.median([c['to_screen_ms'] for c in ch]):.1f}), from the "
              f"change {[round(c['from_change_ms'], 1) for c in ch]} ms; bound {[round(b, 1) for b in bound]} ms "
              f"(pass {[round(c['pass_ms'], 1) for c in ch]}, redraw {[round(c['redraw_ms'], 1) for c in ch]}, "
              f"present {[round(c['present_ms'], 1) for c in ch]})")
        for c, b in zip(ch, bound):
            self.assertLessEqual(c["to_screen_ms"], b)

    @unittest.skip("task 130: kInteractionPublishGapS is waited before each interaction pass")
    def test_orbit_frames_are_for_newer_views(self):
        """B-32: during the orbit a new frame follows every completed pass: every frame the host acquires is a
        new one (its id above the last), is shown, and the gaps between shown frames stay within the bound of
        one view change (the 99th percentile of the gaps against 2 passes + a redraw + a present, each at its
        own 99th percentile; the frame's epoch is not on the frame, ruling 69, so no per-frame epoch is asserted)."""
        fr = self.orbit_frames()
        ids = [f["id"] for f in fr]
        notshown = [f["id"] for f in fr if not f["presented"]]
        a, b = STATE["orbit_window"]
        draws = [d for d in STATE["stamps"]["draws"] if a <= d["t"] <= b]
        gaps = np.diff([f["t"] for f in fr]) * 1e3
        bound = 2 * np.percentile([f["pass_ms"] for f in fr], 99) + float(np.median(np.diff([d["t"] for d in draws]))) * 1e3             + np.percentile([(d["upload_s"] + d["draw_s"]) * 1e3 for d in draws if d["frame"]], 99)
        print(f"RESULT latency {SCENE} orbit: {len(fr)} frames in {ORBIT_S:.0f} s, gap median {np.median(gaps):.1f} p99 "
              f"{np.percentile(gaps, 99):.1f} max {gaps.max():.1f} ms against bound {bound:.1f} ms, not presented {len(notshown)}")
        self.assertEqual(sorted(set(ids)), ids, "a frame was acquired twice or out of order")
        self.assertEqual(notshown, [])
        self.assertLessEqual(float(np.percentile(gaps, 99)), bound)

    def test_orbit_shows_no_stale_frame(self):
        """0007 d25: once a frame of generation g was shown no frame of an older generation follows, and no
        frame is shown twice."""
        fr = [f for f in self.orbit_frames() if f["presented"]]
        gens, ids = [f["generation"] for f in fr], [f["id"] for f in fr]
        stale = [i for i in range(1, len(fr)) if gens[i] < max(gens[:i])]
        twice = len(ids) - len(set(ids))
        print(f"RESULT latency {SCENE} orbit: {len(fr)} presented, stale {len(stale)}, shown twice {twice}")
        self.assertEqual(stale, [])
        self.assertEqual(twice, 0)


def run_cases():
    loader = unittest.defaultTestLoader
    if "-k" in ARGS:  # land's canary runs one named case
        pat = ARGS[ARGS.index("-k") + 1]
        loader.testNamePatterns = [pat if "*" in pat else f"*{pat}*"]
    res = unittest.TextTestRunner(stream=sys.stdout, verbosity=2).run(loader.loadTestsFromTestCase(TestLatency))
    os.makedirs(EVIDENCE, exist_ok=True)
    s = STATE.get("stamps")
    json.dump({"scene": SCENE, "region": STATE.get("region"), "cancel": {k: v for k, v in STATE.get("cancel", {}).items() if k != "abi"},
               "changes": STATE.get("changes"), "orbit_frames": len(s["frames"]) if s else None},
              open(os.path.join(EVIDENCE, f"latency_{SCENE}.json"), "w"), indent=1)
    finish(0 if res.wasSuccessful() else 1)


def main():
    os.environ["ASTRORAY_FRAME_LOG"] = "1"
    work = arg("--work", os.path.join(ROOT, "out", "smoke"))
    os.makedirs(work, exist_ok=True)
    blend = os.path.join(work, f"{SCENE}_latency.blend")
    extra = ["--sampler", "sobol"] if SCENE == "a1_furnace" else []   # a .blend cannot carry another sampler
    p = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "scenes", SCENE, "build.py"),
                        "--out", os.path.join(work, f"{SCENE}_latency.arrec"), "--blend", blend] + extra,
                       capture_output=True, text=True, timeout=600)
    if p.returncode != 0:
        raise RuntimeError(f"scenes/{SCENE}/build.py failed: " + p.stdout + p.stderr)
    smoke_a1.install_and_enable()
    rf = importlib.import_module(MODULE + ".render_final")
    blend_writer.build(blend + ".json", blend)
    bpy.ops.wm.open_mainfile(filepath=blend, load_ui=False)
    scene = bpy.context.scene
    scene.render.engine = "ASTRORAY"
    scene.astroray.device = "CUDA"
    scene.astroray.viewport_samples = 1000000
    if scene.camera is not None:
        scene.camera.data.passepartout_alpha = 0.0
    bpy.app.timers.register(Driver(rf).tick, first_interval=1.0)


if __name__ == "__main__":
    try:
        main()
    except Exception:
        STATE["errors"]["cancel"] = STATE["errors"]["view"] = traceback.format_exc()
        print(STATE["errors"]["view"], flush=True)
        run_cases()
