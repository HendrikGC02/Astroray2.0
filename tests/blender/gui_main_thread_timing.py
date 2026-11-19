# SPDX-License-Identifier: Apache-2.0
"""Main-thread engine call times in a windowed Blender on B-5M (task 094, 0007 V22, 0009 B-21).

Run through `Invoke-ArBlender -Gui -Script tests/blender/gui_main_thread_timing.py` (the land canary adds `-k <case>`).
The addon's ctypes library is wrapped before any job exists, so every ABI call the addon makes on Blender's main
thread is timed: the first scene and session create, the first full sync of S2 (2,000 meshes, 10,000 instances, 300
materials), a move, a material edit, a lamp edit, a vertex edit, a view change, display acquire and release on every
draw, session close and destroy. The timer-driven sequence ends the process itself; exit code 1 on any failed case.
A call from another thread is not counted. The limit is B-21's, read from tools/budgets.toml; only the calls that copy
data the host supplied are exempt (EXEMPT), each with the reason it is a copy.
Ref: docs/decisions/0007-blender-integration.md V22, d23; docs/decisions/0009-build-test-process.md B-21;
tests/blender/gui_edits.py (the driving pattern).
"""
import importlib
import os
import statistics
import subprocess
import sys
import threading
import time
import tomllib
import traceback
import unittest

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
for sub in ("tools", os.path.join("scenes", "lib")):
    sys.path.insert(0, os.path.join(ROOT, sub))
import blend_writer  # noqa: E402
import smoke_a1  # noqa: E402

MODULE = smoke_a1.MODULE
ARGS = smoke_a1.ARGS
WATCHDOG_S = 270.0   # land's canary run gives Blender 300 s
STEP_TIMEOUT_S = 150.0
T0 = time.monotonic()
MAIN = threading.get_ident()
STATE = {"errors": {}, "phase": "setup", "seconds": None, "frames": 0}
CALLS = []  # (function, milliseconds, phase) of every call made on the main thread

# Calls whose time is the copy of arrays the host passed in (B-21: "except copying data the host supplied").
# ar_edit_set_instances copies every per-instance array (about 10,000 x 150 bytes on S2).
EXEMPT = {"ar_edit_set_instances": "copies the host's per-instance arrays"}


def b21_limit_ms():
    with open(os.path.join(ROOT, "tools", "budgets.toml"), "rb") as f:
        row = next(b for b in tomllib.load(f)["budget"] if b["id"] == "B-21")
    if row.get("unit") != "ms":
        raise RuntimeError("tools/budgets.toml B-21 has no limit in ms")
    return float(row["limit"])


def finish(code):
    print(f"gui_main_thread_timing exit {code}", flush=True)
    os._exit(code)  # skips Blender's window teardown; shutdown() already destroyed the contexts


class TimedLib:
    """Stands in for the addon's ctypes library: times each ar_* call made on the main thread."""

    def __init__(self, lib):
        self._lib, self._wrapped = lib, {}

    def __getattr__(self, name):
        fn = getattr(self._lib, name)
        if not name.startswith("ar_"):
            return fn
        w = self._wrapped.get(name)
        if w is None:
            def w(*a, _fn=fn, _name=name):
                t = time.perf_counter()
                try:
                    return _fn(*a)
                finally:
                    if threading.get_ident() == MAIN:
                        CALLS.append((_name, (time.perf_counter() - t) * 1e3, STATE["phase"]))
            w.__name__ = name
            self._wrapped[name] = w
        return w


def summary():
    by = {}
    for name, ms, phase in CALLS:
        by.setdefault(name, []).append((ms, phase))
    return by


