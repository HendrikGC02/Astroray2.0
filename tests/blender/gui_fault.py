# SPDX-License-Identifier: Apache-2.0
"""GPU fault containment inside a windowed Blender (task 109): A1 in Rendered shading on CUDA, with the
gate hook ASTRORAY_TEST_GPU_FAULT=1 making the CUDA device fault before its first launch.

Run through `Invoke-ArBlender -Gui -Script tests/blender/gui_fault.py` (the land canary adds `-k <case>`).
The script sets the hook itself, before the engine DLL loads, so the same run works from land.
Everything is driven by timers (the viewport only draws while Blender's main loop runs) and the script ends
the process itself, exit code 1 on any failed case. The sequence:
  fault:  viewport on CUDA -> the session fails; Blender is alive (the timers keep running)
  after:  the device list, a CUDA F12 refused with the restart text
  cpu:    leave Rendered, F12 on the CPU; the image is not black.
Needs a gate-hook build (the dev preset: test_fault.cu loose in out/bin/kernels) and a CUDA device.
Ref: docs/decisions/0007-blender-integration.md d44 (addon after a fault), V15; 0001 D2.5 (the message);
tests/abi/gpu_test_fault_cli.py (the engine half of this sequence).
"""
import importlib
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
import smoke_a1  # noqa: E402  (the zip install and the argument helpers)

MODULE = smoke_a1.MODULE
ARGS = smoke_a1.ARGS
RESTART = "Save your work and restart Blender."
WATCHDOG_S = 240.0   # land's canary run gives Blender 300 s
STEP_TIMEOUT_S = 60.0
T0 = time.monotonic()
STATE = {"errors": {}}


def finish(code):
    print(f"gui_fault exit {code}", flush=True)
    sys.stdout.flush()
    os._exit(code)  # skips Blender's window teardown; shutdown() already destroyed the contexts


class Driver:
    def __init__(self, rf, native):
        self.rf, self.native = rf, native
        self.step_name, self.step_t0 = "setup", time.monotonic()
        self.job = None

    def go(self, name):
        self.step_name, self.step_t0 = name, time.monotonic()

    def timed_out(self, limit=STEP_TIMEOUT_S):
        return time.monotonic() - self.step_t0 > limit

    def find_area(self):
        for win in bpy.context.window_manager.windows:
            for area in win.screen.areas:
                if area.type == "VIEW_3D":
                    return area
        raise RuntimeError("no 3D viewport")

    def tick(self):
        try:
            STATE["ticks"] = STATE.get("ticks", 0) + 1
            if time.monotonic() - T0 > WATCHDOG_S:
                STATE["errors"]["watchdog"] = f"watchdog: still in step {self.step_name}"
                return self.end()
            return getattr(self, "step_" + self.step_name)()
        except Exception:
            STATE["errors"][self.step_name] = traceback.format_exc()
            print(f"step {self.step_name} failed:\n{STATE['errors'][self.step_name]}", flush=True)
            return self.end()

    def step_setup(self):
        space = self.find_area().spaces.active
        space.shading.type = "RENDERED"
        space.overlay.show_overlays = False
        space.region_3d.view_perspective = "CAMERA"
        self.go("fault")
        return 0.2

    def step_fault(self):
        held = self.rf._contexts.get("CUDA")
        self.job = next(iter(held.jobs), None) if held else None
        if self.job is None or not self.job.failure:
            if self.timed_out():
                raise RuntimeError(f"the viewport never failed; job {self.job and self.job.events}")
            return 0.2
        STATE["failure"] = self.job.failure
        STATE["report"] = self.native.cuda_lost()
        STATE["ticks_at_failure"] = STATE["ticks"]
        self.go("alive")
        return 0.5

    def step_alive(self):
        # The main loop kept running after the fault: more timer ticks, and a redraw of the failed viewport
        # raises nothing.
        STATE["alive"] = STATE["ticks"] > STATE["ticks_at_failure"]
        STATE["devices"] = list(self.native.devices())
        scene = bpy.context.scene
        scene.astroray.device = "CUDA"
        STATE["lost"] = self.native.cuda_lost()
        scene.render.resolution_percentage = 25
        bpy.context.preferences.view.render_display_type = "NONE"
        try:
            bpy.ops.render.render()
            STATE["cuda_refusal"] = None
        except RuntimeError as e:
            STATE["cuda_refusal"] = str(e)
        scene.astroray.device = "CPU"
        self.find_area().spaces.active.shading.type = "SOLID"  # Blender frees the engine
        self.go("cpu_render")
        return 1.0

    def step_cpu_render(self):
        scene = bpy.context.scene
        scene.astroray.samples = 8
        scene.render.resolution_percentage = 25
        out = os.path.join(ROOT, "out", "evidence", "109", "cpu.png")
        os.makedirs(os.path.dirname(out), exist_ok=True)
        scene.render.filepath = out
        scene.render.image_settings.file_format = "PNG"
        bpy.context.preferences.view.render_display_type = "NONE"
        STATE["f12"] = bpy.ops.render.render(write_still=True)
        img = bpy.data.images.load(out, check_existing=False)
        px = np.array(img.pixels[:], np.float32).reshape(-1, 4)
        STATE["cpu_mean"], STATE["cpu_size"] = float(px[:, :3].mean()), tuple(img.size)
        return self.end()

    def end(self):
        try:
            self.rf.shutdown()
        except Exception:
            print("shutdown failed:\n" + traceback.format_exc(), flush=True)
        run_cases()
        return None


