# SPDX-License-Identifier: Apache-2.0
"""Progressive batching and the heat-map display modes in a windowed Blender (task 086), on A1.

Run through `Invoke-ArBlender -Gui -Script tests/blender/gui_batching.py` (the land canary adds `-k <case>`).
Timers drive everything; the script ends the process itself, exit code 1 on any failed case.
  batch: the 3D view is cut down to a small area (a pass takes milliseconds, so the batch size can grow),
         Rendered, a scripted orbit (a view change every 20 ms for 1.2 s), then 3 s of rest. The viewport
         job's frame log records every presented frame with its time and the epoch's samples per pixel.
  heat:  the area maximised, view transform AgX, Sample count and then Bounce count; a screenshot of each
         goes to out/evidence/086/ and is compared with the engine's own RGBA8 frame (0007 V10).
A phase that fails records its error and the run goes on, so a case fails only for its own phase.
Ref: docs/decisions/0007-blender-integration.md d40 (heat maps outside the view transform, P4), d49, d50,
V10, V23; 0009 B-34 (60/s during interaction, 10/s after); viridis end colours: Smith and van der Walt
2015 (matplotlib's table: 0 is (68, 1, 84), 1 is (253, 231, 37)).
"""
import importlib
import json
import os
import subprocess
import sys
import time
import traceback
import unittest

import numpy as np

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
for sub in ("tools", os.path.join("scenes", "lib")):
    sys.path.insert(0, os.path.join(ROOT, sub))
import blend_writer  # noqa: E402
import gui_present  # noqa: E402  (the windowed driver: area lookup, job lookup, maximise)
import smoke_a1  # noqa: E402

MODULE = smoke_a1.MODULE
ARGS = smoke_a1.ARGS
WATCHDOG_S = 270.0
STEP_TIMEOUT_S = 40.0
ORBIT_S = 1.2
REST_S = 3.0
T0 = time.monotonic()
STATE = {"errors": {}}
EVIDENCE = os.path.join(ROOT, "out", "evidence", "086")
VIRIDIS_0, VIRIDIS_1 = (68, 1, 84), (253, 231, 37)


def finish(code):
    print(f"gui_batching exit {code}", flush=True)
    sys.stdout.flush()
    os._exit(code)


