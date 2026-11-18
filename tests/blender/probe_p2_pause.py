# SPDX-License-Identifier: Apache-2.0
"""Probe P2 (task 097): does Blender 5.3 call `view_pause` / `view_resume` on the viewport engines when F12 starts?

Run through `Invoke-ArBlender -Gui -Script tests/blender/probe_p2_pause.py`. Two 3D views are put in Rendered
shading, then F12 is started the way the key does (`render.render` invoked, which runs the render job and
opens the render window). Every engine callback of interest is logged with the identity of the engine
instance: the viewport engines get `view_update`/`view_draw`, the F12 engine gets `update`/`render`. The probe
prints the sequence, then one case checks the expectation of 0007 d28: each viewport engine is paused before
the F12 `update` and resumed after the F12 `render` has returned. The addon's engine has no such methods, so
the probe adds loggers to the class (unregister, add, register) before any viewport exists.
Exit code 1 on a failed case; the script ends the process itself.
Ref: docs/decisions/0007-blender-integration.md d28 and Risks P2; docs/research/01-blender-boundary.md B1
(`view_pause(context)`, `view_resume(context)` are optional RenderEngine callbacks, as in Cycles).
"""
import importlib
import json
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
import gui_present  # noqa: E402  (the windowed driver: area lookup)
import smoke_a1  # noqa: E402

MODULE = smoke_a1.MODULE
ARGS = smoke_a1.ARGS
WATCHDOG_S = 200.0
STEP_TIMEOUT_S = 40.0
T0 = time.monotonic()
STATE = {"errors": {}, "log": []}
EVIDENCE = os.path.join(ROOT, "out", "evidence", "097")


def finish(code):
    print(f"probe_p2_pause exit {code}", flush=True)
    sys.stdout.flush()
    os._exit(code)


def instrument(engine_cls):
    """Adds loggers for the callbacks under test; the originals still run."""
    log = STATE["log"]

    def wrap(name):
        orig = getattr(engine_cls, name, None)

        def call(self, *a, **k):
            log.append((time.monotonic() - T0, name, id(self)))
            return orig(self, *a, **k) if orig else None
        call.__name__ = name
        setattr(engine_cls, name, call)

    for name in ("view_update", "update", "render"):
        wrap(name)
    for name in ("view_pause", "view_resume"):
        def make(n):
            def call(self, context):
                log.append((time.monotonic() - T0, n, id(self)))
            call.__name__ = n
            return call
        setattr(engine_cls, name, make(name))
    bpy.utils.unregister_class(engine_cls)
    bpy.utils.register_class(engine_cls)


class Driver(gui_present.Driver):
    def phase(self):
        return "f12"

    def tick(self):
        try:
            if time.monotonic() - T0 > WATCHDOG_S:
                STATE["errors"]["watchdog"] = f"watchdog: still in step {self.step_name}"
                print(STATE["errors"]["watchdog"], flush=True)
                return self.end()
            return getattr(self, "step_" + self.step_name)()
        except Exception:
            STATE["errors"]["f12"] = traceback.format_exc()
            print(f"phase f12 failed:\n{STATE['errors']['f12']}", flush=True)
            return self.end()

    def timed_out(self, limit=STEP_TIMEOUT_S):
        return super().timed_out(limit)

    def end(self):
        self.rf.shutdown()
        run_cases()
        return None

    def step_setup(self):
        win, area, region = self.find_area()
        with bpy.context.temp_override(window=win, area=area, region=region):
            bpy.ops.screen.area_split(direction="VERTICAL", factor=0.5)
        views = [a for a in win.screen.areas if a.type == "VIEW_3D"]
        if len(views) != 2:
            raise RuntimeError(f"expected two 3D views, found {len(views)}")
        for a in views:
            a.spaces.active.shading.type = "RENDERED"
            a.spaces.active.region_3d.view_perspective = "CAMERA"
        self.go("two_viewports")
        return 0.5

    def viewport_jobs(self):
        held = self.rf._contexts.get("CPU")
        vp = importlib.import_module(MODULE + ".viewport")
        return [j for j in (held.jobs if held else ()) if isinstance(j, vp.ViewportJob)]

    def step_two_viewports(self):
        jobs = self.viewport_jobs()
        for _, area, _ in self.areas():
            area.tag_redraw()
        if len(jobs) < 2 or any(j.t_first_frame is None for j in jobs):
            if self.timed_out():
                raise RuntimeError(f"viewport jobs {len(jobs)}, first frames {[j.t_first_frame for j in jobs]}")
            return 0.2
        STATE["viewport_ids"] = {id(j._engine()) for j in jobs if j._engine and j._engine()}
        self.go("f12")
        return 0.5

    def areas(self):
        win = bpy.context.window_manager.windows[0]
        return [(win, a, next(r for r in a.regions if r.type == "WINDOW")) for a in win.screen.areas if a.type == "VIEW_3D"]

    def step_f12(self):
        win, area, region = self.areas()[0]
        STATE["log"].append((time.monotonic() - T0, "F12_start", 0))
        with bpy.context.temp_override(window=win, area=area, region=region):
            r = bpy.ops.render.render("INVOKE_DEFAULT")
        print(f"render.render INVOKE_DEFAULT -> {r}", flush=True)
        self.go("f12_wait")
        self.saw_job = False
        return 0.2

    def step_f12_wait(self):
        running = bpy.app.is_job_running("RENDER")
        self.saw_job = self.saw_job or running
        if self.saw_job and not running:
            STATE["log"].append((time.monotonic() - T0, "F12_done", 0))
            self.go("settle")
            return 2.0
        if self.timed_out():
            raise RuntimeError(f"F12 never finished (job seen {self.saw_job})")
        return 0.2

    def step_settle(self):
        return self.end()


