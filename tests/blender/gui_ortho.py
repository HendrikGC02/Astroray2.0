# SPDX-License-Identifier: Apache-2.0
"""An orthographic Rendered viewport tells the user (task 159; BACKLOG: orthographic viewport warning).

Run through `Invoke-ArBlender -Gui -Script tests/blender/gui_ortho.py`. A windowed Blender goes to Rendered
shading on the A1 scene, the 3D view is switched to an orthographic (non-camera) view, and every call of
`engine.report` the job makes while forwarding engine report entries is recorded. The engine builds a perspective view from an orthographic
projection and raises CAMERA_UNSUPPORTED "ORTHO" (src/host/scene/camera.cpp); the viewport forwards report
entries to `engine.report`, so a WARNING naming that code must arrive there, which Blender shows in its status
bar. The frame itself stays wrong until orthographic rays exist (M4) and is not asserted.
A timer drives the sequence (the viewport only draws while the main loop runs); the script ends the process.
Ref: blender/astroray/viewport.py (`_report`); tests/blender/gui_edits.py (the driving pattern).
"""
import importlib
import os
import subprocess
import sys
import time
import traceback
import unittest

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
for sub in ("tools", os.path.join("scenes", "lib")):
    sys.path.insert(0, os.path.join(ROOT, sub))
import blend_writer  # noqa: E402
import smoke_a1  # noqa: E402  (the zip install and the argument helpers)

MODULE = smoke_a1.MODULE
ARGS = smoke_a1.ARGS
WATCHDOG_S = 200.0
STEP_TIMEOUT_S = 40.0
T0 = time.monotonic()
STATE = {"errors": {}, "reports": []}


def finish(code):
    print(f"gui_ortho exit {code}", flush=True)
    os._exit(code)  # skips Blender's window teardown


def run_cases():
    print("RESULT engine.report calls: " + repr(STATE["reports"]), flush=True)
    res = unittest.TextTestRunner(stream=sys.stdout, verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(TestOrtho))
    finish(0 if res.wasSuccessful() else 1)


class Driver:
    def __init__(self):
        self.step_name, self.step_t0 = "setup", time.monotonic()

    def go(self, name):
        self.step_name, self.step_t0 = name, time.monotonic()

    def find_area(self):
        for win in bpy.context.window_manager.windows:
            for area in win.screen.areas:
                if area.type == "VIEW_3D":
                    return area
        raise RuntimeError("no 3D viewport")

    def tick(self):
        try:
            if time.monotonic() - T0 > WATCHDOG_S:
                raise RuntimeError(f"watchdog: still in step {self.step_name}")
            return getattr(self, "step_" + self.step_name)()
        except Exception:
            STATE["errors"]["run"] = traceback.format_exc()
            print(STATE["errors"]["run"], flush=True)
            return run_cases()

    def step_setup(self):
        rf = importlib.import_module(MODULE + ".render_final")
        orig = rf.Job._report

        class Recorder:
            """The engine as the job sees it, every report kept before Blender gets it."""

            def __init__(self, engine):
                self.engine = engine

            def report(self, kinds, message):
                STATE["reports"].append((sorted(kinds), message))
                return self.engine.report(kinds, message)

        rf.Job._report = lambda job, engine, reported, revision: orig(job, Recorder(engine), reported, revision)
        space = self.find_area().spaces.active
        space.shading.type = "RENDERED"
        space.overlay.show_overlays = False
        space.region_3d.view_perspective = "ORTHO"
        self.go("wait")
        return 0.5

    def step_wait(self):
        if any("CAMERA_UNSUPPORTED" in m for _, m in STATE["reports"]):
            return run_cases()
        if time.monotonic() - self.step_t0 > STEP_TIMEOUT_S:
            return run_cases()
        return 0.2


class TestOrtho(unittest.TestCase):
    def test_orthographic_viewport_reports_unsupported(self):
        if "run" in STATE["errors"]:
            self.fail(STATE["errors"]["run"])
        hits = [r for r in STATE["reports"] if "CAMERA_UNSUPPORTED" in r[1] and "ORTHO" in r[1]]
        self.assertTrue(hits, f"no CAMERA_UNSUPPORTED ORTHO report; got {STATE['reports']}")
        self.assertIn("WARNING", hits[0][0])


def main():
    work = smoke_a1.arg("--work", os.path.join(ROOT, "out", "smoke"))
    os.makedirs(work, exist_ok=True)
    blend = os.path.join(work, "a1_ortho.blend")
    p = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "scenes", "a1_furnace", "build.py"),
                        "--out", os.path.join(work, "a1_ortho.arrec"), "--blend", blend, "--sampler", "sobol"],
                       capture_output=True, text=True, timeout=300)
    if p.returncode != 0:
        raise RuntimeError("scenes/a1_furnace/build.py failed: " + p.stdout + p.stderr)
    smoke_a1.install_and_enable()
    blend_writer.build(blend + ".json", blend)
    bpy.ops.wm.open_mainfile(filepath=blend, load_ui=False)
    scene = bpy.context.scene
    scene.render.engine = "ASTRORAY"
    scene.astroray.device = "CPU"
    scene.astroray.viewport_samples = 16
    bpy.app.timers.register(Driver().tick, first_interval=1.0)


if __name__ == "__main__":
    try:
        main()
    except Exception:
        STATE["errors"]["run"] = traceback.format_exc()
        print(STATE["errors"]["run"], flush=True)
        run_cases()