class Driver(gui_present.Driver):
    def __init__(self, rf):
        super().__init__(rf)
        self.step_name = "setup"

    def phase(self):
        return "heat" if self.step_name.startswith("h_") else "batch"

    def tick(self):
        try:
            if time.monotonic() - T0 > WATCHDOG_S:
                STATE["errors"]["watchdog"] = f"watchdog: still in step {self.step_name}"
                print(STATE["errors"]["watchdog"], flush=True)
                return self.end()
            return getattr(self, "step_" + self.step_name)()
        except Exception:
            phase = self.phase()
            STATE["errors"][phase] = traceback.format_exc()
            print(f"phase {phase} failed:\n{STATE['errors'][phase]}", flush=True)
            if phase == "heat":
                return self.end()
            self.go("h_start")  # the heat phase starts from whatever state is left
            return 0.1

    def timed_out(self, limit=STEP_TIMEOUT_S):
        return super().timed_out(limit)

    def screenshot(self, name):
        win, area, region = self.find_area()
        out = os.path.join(EVIDENCE, name)
        with bpy.context.temp_override(window=win, area=area, region=region):
            bpy.ops.screen.screenshot_area(filepath=out)
        STATE.setdefault("screenshots", {})[name] = (out, (region.x - area.x, region.y - area.y, region.width, region.height))

    def end(self):
        self.rf.shutdown()
        run_cases()
        return None

    # ---- batch phase

    def step_setup(self):
        """Cut the 3D view down: split twice, turn every larger 3D view into a Properties editor."""
        win, area, region = self.find_area()
        for direction, factor in (("VERTICAL", 0.1), ("HORIZONTAL", 0.2)):
            win, area, region = self.find_area()
            with bpy.context.temp_override(window=win, area=area, region=region):
                bpy.ops.screen.area_split(direction=direction, factor=factor)
            views = [a for a in win.screen.areas if a.type == "VIEW_3D"]
            small = min(views, key=lambda a: a.width * a.height)
            for a in views:
                if a is not small:
                    a.type = "PROPERTIES"
        win, area, region = self.find_area()
        space = area.spaces.active
        space.shading.type = "RENDERED"
        space.overlay.show_overlays = False
        space.show_gizmo = False
        for name in ("show_region_header", "show_region_toolbar", "show_region_ui", "show_region_tool_header",
                     "show_region_hud"):
            if hasattr(space, name):
                setattr(space, name, False)
        space.region_3d.view_perspective = "CAMERA"
        STATE["small_region"] = (region.width, region.height)
        self.go("first_frame")
        return 0.2

    def step_first_frame(self):
        self.job = self.job or self.find_job()
        if self.job is None:
            if self.timed_out():
                raise RuntimeError("no viewport job appeared")
            return 0.2
        if self.job.frames is None:
            self.job.frames = []
            self.job.acquired = []
        if not self.job.frames:
            if self.timed_out():
                raise RuntimeError(f"no frame presented; events {self.job.events}")
            return 0.1
        self.go("rest1")
        return 1.0

    def step_rest1(self):
        self.go("orbit")
        STATE["t_orbit_start"] = time.monotonic()
        self.ticks = 0
        return 0.0

    def step_orbit(self):
        _, area, _ = self.find_area()
        area.spaces.active.region_3d.view_camera_zoom += 0.3  # a view change: no depsgraph update
        STATE["t_orbit_last"] = time.monotonic()
        self.ticks += 1
        if STATE["t_orbit_last"] - STATE["t_orbit_start"] >= ORBIT_S:
            self.go("rest2")
            return REST_S
        return 0.02

    def step_rest2(self):
        STATE["batch_frames"] = list(self.job.frames)
        STATE["orbit_ticks"] = self.ticks
        STATE["batch_events"] = list(self.job.events)
        self.go("h_start")
        return 0.1

    # ---- heat phase

    def step_h_start(self):
        if STATE.get("small_region"):
            self.toggle_maximize()  # a region that outgrows the session gets a new session (085)
            STATE["maximized"] = True
        scene = bpy.context.scene
        scene.view_settings.view_transform = "AgX"
        scene.view_settings.look = "None"
        scene.astroray.viewport_samples = 8
        scene.astroray.display_mode = "SAMPLE_COUNT"
        STATE["h_t0"] = time.monotonic()
        self.go("h_samples")
        return 0.5

    def heat_frame(self, mode):
        job = self.find_job()
        if job is None:
            return None
        got = [f for f in job.frames if f.get("heat") and f["mode"] == mode]
        return got[-1] if got else None

    def step_h_samples(self):
        job = self.find_job()
        self.find_area()[1].tag_redraw()  # a frame published after the last draw would otherwise wait
        if job is None or not self.heat_converged(job, 1):
            if self.timed_out():
                raise RuntimeError(f"no converged Sample count frame; events {job and job.events}")
            return 0.2
        self.job = job
        self.go("h_samples_shot")
        return 0.7

    def heat_converged(self, job, mode):
        f = self.heat_frame(mode)
        return f is not None and f["size"][0] > 1000 and self.converged_job(job)

    def converged_job(self, job):
        import ctypes
        prog = job.abi.ar_progress()
        job.lib.ar_session_get_progress(job.session, ctypes.byref(prog))
        return prog.state == job.abi.AR_STATE_CONVERGED

    def step_h_samples_shot(self):
        STATE["samples_frame"] = self.heat_frame(1)
        self.screenshot("heatmap_samples.png")
        bpy.context.scene.astroray.display_mode = "BOUNCE_COUNT"
        self.go("h_bounces")
        return 0.5

    def step_h_bounces(self):
        job = self.find_job()
        self.find_area()[1].tag_redraw()
        if job is None or not self.heat_converged(job, 2):
            if self.timed_out():
                raise RuntimeError(f"no converged Bounce count frame; events {job and job.events}")
            return 0.2
        self.go("h_bounces_shot")
        return 0.7

    def step_h_bounces_shot(self):
        STATE["bounces_frame"] = self.heat_frame(2)
        self.screenshot("heatmap_bounces.png")
        STATE["heat_events"] = list(self.job.events)
        return self.end()