class TestPause(unittest.TestCase):
    def setUp(self):
        if "watchdog" in STATE["errors"]:
            self.fail(STATE["errors"]["watchdog"])
        if "f12" in STATE["errors"] or "main" in STATE["errors"]:
            self.fail("the probe did not complete:\n" + "\n".join(STATE["errors"].values()))

    def seq(self):
        names = {}
        out = []
        for t, name, ident in STATE["log"]:
            label = names.setdefault(ident, f"E{len(names)}") if ident else "-"
            out.append((round(t, 2), name, label))
        return out

    def test_viewports_paused_during_f12(self):
        """0007 d28 / P2: every viewport engine instance gets view_pause after F12 starts and before F12's
        update, and view_resume after F12's render returned."""
        log = STATE["log"]
        by = lambda n: [(t, i) for t, nm, i in log if nm == n]  # noqa: E731
        start = by("F12_start")[0][0]
        done = by("F12_done")[0][0]
        pauses, resumes = by("view_pause"), by("view_resume")
        print("PROBE P2 sequence (time s, callback, engine):")
        for row in self.seq():
            if row[1] != "view_update" or start - 1 < row[0] < done + 3:
                print("   ", row)
        print(f"PROBE P2 result: view_pause calls {len(pauses)}, view_resume calls {len(resumes)}, "
              f"viewport engines {len(STATE.get('viewport_ids', ()))}")
        vids = STATE["viewport_ids"]
        self.assertEqual({i for _, i in pauses} & vids, vids, "view_pause was not called on every viewport engine")
        first_update = min(t for t, _ in by("update"))
        self.assertTrue(all(start <= t <= first_update for t, _ in pauses), "view_pause came outside F12's start")
        last_render = max(t for t, _ in by("render"))
        self.assertEqual({i for _, i in resumes} & vids, vids, "view_resume was not called on every viewport engine")
        self.assertTrue(all(t >= last_render for t, _ in resumes), "view_resume came before F12's render returned")


def run_cases():
    loader = unittest.defaultTestLoader
    if "-k" in ARGS:
        pat = ARGS[ARGS.index("-k") + 1]
        loader.testNamePatterns = [pat if "*" in pat else f"*{pat}*"]
    res = unittest.TextTestRunner(stream=sys.stdout, verbosity=2).run(loader.loadTestsFromTestCase(TestPause))
    json.dump([list(r) for r in STATE["log"]], open(os.path.join(EVIDENCE, "p2_log.json"), "w"))
    finish(0 if res.wasSuccessful() else 1)


def main():
    work = smoke_a1.arg("--work", os.path.join(ROOT, "out", "smoke"))
    os.makedirs(work, exist_ok=True)
    os.makedirs(EVIDENCE, exist_ok=True)
    blend = os.path.join(work, "a1_p2.blend")
    p = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "scenes", "a1_furnace", "build.py"),
                        "--out", os.path.join(work, "a1_p2.arrec"), "--blend", blend, "--sampler", "sobol"],
                       capture_output=True, text=True, timeout=300)
    if p.returncode != 0:
        raise RuntimeError("scenes/a1_furnace/build.py failed: " + p.stdout + p.stderr)
    smoke_a1.install_and_enable()
    rf = importlib.import_module(MODULE + ".render_final")
    eng = importlib.import_module(MODULE + ".engine")
    instrument(eng.AstrorayRenderEngine)
    blend_writer.build(blend + ".json", blend)
    bpy.ops.wm.open_mainfile(filepath=blend, load_ui=False)
    scene = bpy.context.scene
    scene.render.engine = "ASTRORAY"
    scene.astroray.device = "CPU"
    scene.astroray.viewport_samples = 64
    scene.render.resolution_x = scene.render.resolution_y = 64  # a short F12
    scene.camera.data.passepartout_alpha = 0.0
    driver = Driver(rf)
    bpy.app.timers.register(driver.tick, first_interval=1.0)


if __name__ == "__main__":
    try:
        main()
    except Exception:
        STATE["errors"]["main"] = traceback.format_exc()
        print(STATE["errors"]["main"], flush=True)
        os.makedirs(EVIDENCE, exist_ok=True)
        run_cases()
