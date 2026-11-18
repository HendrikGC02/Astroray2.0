# SPDX-License-Identifier: Apache-2.0
"""Viewport record and replay (task 111, 0001 V11) in a windowed Blender, A1 in Rendered shading.

Run through `Invoke-ArBlender -Gui -Script tests/blender/gui_record_replay.py` (the land canary adds `-k <case>`).
ASTRORAY_RECORD is set before the first context exists, so the recorder opens with it. A timer-driven run then
syncs A1 into a viewport session (the viewport polls progress on every draw), waits for convergence, moves the
sphere, waits, changes its material, waits, and reads the live film of that last state with
ar_session_write_fits (the calls driven directly through the ABI, in this process). Shutdown completes the
recording. `astroray_cli render --device cpu --seed 7` then replays it (the scene's seed is 7, so the override
changes nothing) and the two films must agree bit for bit on the CPU device; the recording must hold no AR_QUERY
call and exactly the first sync and the two edits as commits.
Ref: docs/decisions/0001-product-shape.md V11, D1.4, D2.10; tools/integrator_checks.py check_record_replay (the
comparison and the query rule); tests/blender/gui_present.py (the driving pattern).
"""
import ctypes
import importlib
import importlib.util
import os
import re
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
import integrator_checks as ic  # noqa: E402  (compare_films, read_fits, the query list)
import smoke_a1  # noqa: E402

MODULE = smoke_a1.MODULE
ARGS = smoke_a1.ARGS
WATCHDOG_S = 200.0
STEP_TIMEOUT_S = 40.0
SEED = 7
T0 = time.monotonic()
STATE = {"errors": {}}
WORK = smoke_a1.arg("--work", os.path.join(ROOT, "out", "smoke"))
REC = os.path.join(WORK, "viewport_sync.arrec")
LIVE = os.path.join(WORK, "viewport_live.fits")
REPLAY = os.path.join(WORK, "viewport_replay.fits")


def finish(code):
    print(f"gui_record_replay exit {code}", flush=True)
    os._exit(code)


class Driver:
    """first frame -> settled -> move -> settled -> material -> settled -> live film -> shutdown."""

    def __init__(self, rf):
        self.rf, self.job, self.step_name, self.t0 = rf, None, "setup", time.monotonic()
        self.before = 0

    def go(self, name):
        self.step_name, self.t0 = name, time.monotonic()

    def timed_out(self, limit=STEP_TIMEOUT_S):
        return time.monotonic() - self.t0 > limit

    def tick(self):
        try:
            if time.monotonic() - T0 > WATCHDOG_S:
                raise RuntimeError(f"watchdog: still in step {self.step_name}")
            return getattr(self, "step_" + self.step_name)()
        except Exception:
            STATE["errors"]["run"] = traceback.format_exc()
            print(STATE["errors"]["run"], flush=True)
            return self.end()

    def progress(self):
        prog = self.job.abi.ar_progress()
        self.job.lib.ar_session_get_progress(self.job.session, ctypes.byref(prog))
        return prog

    def settled(self, above):
        p = self.progress()
        return (p.state == self.job.abi.AR_STATE_CONVERGED and p.active_generation == p.pending_generation
                and p.active_generation > above)

    def step_setup(self):
        for win in bpy.context.window_manager.windows:
            for area in win.screen.areas:
                if area.type == "VIEW_3D":
                    space = area.spaces.active
                    space.shading.type = "RENDERED"
                    space.region_3d.view_perspective = "CAMERA"
        self.go("first")
        return 0.2

    def step_first(self):
        held = self.rf._contexts.get("CPU")
        self.job = next((j for j in (held.jobs if held else ()) if hasattr(j, "frames")), None)
        if self.job is None or not self.job.session.value or not self.settled(0):
            if self.timed_out():
                raise RuntimeError("the first generation never converged")
            return 0.2
        STATE["first_generation"] = self.progress().active_generation
        self.sphere = next(o for o in bpy.context.scene.objects if o.type == "MESH")
        self.sphere.location.x += 1.0
        self.go("settle_move")
        return 0.2

    def step_settle_move(self):
        gen = STATE["first_generation"]
        if not self.settled(gen):
            if self.timed_out():
                raise RuntimeError(f"the move never settled; events {self.job.events}")
            return 0.2
        STATE["move_generation"] = self.progress().active_generation
        self.sphere.location.z += 0.3  # the second plain move: a transform patch
        self.go("settle_move2")
        return 0.2

    def step_settle_move2(self):
        if not self.settled(STATE["move_generation"]):
            if self.timed_out():
                raise RuntimeError(f"the second move never settled; events {self.job.events}")
            return 0.2
        STATE["move_generation"] = self.progress().active_generation
        self.sphere.material_slots[0].material.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = \
            (0.9, 0.25, 0.05, 1.0)
        self.go("settle_material")
        return 0.2

    def step_settle_material(self):
        if not self.settled(STATE["move_generation"]):
            if self.timed_out():
                raise RuntimeError(f"the material edit never settled; events {self.job.events}")
            return 0.2
        STATE["events"] = list(self.job.events)
        abi = self.job.abi
        st = self.job.lib.ar_session_write_fits(self.job.session, LIVE.encode(), abi.AR_FITS_DUMP)
        if st != 0:
            raise RuntimeError(f"ar_session_write_fits returned {st}")
        return self.end()

    def end(self):
        try:
            self.rf.shutdown()  # the recording is complete only after its context is destroyed
        except Exception:
            STATE["errors"]["run"] = STATE["errors"].get("run", "") + traceback.format_exc()
            print(STATE["errors"]["run"], flush=True)
        finally:
            run_cases()  # ends the process, whatever happened above