class Driver:
    """The timer-driven sequence; each step returns the next timer interval or None when finished."""

    def __init__(self, rf):
        self.rf, self.job, self.step_name, self.step_t0 = rf, None, "setup", time.monotonic()
        self.edits = [("move", self.e_move), ("material", self.e_material), ("lamp", self.e_lamp),
                      ("move_again", self.e_move), ("vertex", self.e_vertex), ("view", self.e_view)]
        self.cur = None

    def go(self, name):
        self.step_name, self.step_t0 = name, time.monotonic()
        STATE["phase"] = name

    def timed_out(self, limit=STEP_TIMEOUT_S):
        return time.monotonic() - self.step_t0 > limit

    def tick(self):
        try:
            if time.monotonic() - T0 > WATCHDOG_S:
                STATE["errors"]["watchdog"] = f"watchdog: still in step {self.step_name}"
                print(STATE["errors"]["watchdog"], flush=True)
                return self.end()
            return getattr(self, "step_" + self.step_name)()
        except Exception:
            STATE["errors"][self.step_name] = traceback.format_exc()
            print(f"step {self.step_name} failed:\n{STATE['errors'][self.step_name]}", flush=True)
            return self.end()

    def find_area(self):
        for win in bpy.context.window_manager.windows:
            for area in win.screen.areas:
                if area.type == "VIEW_3D":
                    return win, area, next(r for r in area.regions if r.type == "WINDOW")
        raise RuntimeError("no 3D viewport")

    def find_job(self):
        held = self.rf._contexts.get("CUDA")
        return next((j for j in (held.jobs if held else ()) if hasattr(j, "frames")), None)

    def progress(self):
        import ctypes
        prog = self.job.abi.ar_progress()
        self.job.lib.ar_session_get_progress(self.job.session, ctypes.byref(prog))
        return prog

    # ---- the edits (the objects exist in B-5M's blend: scenes/lib/blend_writer.py)

    def e_move(self):
        self.mover.location.x += 0.5

    def e_material(self):
        bpy.data.materials[0].node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (0.9, 0.1, 0.1, 1)

    def e_lamp(self):
        self.lamp.data.energy *= 2.0

    def e_vertex(self):
        self.mover.data.vertices[0].co.z += 0.1

    def e_view(self):
        win, area, region = self.find_area()
        rv3d = area.spaces.active.region_3d
        rv3d.view_camera_offset[0] += 0.05  # a camera view that moves is a new view hash (0007 d49)
        area.tag_redraw()

    # ---- steps

    def step_setup(self):
        scene = bpy.context.scene
        self.mover = next(o for o in scene.objects if o.type == "MESH" and o.parent is None and o.data.users == 1)
        self.lamp = next(o for o in scene.objects if o.type == "LIGHT")
        win, area, region = self.find_area()
        space = area.spaces.active
        space.shading.type = "RENDERED"
        space.overlay.show_overlays = False
        space.region_3d.view_perspective = "CAMERA"
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
        if not self.job.frames:
            if self.timed_out():
                raise RuntimeError(f"no frame presented; events {self.job.events}, failure {self.job.failure}")
            return 0.1
        STATE["first_frame_s"] = time.monotonic() - T0
        self.go("settle")
        return 0.1

    def settled(self):
        p = self.progress()
        return (p.state == self.job.abi.AR_STATE_CONVERGED and p.active_generation == p.pending_generation
                and self.job.frames and self.job.frames[-1]["generation"] >= p.active_generation)

    def step_settle(self):
        if not self.settled():
            if self.timed_out(60.0):
                raise RuntimeError("the viewport never converged")
            return 0.1
        self.go("next")
        return 0.3

    def step_next(self):
        if not self.edits:
            return self.close()
        name, apply = self.edits.pop(0)
        self.cur = {"name": name, "active": self.progress().active_generation, "events": len(self.job.events)}
        self.go("edit_" + name)
        apply()
        return 0.1

    def step_edit(self):
        c = self.cur
        if len(self.job.events) == c["events"]:
            if self.timed_out(30.0):
                raise RuntimeError(f"no sync event after the {c['name']} edit; events {self.job.events[-4:]}")
            return 0.1
        if c["name"] != "view" and self.progress().active_generation == c["active"]:
            if self.timed_out(60.0):
                raise RuntimeError(f"the {c['name']} edit never activated")
            return 0.1
        if not self.settled():
            if self.timed_out(60.0):
                raise RuntimeError(f"the {c['name']} edit never settled")
            return 0.1
        self.go("next")
        return 0.2

    def __getattr__(self, name):
        if name.startswith("step_edit_"):
            return self.step_edit
        raise AttributeError(name)

    def close(self):
        """Switching to solid shading drops the engine instance: ar_session_close from __del__, the reaper timer
        then destroys the session and the scene."""
        self.go("closing")
        win, area, region = self.find_area()
        area.spaces.active.shading.type = "SOLID"
        return 0.2

    def step_closing(self):
        held = self.rf._contexts.get("CUDA")
        if held is not None and any(j.scene_h.value or j.session.value or j.retired for j in held.jobs):
            if self.timed_out(30.0):
                raise RuntimeError("the viewport job was not torn down")
            return 0.2
        return self.end()

    def end(self):
        STATE["seconds"] = time.monotonic() - T0
        try:
            self.rf.shutdown()
        except Exception:
            STATE["errors"]["shutdown"] = traceback.format_exc()
            print(STATE["errors"]["shutdown"], flush=True)
        finally:
            report()
            run_cases()  # ends the process, whatever happened above
        return None