def region_pixels(name):
    """The screenshot's region as (h, w, 3) uint8, raw: Non-Color so Blender does not convert the bytes."""
    path, (x0, y0, w, h) = STATE["screenshots"][name]
    img = bpy.data.images.load(path)
    img.colorspace_settings.name = "Non-Color"
    px = np.array(img.pixels[:], np.float32).reshape(img.size[1], img.size[0], 4)[:, :, :3]
    return np.rint(px[y0:y0 + h, x0:x0 + w] * 255.0).astype(np.int16)


def rate(frames, t0, t1):
    n = sum(1 for f in frames if t0 <= f["t"] < t1)
    return n, n / (t1 - t0)


class Case(unittest.TestCase):
    phase = "batch"

    def setUp(self):
        if "watchdog" in STATE["errors"]:
            self.fail(STATE["errors"]["watchdog"])
        if self.phase in STATE["errors"]:
            self.fail(f"the {self.phase} phase did not complete:\n" + STATE["errors"][self.phase])


class TestBatching(Case):
    def test_one_pass_per_update_during_interaction(self):
        """During the orbit each displayed update is one pass: the epoch has at most 2 samples per pixel
        at every presented frame (a frame is acquired a moment after it was published), and frames come at
        most 60/s (0009 B-34, with a 10% margin for the clock of the draw)."""
        fr = STATE["batch_frames"]
        t0, t1 = STATE["t_orbit_start"] + 0.1, STATE["t_orbit_last"]
        during = [f for f in fr if t0 <= f["t"] <= t1]
        n, r = rate(fr, t0, t1)
        spp = [f["spp"] for f in during]
        print(f"RESULT batching orbit: {STATE['orbit_ticks']} view changes, {n} frames in {t1 - t0:.2f} s = {r:.1f}/s, "
              f"samples per frame max {max(spp, default=0)}, median {np.median(spp) if spp else 0}")
        self.assertGreaterEqual(n, 6)
        self.assertLessEqual(max(spp), 2)
        self.assertLessEqual(r, 66.0)

    def test_passes_per_update_grow_after_interaction(self):
        """After the window the epoch's samples jump by more per displayed update, growing as it settles."""
        fr = STATE["batch_frames"]
        t0 = STATE["t_orbit_last"] + 0.5
        after = [f for f in fr if f["t"] >= t0]
        spp = [f["spp"] for f in after]
        steps = np.diff(spp)
        print(f"RESULT batching rest: {len(after)} frames, samples {spp[:3]}..{spp[-3:]}, steps "
              f"{steps[:4].tolist()}..{steps[-4:].tolist()}")
        self.assertGreaterEqual(len(after), 5)
        self.assertGreaterEqual(int(steps.max()), 4)
        half = len(steps) // 2
        self.assertGreater(float(steps[half:].mean()), float(steps[:half].mean()))

    def test_publish_rate_after_interaction(self):
        """0009 B-34: no more than 10 frames/s once interaction has ended (a 20% margin)."""
        fr = STATE["batch_frames"]
        t0 = STATE["t_orbit_last"] + 0.5
        t1 = STATE["t_orbit_last"] + REST_S
        n, r = rate(fr, t0, t1)
        print(f"RESULT batching rest: {n} frames in {t1 - t0:.2f} s = {r:.1f}/s")
        self.assertGreaterEqual(n, 3)
        self.assertLessEqual(r, 12.0)