class TestReplay(unittest.TestCase):
    def setUp(self):
        if "run" in STATE["errors"]:
            self.fail("the recording run did not complete:\n" + STATE["errors"]["run"])

    def test_bit_identical(self):
        """The CLI's replay of the viewport recording equals the live film of the same calls, bit for bit."""
        env = {k: v for k, v in os.environ.items() if not k.startswith("ASTRORAY_")}
        env["ASTRORAY_TEST_THREADS"] = "1"
        r = subprocess.run([ic.analytic.CLI, "render", REC, "-o", REPLAY, "--device", "cpu", "--seed", str(SEED)],
                           capture_output=True, text=True, cwd=WORK, env=env, timeout=600)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        live, replay = ic.read_fits(LIVE)[1], ic.read_fits(REPLAY)[1]
        ok, text = ic.compare_films(live, replay, "cpu")
        print(f"RESULT replay viewport recording, CLI vs live film: {text}", flush=True)
        self.assertTrue(ok, text)

    def test_recording_shape(self):
        """No AR_QUERY call is recorded (0001 V11), the first sync and the three edits are the only commits, and
        the second move went out as a transform patch."""
        with open(ic.RECORDED_QUERIES, encoding="utf-8") as f:
            queries = set(re.findall(r"AR_QUERY\s+\w+\s+(\w+)\s*\(", f.read()))
        spec = importlib.util.spec_from_file_location("_arrec_rr", os.path.join(ROOT, "out", "build", "gen", "arrec.py"))
        arrec = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(arrec)
        _, calls = arrec.decode(REC)
        names = [n for n, _ in calls]
        print(f"RESULT replay recording: {len(calls)} calls, {names.count('ar_edit_commit')} commits, "
              f"{names.count('ar_edit_set_instances')} instance tables, {names.count('ar_edit_update_transforms')} patches, events {STATE['events']}", flush=True)
        self.assertTrue(queries, "no AR_QUERY name was read from astroray.h: the rule below would hold for anything")
        self.assertFalse(set(names) & queries, sorted(set(names) & queries))
        self.assertEqual(names.count("ar_edit_commit"), 4)
        # The first move after the full sync builds the instance table (a known inefficiency), the second is a patch.
        self.assertEqual(names.count("ar_edit_update_transforms"), 1)
        self.assertEqual(names.count("ar_edit_set_instances"), 2)
        self.assertEqual(STATE["events"].count("full"), 1)
        self.assertEqual([e for e in STATE["events"] if e.startswith("delta")], ["delta instances", "delta instances", "delta materials,geometry"])


def run_cases():
    loader = unittest.defaultTestLoader
    if "-k" in ARGS:
        pat = ARGS[ARGS.index("-k") + 1]
        loader.testNamePatterns = [pat if "*" in pat else f"*{pat}*"]
    res = unittest.TextTestRunner(stream=sys.stdout, verbosity=2).run(loader.loadTestsFromTestCase(TestReplay))
    finish(0 if res.wasSuccessful() else 1)


def main():
    os.makedirs(WORK, exist_ok=True)
    blend = os.path.join(WORK, "a1_replay.blend")
    p = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "scenes", "a1_furnace", "build.py"),
                        "--out", os.path.join(WORK, "a1_replay.arrec"), "--blend", blend, "--sampler", "sobol"],
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
    scene.astroray.seed = SEED
    scene.astroray.viewport_samples = 16
    scene.camera.data.passepartout_alpha = 0.0
    for stale in (REC, LIVE, REPLAY):
        if os.path.exists(stale):
            os.remove(stale)
    os.environ["ASTRORAY_RECORD"] = REC  # before the first context: the recorder opens with it
    bpy.app.timers.register(Driver(rf).tick, first_interval=1.0)


if __name__ == "__main__":
    try:
        main()
    except Exception:
        STATE["errors"]["run"] = traceback.format_exc()
        print(STATE["errors"]["run"], flush=True)
        run_cases()