class TestFault(unittest.TestCase):
    def test_blender_survives_and_reports(self):
        """Blender is alive after the fault and the report carries the engine's message with 0001's restart
        text."""
        self.assertEqual(STATE["errors"], {})
        self.assertTrue(STATE["alive"], "no timer tick after the fault")
        print(f"RESULT fault report: {STATE['report']!r}")
        self.assertIsNotNone(STATE["report"], "the addon did not take the GPU error as CUDA lost")
        self.assertIn("CUDA_ERROR_ILLEGAL_ADDRESS", STATE["report"])
        self.assertIn(RESTART, STATE["report"])
        self.assertIn("AR_ERR_DEVICE", STATE["report"])

    def test_cpu_only_after_fault(self):
        """The device list is the CPU alone and a CUDA F12 is refused, naming the restart."""
        print(f"RESULT fault devices {STATE.get('devices')}, CUDA refusal {STATE.get('cuda_refusal')!r}")
        self.assertEqual(STATE.get("devices"), ["CPU"])
        self.assertIsNotNone(STATE.get("cuda_refusal"))
        self.assertIn(RESTART, STATE["cuda_refusal"])

    def test_cpu_f12_completes(self):
        """An F12 render on the CPU, in the same process, finishes with a non-black image."""
        print(f"RESULT fault cpu F12 {STATE.get('f12')} mean {STATE.get('cpu_mean')} size {STATE.get('cpu_size')}")
        self.assertEqual(STATE.get("f12"), {"FINISHED"})
        self.assertGreater(STATE.get("cpu_mean", 0.0), 0.05)


def run_cases():
    loader = unittest.defaultTestLoader
    if "-k" in ARGS:  # land's canary runs one named case
        pat = ARGS[ARGS.index("-k") + 1]
        loader.testNamePatterns = [pat if "*" in pat else f"*{pat}*"]
    res = unittest.TextTestRunner(stream=sys.stdout, verbosity=2).run(loader.loadTestsFromTestCase(TestFault))
    finish(0 if res.wasSuccessful() else 1)


def main():
    # Before the engine DLL loads: its device reads the hook once, when the CUDA device is created, and
    # the fault module is a loose file in a dev build.
    os.environ["ASTRORAY_TEST_GPU_FAULT"] = "1"
    os.environ["ASTRORAY_KERNEL_DIR"] = os.path.join(ROOT, "out", "bin", "kernels")
    work = smoke_a1.arg("--work", os.path.join(ROOT, "out", "smoke"))
    os.makedirs(work, exist_ok=True)
    blend = os.path.join(work, "a1_fault.blend")
    p = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "scenes", "a1_furnace", "build.py"),
                        "--out", os.path.join(work, "a1_fault.arrec"), "--blend", blend, "--sampler", "sobol"],
                       capture_output=True, text=True, timeout=300)
    if p.returncode != 0:
        raise RuntimeError("scenes/a1_furnace/build.py failed: " + p.stdout + p.stderr)
    smoke_a1.install_and_enable()
    rf = importlib.import_module(MODULE + ".render_final")
    native = importlib.import_module(MODULE + ".native")
    blend_writer.build(blend + ".json", blend)
    bpy.ops.wm.open_mainfile(filepath=blend, load_ui=False)
    scene = bpy.context.scene
    scene.render.engine = "ASTRORAY"
    scene.astroray.device = "CUDA"
    scene.astroray.viewport_samples = 16
    driver = Driver(rf, native)
    bpy.app.timers.register(driver.tick, first_interval=1.0)


if __name__ == "__main__":
    try:
        main()
    except Exception:
        STATE["errors"]["main"] = traceback.format_exc()
        print(STATE["errors"]["main"], flush=True)
        run_cases()