def report():
    limit = b21_limit_ms()
    for name, rows in sorted(summary().items()):
        ms = [m for m, _ in rows]
        worst = max(rows)
        print(f"RESULT main-thread call {name}: n={len(ms)} median {statistics.median(ms):.3f} ms max {worst[0]:.3f} ms "
              f"(in {worst[1]}){' EXEMPT: ' + EXEMPT[name] if name in EXEMPT else ''}{' OVER' if worst[0] > limit and name not in EXEMPT else ''}",
              flush=True)
    print(f"RESULT B-21 limit {limit} ms; first frame at {STATE.get('first_frame_s')} s; run {STATE['seconds']} s", flush=True)


class TestMainThreadTiming(unittest.TestCase):
    def setUp(self):
        for k in ("setup", "first_frame", "settle", "watchdog", "shutdown"):
            if k in STATE["errors"]:
                self.fail(f"the run failed in {k}:\n{STATE['errors'][k]}")

    def test_every_call_the_task_names_was_made_on_the_main_thread(self):
        """The instrument saw the first scene and session create, the edits, display acquire and release, and the
        close and destroy of the session and scene: a call that is never made is not within the limit, it is unmeasured."""
        seen = {name for name, _, _ in CALLS}
        want = {"ar_context_create", "ar_scene_create", "ar_session_create", "ar_session_start", "ar_session_set_view",
                "ar_scene_edit_begin", "ar_edit_set_instances", "ar_edit_mesh_begin", "ar_edit_item_commit",
                "ar_edit_update_transforms", "ar_edit_set_material", "ar_edit_set_light", "ar_edit_commit",
                "ar_display_acquire", "ar_display_release", "ar_session_get_progress", "ar_session_close",
                "ar_session_destroy", "ar_scene_destroy"}
        self.assertEqual(sorted(want - seen), [], "calls never made on the main thread")

    def test_no_call_exceeds_b21(self):
        """B-21: any engine call on Blender's main thread takes at most 5 ms, except copying host data."""
        limit = b21_limit_ms()
        over = {}
        for name, ms, phase in CALLS:
            if ms > limit and name not in EXEMPT and ms > over.get(name, (0, ""))[0]:
                over[name] = (ms, phase)
        self.assertEqual(over, {}, f"calls above {limit} ms: (milliseconds, phase)")

    def test_exemptions_are_calls_that_copy_host_data(self):
        """Only the calls EXEMPT names are excused, and each must have been made (an unused entry is dead weight)."""
        seen = {name for name, _, _ in CALLS}
        self.assertEqual(sorted(set(EXEMPT) - seen), [])


def run_cases():
    loader = unittest.defaultTestLoader
    if "-k" in ARGS:  # land's canary runs one named case
        pat = ARGS[ARGS.index("-k") + 1]
        loader.testNamePatterns = [pat if "*" in pat else f"*{pat}*"]
    res = unittest.TextTestRunner(stream=sys.stdout, verbosity=2).run(loader.loadTestsFromTestCase(TestMainThreadTiming))
    finish(0 if res.wasSuccessful() else 1)


def main():
    work = smoke_a1.arg("--work", os.path.join(ROOT, "out", "smoke"))
    os.makedirs(work, exist_ok=True)
    blend = os.path.join(work, "b5m_timing.blend")
    p = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "scenes", "b5m", "build.py"),
                        "--out", os.path.join(work, "b5m_timing.arrec"), "--blend", blend],
                       capture_output=True, text=True, timeout=300)
    if p.returncode != 0:
        raise RuntimeError("scenes/b5m/build.py failed: " + p.stdout + p.stderr)
    smoke_a1.install_and_enable()
    native = importlib.import_module(MODULE + ".native")
    rf = importlib.import_module(MODULE + ".render_final")
    native._lib = TimedLib(native.load())  # before any job: every job takes its library from native.load()
    blend_writer.build(blend + ".json", blend)
    bpy.ops.wm.open_mainfile(filepath=blend, load_ui=False)
    scene = bpy.context.scene
    scene.render.engine = "ASTRORAY"
    scene.astroray.device = "CUDA"
    scene.astroray.viewport_samples = 4
    bpy.app.timers.register(Driver(rf).tick, first_interval=1.0)


if __name__ == "__main__":
    try:
        main()
    except Exception:
        STATE["errors"]["setup"] = traceback.format_exc()
        print(STATE["errors"]["setup"], flush=True)
        run_cases()