class TestHeatmap(Case):
    phase = "heat"

    def check_screen(self, name, frame):
        reg = region_pixels(name)
        px = frame["bytes"][:, :, :3].astype(np.int16)
        self.assertEqual(reg.shape, px.shape)
        d = np.abs(reg - px)
        print(f"RESULT heat {name}: region {px.shape[1]}x{px.shape[0]}, max |screen - frame| {int(d.max())}/255, "
              f"pixels differing by more than 1: {int((d.max(axis=2) > 1).sum())}")
        return reg, px, d

    def test_viridis_exact(self):
        """0007 V10: under AgX the screen shows the engine's sRGB bytes exactly (+-1/255), and a converged
        Sample count frame is viridis(1.0) everywhere."""
        f = STATE["samples_frame"]
        reg, px, d = self.check_screen("heatmap_samples.png", f)
        flat = px.reshape(-1, 3)
        print(f"RESULT heat samples frame: min {flat.min(axis=0).tolist()} max {flat.max(axis=0).tolist()}, "
              f"screen mean {reg.reshape(-1, 3).mean(axis=0).round(2).tolist()}")
        self.assertLessEqual(int(np.abs(flat - np.array(VIRIDIS_1)).max()), 1)
        self.assertLessEqual(int(d.max()), 1)

    def test_bounce_count_patches(self):
        """The furnace's sky is 0 bounces (viridis 0), the sphere has paths that bounce, and the screen
        again equals the frame."""
        f = STATE["bounces_frame"]
        reg, px, d = self.check_screen("heatmap_bounces.png", f)
        sky = px[:4, :4].reshape(-1, 3)
        flat = px.reshape(-1, 3)
        colours = np.unique(flat, axis=0)
        print(f"RESULT heat bounces: corner {sky.mean(axis=0).round(1).tolist()}, {len(colours)} distinct colours, "
              f"{int((np.abs(flat - np.array(VIRIDIS_0)).max(axis=1) > 8).sum())} pixels off viridis(0)")
        self.assertLessEqual(int(np.abs(sky - np.array(VIRIDIS_0)).max()), 1)
        self.assertGreater(int((np.abs(flat - np.array(VIRIDIS_0)).max(axis=1) > 8).sum()), 500)
        self.assertLessEqual(int(d.max()), 1)

    def test_mode_reaches_the_engine(self):
        """Switching the property sends the mode to the session (Beauty first, then 1, then 2)."""
        hm = importlib.import_module(MODULE + ".heatmap_draw")
        print(f"RESULT heat route: {hm.ROUTE}")
        modes = [e for e in STATE["heat_events"] if e.startswith("display_mode")]
        self.assertEqual([m for m in modes if m != "display_mode 0"][-2:], ["display_mode 1", "display_mode 2"])


def run_cases():
    loader = unittest.defaultTestLoader
    if "-k" in ARGS:  # land's canary runs one named case
        pat = ARGS[ARGS.index("-k") + 1]
        loader.testNamePatterns = [pat if "*" in pat else f"*{pat}*"]
    suite = unittest.TestSuite()
    for c in (TestBatching, TestHeatmap):
        suite.addTests(loader.loadTestsFromTestCase(c))
    res = unittest.TextTestRunner(stream=sys.stdout, verbosity=2).run(suite)
    json.dump({"small_region": STATE.get("small_region"), "orbit_ticks": STATE.get("orbit_ticks")},
              open(os.path.join(EVIDENCE, "run.json"), "w"), indent=1)
    finish(0 if res.wasSuccessful() else 1)


def main():
    work = smoke_a1.arg("--work", os.path.join(ROOT, "out", "smoke"))
    os.makedirs(work, exist_ok=True)
    os.makedirs(EVIDENCE, exist_ok=True)
    blend = os.path.join(work, "a1_batching.blend")
    p = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "scenes", "a1_furnace", "build.py"),
                        "--out", os.path.join(work, "a1_batching.arrec"), "--blend", blend, "--sampler", "sobol"],
                       capture_output=True, text=True, timeout=300)
    if p.returncode != 0:
        raise RuntimeError("scenes/a1_furnace/build.py failed: " + p.stdout + p.stderr)
    smoke_a1.install_and_enable()
    rf = importlib.import_module(MODULE + ".render_final")
    blend_writer.build(blend + ".json", blend)
    bpy.ops.wm.open_mainfile(filepath=blend, load_ui=False)
    scene = bpy.context.scene
    scene.render.engine = "ASTRORAY"
    scene.astroray.device = "CPU"
    scene.astroray.viewport_samples = 1024  # never converges during the batch phase
    scene.camera.data.passepartout_alpha = 0.0
    driver = Driver(rf)
    bpy.app.timers.register(driver.tick, first_interval=1.0)


if __name__ == "__main__":
    try:
        main()
    except Exception:
        STATE["errors"]["main"] = traceback.format_exc()
        print(STATE["errors"]["main"], flush=True)
        run_cases()
