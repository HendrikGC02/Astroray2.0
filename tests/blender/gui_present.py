# SPDX-License-Identifier: Apache-2.0
"""The viewport present, in a windowed Blender (task 085): A1 in Rendered shading.

Run through `Invoke-ArBlender -Gui -Script tests/blender/gui_present.py` (the land canary adds `-k <case>`).
Everything is driven by timers (the viewport only draws while Blender's main loop runs) and the script ends
the process itself, with exit code 1 on any failed case. One run walks the whole sequence, then the cases
read what it recorded:
  main:  switch to Rendered -> first frame -> move the sphere (twice) -> change the view -> leave Rendered
  grow:  Rendered again with a coloured sphere -> GPU read-back of the unpacked texture against the host
         frame (small test stripes) -> maximise the area, so the region outgrows the session -> leave Rendered
  scn:   three teardown scenarios, each its own phase: a second retirement overlapping the final one (the
         final session closes first), a failed timer registration at park, a failed session create during a
         resize followed by park
  p2:    a scene without a camera, with a lamp and a textured plane -> first frame, screenshot.
A phase that fails records its error and the run goes on to the next, so a case fails only for its own phase.
The viewport job keeps a lifecycle `events` list and, when asked, a log of every presented frame
(generation and a down-sampled copy of the pixels), so the cases compare numbers, never looks.
A screenshot of the viewport area goes to out/evidence/085/viewport.png.
Ref: docs/decisions/0007-blender-integration.md d23, d25, d26, d29, d49; docs/spikes/B-blender-end-to-end
gui_common.py (the windowed-Blender driving pattern).
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
import smoke_a1  # noqa: E402  (the zip install and the argument helpers)

MODULE = smoke_a1.MODULE
ARGS = smoke_a1.ARGS
WATCHDOG_S = 270.0  # land's canary run gives Blender 300 s
SETTLE_S = 2.0       # let the first generation render a while before the sphere moves
STEP_TIMEOUT_S = 25.0  # every step takes a few seconds; a failing phase must not eat the budget
T0 = time.monotonic()
STATE = {"errors": {}}
SCENARIOS = ("overlap", "failpark", "failcreate")
SMALL_STRIPE_ROWS = 100  # rows per stripe in the read-back check: 13 stripes at the 2100x1221 region


class LibSpy:
    """Stands in for a job's ctypes library: logs the close and destroy calls where they are made, with the
    state the job last saw for that handle, and can keep a closed session reporting RENDERING for a number of
    polls, so a destroy that does not wait for CLOSED shows."""

    def __init__(self, lib, abi):
        self._lib, self._abi = lib, abi
        self.calls, self.hold, self.last_state = [], {}, {}
        self.fail_create = False
        self.destroyed = set()

    def __getattr__(self, name):
        fn = getattr(self._lib, name)
        if name == "ar_session_get_progress":
            def get(session, ref):
                r = fn(session, ref)
                prog, key = ref._obj, session.value
                if self.hold.get(key, 0) > 0 and prog.state == self._abi.AR_STATE_CLOSED:
                    self.hold[key] -= 1
                    prog.state = self._abi.AR_STATE_RENDERING
                self.last_state[key] = prog.state
                self.calls.append(("progress", key, prog.state, time.monotonic()))
                return r
            return get
        if name == "ar_session_create" and self.fail_create:
            return lambda *a: 1  # any status but AR_OK
        if name in ("ar_session_close", "ar_session_destroy", "ar_scene_destroy"):
            def call(handle, *a):
                self.calls.append((name, handle.value, self.last_state.get(handle.value), time.monotonic()))
                if name == "ar_session_destroy":
                    again = handle.value in self.destroyed
                    self.destroyed.add(handle.value)
                    if again:  # recorded as a violation; not passed on, a second destroy would corrupt the engine
                        return 0
                return fn(handle, *a)
            return call
        return fn

    def native(self, key=None):
        return [c for c in self.calls if c[0] != "progress" and (key is None or c[1] == key)]


def finish(code):
    print(f"gui_present exit {code}", flush=True)
    sys.stdout.flush()
    os._exit(code)  # skips Blender's window teardown; shutdown() already destroyed the contexts


def dark_centroid(rgb):
    """(x, y, count) of the sphere in a down-sampled frame: pixels below the middle of 0.8 and 0.4.
    x and y are fractions of the frame, y growing with the frame's row index."""
    mask = rgb.mean(axis=2) < 0.6
    ys, xs = np.nonzero(mask)
    if not len(xs):
        return None
    return xs.mean() / mask.shape[1], ys.mean() / mask.shape[0], int(len(xs))


class Driver:
    """The timer-driven sequence; each step returns the next timer interval or None when finished."""

    def __init__(self, rf):
        self.rf, self.job, self.step_name, self.step_t0 = rf, None, "setup", time.monotonic()
        self.pre = None
        self.screen = None
        self.todo = list(SCENARIOS)

    def find_area(self):
        for win in bpy.context.window_manager.windows:
            for area in win.screen.areas:
                if area.type == "VIEW_3D":
                    return win, area, next(r for r in area.regions if r.type == "WINDOW")
        raise RuntimeError("no 3D viewport")

    def find_job(self):
        held = self.rf._contexts.get("CPU")
        for j in (held.jobs if held else ()):
            if hasattr(j, "frames"):
                return j
        return None

    def go(self, name):
        self.step_name, self.step_t0 = name, time.monotonic()

    def timed_out(self):
        return time.monotonic() - self.step_t0 > STEP_TIMEOUT_S

    def phase(self):
        n = self.step_name
        if n.startswith("s_"):
            return "scn:" + self.scn
        return "grow" if n.startswith("g_") else "p2" if n.startswith(("cam_", "p2_")) else "main"

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
            if phase == "p2":
                return self.end()
            self.go("recover")
            self.next_after_recover = {"main": "g_start", "grow": "s_start"}.get(phase, "s_start")
            return 0.1

    def step_recover(self):
        return self.recover(self.next_after_recover)

    def recover(self, next_step):
        """Back to a plain viewport so the next phase starts from a known state."""
        if STATE.get("maximized"):
            self.toggle_maximize()
        _, area, _ = self.find_area()  # after the toggle: the maximised area's pointer is gone
        area.spaces.active.shading.type = "SOLID"
        self.go("recover_wait")
        self.next_after_recover = next_step
        return 1.5

    def step_recover_wait(self):
        self.go(self.next_after_recover)
        return 0.1

    def toggle_maximize(self):
        win, area, region = self.find_area()
        with bpy.context.temp_override(window=win, area=area, region=region):
            bpy.ops.screen.screen_full_area(use_hide_panels=True)
        STATE["maximized"] = not STATE.get("maximized", False)

    # ---- steps

    def step_setup(self):
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
        STATE["region"] = (region.width, region.height)
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
            self.job.events.append("frame log on")
        if not self.job.frames:
            if self.timed_out():
                raise RuntimeError(f"no frame presented; events {self.job.events}")
            return 0.1
        STATE["t_first_frame"] = self.job.frames[0]["t"]
        self.go("settle")
        return SETTLE_S

    def converged(self):
        import ctypes
        prog = self.job.abi.ar_progress()
        self.job.lib.ar_session_get_progress(self.job.session, ctypes.byref(prog))
        return prog.state == self.job.abi.AR_STATE_CONVERGED

    def step_settle(self):
        # Early passes of a progressive film are not normalised yet (corner pixels read 0.66..0.82 for the
        # world's 0.8), so the pre-edit frame is taken once the viewport's 64 samples are done.
        if not self.converged():
            if self.timed_out():
                raise RuntimeError("the first generation never converged")
            return 0.1
        self.go("settle_done")
        return 0.3

    def step_settle_done(self):
        self.pre = self.job.frames[-1]
        STATE["pre"] = self.pre
        STATE["events_before_edit"] = list(self.job.events)
        self.sphere = next(o for o in bpy.context.scene.objects if o.type == "MESH")
        self.sphere.location.x += 1.0
        self.sphere.location.z += 0.5
        self.go("after_move")
        return 0.1

    def newer(self, frame_gen):
        return [f for f in self.job.frames if f["generation"] > frame_gen]

    def step_after_move(self):
        got = self.newer(self.pre["generation"])
        if not got:
            if self.timed_out():
                raise RuntimeError(f"no frame of a newer generation after the move; events {self.job.events}")
            return 0.1
        self.go("after_move_settle")  # the main loop must keep running while that generation renders
        return SETTLE_S

    def step_after_move_settle(self):
        STATE["post"] = self.job.frames[-1]
        STATE["events_after_move"] = list(self.job.events)
        self.screenshot("viewport_edit.png")
        self.sphere.location.x -= 0.5  # a second plain move: the transform patch path
        self.go("after_second_move")
        return 0.1

    def step_after_second_move(self):
        if not self.newer(STATE["post"]["generation"]):
            if self.timed_out():
                raise RuntimeError(f"no frame after the second move; events {self.job.events}")
            return 0.1
        STATE["events_after_second_move"] = list(self.job.events)
        STATE["views_before"] = self.job.events.count("set_view")
        _, area, _ = self.find_area()
        self.view_id = self.job.frames[-1]["id"]
        area.spaces.active.region_3d.view_camera_zoom += 6.0  # a view change: no depsgraph update
        self.go("after_view")
        return 0.1

    def step_after_view(self):
        if not [f for f in self.job.frames if f["id"] > self.view_id] or \
                self.job.events.count("set_view") == STATE["views_before"]:
            if self.timed_out():
                raise RuntimeError(f"no frame after the view change; events {self.job.events}")
            return 0.1
        STATE["events_after_view"] = list(self.job.events)
        STATE["gens"] = [f["generation"] for f in self.job.frames]
        STATE["acquired"] = list(self.job.acquired)
        STATE["all_frames"] = list(self.job.frames)
        # Leaving Rendered: the spy keeps the session "not yet closed" for 4 polls of the destroying timer.
        self.spy = LibSpy(self.job.lib, self.job.abi)
        self.spy.hold[self.job.session.value] = 4
        STATE["main_session"], STATE["main_scene"] = self.job.session.value, self.job.scene_h.value
        self.job.lib = self.spy
        _, area, _ = self.find_area()
        area.spaces.active.shading.type = "SOLID"  # Blender frees the engine: __del__ closes the session
        self.go("closing")
        return 0.1

    def step_closing(self):
        if "destroy_scene" not in self.job.events:
            if self.timed_out():
                raise RuntimeError(f"the session was never destroyed; events {self.job.events}")
            return 0.1
        STATE["events_final"] = list(self.job.events)
        STATE["handles_final"] = (self.job.session.value, self.job.scene_h.value)
        STATE["close_native"] = self.spy.native()
        STATE["close_calls"] = list(self.spy.calls)
        self.go("g_start")
        return 0.5

    def step_g_start(self):
        """Rendered again on A1 with a coloured sphere, so that a swapped half pair in the unpack shows."""
        scene = bpy.context.scene
        sphere = next(o for o in scene.objects if o.type == "MESH")
        sphere.material_slots[0].material.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = \
            (0.9, 0.25, 0.05, 1.0)
        sphere.location = (0.0, 0.0, 0.0)
        _, area, region = self.find_area()
        area.spaces.active.shading.type = "RENDERED"
        STATE["grow_region_before"] = (region.width, region.height)
        self.go("g_first")
        return 0.2

    def step_g_first(self):
        job = self.find_job()
        if job is None:
            if self.timed_out():
                raise RuntimeError("no viewport job appeared (grow phase)")
            return 0.1
        self.job = job
        self.job.frames, self.job.acquired = [], []
        self.job.present.set_stripe_bytes(SMALL_STRIPE_ROWS * STATE["grow_region_before"][0] * 8)
        self.job.check_uploads = 3
        self.go("g_checks")
        return 0.2

    def step_g_checks(self):
        if len(self.job.upload_checks) < 3:
            if self.timed_out():
                raise RuntimeError(f"no read-back checks; events {self.job.events}")
            return 0.1
        STATE["upload_checks"] = list(self.job.upload_checks)
        # The region outgrows the session's film: maximise the area. The spy holds the old session "open".
        self.spy = LibSpy(self.job.lib, self.job.abi)
        old = self.job.session.value
        self.spy.hold[old] = 30
        self.job.lib = self.spy
        STATE["grow_old_session"] = old
        self.job.events.append("grow")
        self.toggle_maximize()
        STATE["t_grow"] = time.monotonic()
        self.job.frames = []
        self.go("g_after")
        return 0.1

    def step_g_after(self):
        _, _, region = self.find_area()
        size = (region.width, region.height)
        done = [c for c in self.spy.native(STATE["grow_old_session"]) if c[0] == "ar_session_destroy"]
        new_frames = [f for f in self.job.frames if f["size"] == size and size != STATE["grow_region_before"]]
        if not (done and new_frames):
            if self.timed_out():
                raise RuntimeError(f"no new frame / old destroy after growing to {size}; events {self.job.events}")
            return 0.1
        STATE["grow_size"] = size
        STATE["grow_frames"] = list(self.job.frames)
        STATE["grow_native"] = self.spy.native(STATE["grow_old_session"])
        STATE["grow_events"] = list(self.job.events)
        self.toggle_maximize()
        _, area, _ = self.find_area()
        area.spaces.active.shading.type = "SOLID"
        self.go("g_close")
        return 0.1

    def step_g_close(self):
        if "destroy_scene" not in self.job.events:
            if self.timed_out():
                raise RuntimeError(f"the grown job never destroyed its scene; events {self.job.events}")
            return 0.1
        self.todo = list(SCENARIOS)
        self.go("s_start")
        return 0.5

    def step_s_start(self):
        """Next teardown scenario (a fresh Rendered viewport, hence a fresh job), or on to the camera-less scene."""
        if not self.todo:
            self.go("cam_setup")
            return 0.5
        self.scn = self.todo.pop(0)
        if STATE.get("maximized"):
            self.toggle_maximize()
        _, area, region = self.find_area()
        area.spaces.active.shading.type = "RENDERED"
        self.go("s_frame")
        return 0.3

    def step_s_frame(self):
        job = self.find_job()
        if job is None or job.t_first_frame is None or job.t_first_frame <= STATE.get("s_after", 0):
            if self.timed_out():
                raise RuntimeError(f"scenario {self.scn}: no first frame")
            return 0.1
        self.job = job
        self.spy = LibSpy(job.lib, job.abi)
        job.lib = self.spy
        STATE["scn_session_" + self.scn] = job.session.value
        STATE["scn_scene_" + self.scn] = job.scene_h.value
        if self.scn == "overlap":
            self.spy.hold[job.session.value] = 40  # the first session stays "open" 2 s, the second closes at once
        if self.scn == "failcreate":
            self.spy.fail_create = True
        if self.scn == "failpark":
            def refuse(*a, **k):
                raise RuntimeError("timers refused")
            job.register_timer = refuse
            return self.s_leave()
        self.toggle_maximize()  # the region outgrows the session
        self.go("s_replaced")
        return 0.1

    def step_s_replaced(self):
        events = self.job.events
        if "replace_session" not in events or (self.scn == "failcreate" and "failed" not in events):
            if self.timed_out():
                raise RuntimeError(f"scenario {self.scn}: no replacement; events {events}")
            return 0.1
        return self.s_leave()

    def s_leave(self):
        if STATE.get("maximized"):
            self.toggle_maximize()
        _, area, _ = self.find_area()
        area.spaces.active.shading.type = "SOLID"  # __del__ -> park
        self.go("s_done")
        return 0.1

    def step_s_done(self):
        if "destroy_scene" not in self.job.events:
            if self.timed_out():
                raise RuntimeError(f"scenario {self.scn}: scene never destroyed; events {self.job.events}; parked {self.job.parked} session {self.job.session.value} retired {[r.value for r in self.job.retired]} reaper {self.job._reaper_on}")
            return 0.1
        STATE["scn_" + self.scn] = {"native": self.spy.native(), "events": list(self.job.events),
                                    "session": STATE["scn_session_" + self.scn],
                                    "scene": STATE["scn_scene_" + self.scn]}
        STATE["s_after"] = time.monotonic()
        self.go("s_start")
        return 0.5

    def step_cam_setup(self):
        """A scene without a camera, plus an area lamp and a textured plane, in a user (not camera) view."""
        import math
        from mathutils import Euler, Vector
        scene = bpy.context.scene
        cam = scene.camera
        scene.camera = None
        bpy.data.objects.remove(cam)
        light = bpy.data.objects.new("Lamp", bpy.data.lights.new("Lamp", "AREA"))
        light.data.energy, light.data.size = 800.0, 3.0
        light.location = (-1.0, -2.0, 3.0)
        light.rotation_euler = (math.radians(30), 0.0, math.radians(-20))
        scene.collection.objects.link(light)
        # A file-backed image: generated images wait for the engine's PIXELS support (images.py).
        gen = bpy.data.images.new("CheckerGen", 8, 8)
        px = np.ones((8, 8, 4), np.float32)
        px[:, :, :3] = (np.indices((8, 8)).sum(axis=0) % 2)[:, :, None] * 0.7 + 0.1
        gen.pixels.foreach_set(px.reshape(-1))
        path = os.path.join(ROOT, "out", "smoke", "checker.png")
        gen.filepath_raw, gen.file_format = path, "PNG"
        gen.save()
        img = bpy.data.images.load(path)
        mat = bpy.data.materials.new("PlaneMat")
        mat.use_nodes = True
        tex = mat.node_tree.nodes.new("ShaderNodeTexImage")
        tex.image, tex.interpolation = img, "Closest"
        mat.node_tree.links.new(tex.outputs["Color"], mat.node_tree.nodes["Principled BSDF"].inputs["Base Color"])
        me = bpy.data.meshes.new("Plane")
        me.from_pydata([(-3, -3, 0), (3, -3, 0), (3, 3, 0), (-3, 3, 0)], [], [(0, 1, 2, 3)])
        me.uv_layers.new(name="UVMap").data.foreach_set("uv", (0, 0, 4, 0, 4, 4, 0, 4))
        me.materials.append(mat)
        plane = bpy.data.objects.new("Plane", me)
        plane.location = (0.0, 0.0, -1.2)
        scene.collection.objects.link(plane)
        _, area, _ = self.find_area()
        rv3d = area.spaces.active.region_3d
        rv3d.view_perspective = "PERSP"
        rv3d.view_location = Vector((0.0, 0.0, -0.3))
        rv3d.view_distance = 9.0
        rv3d.view_rotation = Euler((math.radians(70), 0.0, math.radians(25))).to_quaternion()
        area.spaces.active.shading.type = "RENDERED"
        STATE["t_switch"] = time.monotonic()
        self.go("p2_first_frame")
        return 0.05

    def step_p2_first_frame(self):
        job = self.find_job()
        if job is None or job.t_first_frame is None:
            if self.timed_out():
                raise RuntimeError(f"no frame in the camera-less scene; events {job and job.events}")
            return 0.05
        self.job = job
        STATE["p2_seconds"] = job.t_first_frame - STATE["t_switch"]
        STATE["p2_events"] = list(job.events)
        job.frames = []
        self.go("p2_settle")
        return SETTLE_S + 1.0

    def step_p2_settle(self):
        STATE["p2_frame"] = self.job.frames[-1] if self.job.frames else None
        self.screenshot("viewport.png")
        return self.end()

    def screenshot(self, name):
        win, area, region = self.find_area()
        out = os.path.join(smoke_a1.arg("--evidence", os.path.join(ROOT, "out", "evidence", "085")), name)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with bpy.context.temp_override(window=win, area=area, region=region):
            bpy.ops.screen.screenshot_area(filepath=out)
        STATE.setdefault("screenshots", {})[name] = (out, (region.x - area.x, region.y - area.y, region.width, region.height))
        STATE["screenshot"] = out
        STATE["screenshot_region"] = (region.x - area.x, region.y - area.y, region.width, region.height)

    def end(self):
        STATE["seconds"] = time.monotonic() - T0
        self.rf.shutdown()  # a leftover job is closed here, in order; nothing to destroy if the run was clean
        run_cases()
        return None


PHASE_OF = {
    "test_gpu_unpack_matches_host": "grow", "test_region_growth_retires_session": "grow",
    "test_overlapping_retirements_end_with_the_scene": "scn:overlap",
    "test_failed_timer_registration_destroys_once": "scn:failpark",
    "test_failed_create_then_park_destroys_session_first": "scn:failcreate",
    "test_camera_less_scene_presents": "p2", "test_acceptance_screenshot": "p2",
}


class TestPresent(unittest.TestCase):
    def setUp(self):
        """A case needs its own phase to have completed; an earlier phase that failed does not matter to it
        except through the state it left (each phase starts from a plain viewport)."""
        if "watchdog" in STATE["errors"]:
            self.fail(STATE["errors"]["watchdog"])
        phase = PHASE_OF.get(self._testMethodName, "main")
        if phase in STATE["errors"]:
            self.fail(f"the {phase} phase of the viewport sequence did not complete:\n" + STATE["errors"][phase])

    def test_first_frame_presents(self):
        """Rendered shading presents a frame of the region's size whose pixels are A1's: the world's 0.8
        outside the sphere (exact: camera rays that miss), about rho * 0.8 = 0.4 on it."""
        f = STATE["pre"]
        print(f"RESULT present first frame size {f['size']} generation {f['generation']} region {STATE['region']}")
        self.assertEqual(f["size"], STATE["region"])
        rgb = f["rgb"]
        self.assertTrue(np.isfinite(rgb).all())
        corner = rgb[:3, :3].mean()
        print("RESULT present corner by frame " + " ".join(f"g{x['generation']}:{x['rgb'][:3, :3].mean():.4f}" for x in STATE["all_frames"][:8]))
        c = dark_centroid(rgb)
        sphere = rgb[rgb.mean(axis=2) < 0.6].mean()
        print(f"RESULT present corner {corner:.4f} (want 0.8), sphere {sphere:.4f} (want 0.4), pixels {c[2]}")
        self.assertAlmostEqual(corner, 0.8, delta=0.01)
        self.assertAlmostEqual(sphere, 0.4, delta=0.06)

    def test_generations_never_decrease(self):
        """d25: the presented generations are non-decreasing, and the edits raised them."""
        g = STATE["gens"]
        print(f"RESULT present generations {g[0]}..{g[-1]} over {len(g)} frames")
        self.assertEqual(g, sorted(g))
        self.assertGreater(g[-1], g[0])
        # Every frame the viewport acquired, shown or not: a frame older than one already shown was refused.
        ids = [a["id"] for a in STATE["acquired"]]
        self.assertEqual(ids, sorted(set(ids)), "a frame was acquired twice: acquire must pass the last frame seen")
        shown, rejected = -1, 0
        for a in STATE["acquired"]:
            if a["presented"]:
                self.assertGreaterEqual(a["generation"], shown, f"a stale frame was presented: {a}")
                shown = a["generation"]
            else:
                rejected += 1
                self.assertLess(a["generation"], shown, f"a current frame was refused: {a}")
        print(f"RESULT present acquired {len(STATE['acquired'])} frames, {rejected} refused as stale")

    def test_stale_frame_is_refused(self):
        """d25 directly: with generation 5 on screen, a frame of generation 4 is not wanted, 5 and 6 are. The
        engine offers no stale frame in a normal run, so the rule is fed one."""
        pres = importlib.import_module(MODULE + ".present")
        abi = importlib.import_module(MODULE + "._abi")
        p = pres.Present()
        p.generation = 5
        f = abi.ar_display_frame()
        for gen, want in ((4, False), (0, False), (5, True), (6, True)):
            f.generation = gen
            self.assertEqual(p.wants(f), want, f"generation {gen} against 5")

    def test_edit_changes_frame(self):
        """A scripted object move sends a delta (no full resync) and the next presented frame has a newer
        generation and a different image: the sphere moved right and up (rows grow upward in the frame)."""
        pre, post = STATE["pre"], STATE["post"]
        a, b = dark_centroid(pre["rgb"]), dark_centroid(post["rgb"])
        print(f"RESULT present edit: generation {pre['generation']} -> {post['generation']}, "
              f"sphere centre ({a[0]:.3f}, {a[1]:.3f}) -> ({b[0]:.3f}, {b[1]:.3f})")
        self.assertGreater(post["generation"], pre["generation"])
        self.assertGreater(b[0] - a[0], 0.1)
        self.assertGreater(b[1] - a[1], 0.03)
        before = STATE["events_before_edit"]
        added = STATE["events_after_move"][len(before):]
        second = STATE["events_after_second_move"][len(STATE["events_after_move"]):]
        print(f"RESULT present events: first move {added}, second move {second}")
        self.assertEqual(before.count("full"), 1)
        self.assertNotIn("full", added + second)
        self.assertTrue(any(e.startswith("delta") and "instances" in e for e in added))
        self.assertTrue(any(e.startswith("delta") and "instances" in e for e in second))

    def test_view_change_sets_view(self):
        """set_view goes out when the view hash changed and not otherwise (d49): once at the start and
        once for the zoom, with no depsgraph update in between."""
        n = STATE["events_after_view"].count("set_view")
        print(f"RESULT present set_view calls {n} over the whole run")
        self.assertEqual(STATE["views_before"], 1)
        self.assertEqual(n, 2)

    def test_screenshot_shows_the_frame(self):
        """What reached the screen: the area shows the moved sphere right of the centre and above it
        (the PNG is read back bottom-up by Blender, like the frame)."""
        reg, w, h = self.region_pixels("viewport_edit.png")
        lum = reg.mean(axis=2)
        bg = np.median(lum[:8, :8])
        mask = lum < bg - 0.05
        ys, xs = np.nonzero(mask)
        cx, cy = xs.mean() / w, ys.mean() / h
        print(f"RESULT present screenshot region {w}x{h}: background {bg:.3f}, {mask.sum()} sphere pixels, "
              f"centre ({cx:.3f}, {cy:.3f})")
        self.assertGreater(mask.sum(), 500)
        self.assertGreater(cx, 0.58)
        self.assertGreater(cy, 0.52)

    @staticmethod
    def region_pixels(name):
        path, (x0, y0, w, h) = STATE["screenshots"][name]
        img = bpy.data.images.load(path)
        px = np.array(img.pixels[:], np.float32).reshape(img.size[1], img.size[0], 4)[:, :, :3]
        return px[y0:y0 + h, x0:x0 + w], w, h

    def test_camera_less_scene_presents(self):
        """A scene without a camera, in a user view, with a lamp and a textured plane, presents a frame of
        the region's size that is not black, with no failure."""
        f = STATE["p2_frame"] or {}
        ev = STATE["p2_events"]
        print(f"RESULT present camera-less: first frame {STATE['p2_seconds'] * 1000:.0f} ms after switching to "
              f"Rendered, events {ev}")
        self.assertNotIn("failed", ev)
        self.assertTrue(STATE["p2_frame"], "no frame was presented")
        rgb = f["rgb"]
        self.assertTrue(np.isfinite(rgb).all())
        self.assertGreater(float(rgb.mean()), 0.01)
        self.assertLess(STATE["p2_seconds"], 5.0)

    def test_acceptance_screenshot(self):
        """The acceptance look: the lamp-lit textured plane on the screen, measured."""
        reg, w, h = self.region_pixels("viewport.png")
        lum = reg.mean(axis=2)
        print(f"RESULT present acceptance screenshot {w}x{h}: luminance min {lum.min():.3f} mean {lum.mean():.3f} "
              f"max {lum.max():.3f}, std {lum.std():.3f}, pixels above 0.02: {int((lum > 0.02).sum())}")
        # A patch on the plane's near half (fractions of the region, rows bottom-up): the checker image shows
        # as dark and light cells (checker 0.1 and 0.8 in the file, lit by the lamp), not the magenta of an
        # image the engine does not have.
        patch = lum[int(h * 0.226):int(h * 0.329), int(w * 0.325):int(w * 0.425)]
        lo, hi = np.percentile(patch, [10, 90])
        print(f"RESULT present checker patch: 10% {lo:.3f}, 90% {hi:.3f}, std {patch.std():.3f}")
        self.assertGreater(float(hi - lo), 0.3)
        colour = reg[int(h * 0.226):int(h * 0.329), int(w * 0.325):int(w * 0.425)]
        self.assertLess(float(np.abs(colour[:, :, 0] - colour[:, :, 1]).mean()), 0.05)  # grey, not magenta

    def test_padded_frame_is_refused(self):
        """The unpack shader indexes rows by width, so a frame with padded rows or another format is refused
        with a clear message rather than shown sheared."""
        pres = importlib.import_module(MODULE + ".present")
        abi = importlib.import_module(MODULE + "._abi")
        f = abi.ar_display_frame()
        f.width, f.height, f.format, f.row_bytes = 32, 4, 0, 32 * 8
        pres.check_layout(f)
        f.row_bytes = 32 * 8 + 16
        with self.assertRaisesRegex(ValueError, "row_bytes .* not width\*8"):
            pres.check_layout(f)
        f.row_bytes, f.format = 32 * 8, 1
        with self.assertRaisesRegex(ValueError, "not RGBA16F"):
            pres.check_layout(f)

    def test_close_destroys(self):
        """Leaving Rendered: the native calls, as the spy saw them, are ar_session_close, ar_session_destroy,
        ar_scene_destroy in that order, on the job's own handles."""
        names = [(c[0], c[1]) for c in STATE["close_native"]]
        print(f"RESULT present close native calls {names}")
        s, sc = STATE["main_session"], STATE["main_scene"]
        self.assertEqual(names, [("ar_session_close", s), ("ar_session_destroy", s), ("ar_scene_destroy", sc)])
        self.assertEqual(STATE["handles_final"], (None, None))

    def test_close_waits_for_closed(self):
        """The destroy waits for CLOSED: the spy kept the closed session reporting RENDERING for 4 polls, and
        the destroy came after those polls, when the last state the job saw was CLOSED."""
        abi_closed = importlib.import_module(MODULE + "._abi").AR_STATE_CLOSED
        s = STATE["main_session"]
        calls = STATE["close_calls"]
        i = next(k for k, c in enumerate(calls) if c[0] == "ar_session_destroy" and c[1] == s)
        before = [c[2] for c in calls[:i] if c[0] == "progress" and c[1] == s]
        print(f"RESULT present close: states seen before the destroy {before}, state at the destroy {calls[i][2]}")
        self.assertEqual(calls[i][2], abi_closed)
        self.assertEqual(sum(1 for st in before if st != abi_closed), 4)

    def test_gpu_unpack_matches_host(self):
        """The persistent texture read back from the GPU equals the host frame, bit for bit in half precision,
        on a coloured scene (a swapped half pair would swap red/green with blue/alpha) with the stripe size
        forced to 100 rows, so several stripes and row offsets are exercised."""
        checks = STATE["upload_checks"]
        for c in checks:
            print(f"RESULT present gpu unpack {c['size']} in {c['stripes']} stripes: max diff {c['max_diff']:.3g}, "
                  f"largest channel spread in the frame {c['colour_spread']:.3f}")
            self.assertGreater(c["stripes"], 5)
            self.assertEqual(c["max_diff"], 0.0)
            self.assertGreater(c["colour_spread"], 0.3)  # coloured pixels exist, so a channel mix-up cannot hide

    def test_region_growth_retires_session(self):
        """A region that outgrows the session's film gets a new session and a frame of the new size, and the
        old session is closed at once and destroyed by the timer only after it reported CLOSED, while frames
        kept being presented: the draw never waited for it."""
        abi_closed = importlib.import_module(MODULE + "._abi").AR_STATE_CLOSED
        old = STATE["grow_old_session"]
        native_calls = STATE["grow_native"]
        names = [c[0] for c in native_calls]
        print(f"RESULT present growth {STATE['grow_region_before']} -> {STATE['grow_size']}: old session calls {names}, "
              f"events {[e for e in STATE['grow_events'] if e != 'frame log on']}")
        self.assertEqual(names, ["ar_session_close", "ar_session_destroy"])
        self.assertEqual(native_calls[1][2], abi_closed)
        self.assertIn("replace_session", STATE["grow_events"])
        self.assertIn("destroy_session via timer", STATE["grow_events"])
        t_close, t_destroy = native_calls[0][3], native_calls[1][3]
        between = [f for f in STATE["grow_frames"] if t_close < f["t"] < t_destroy]
        print(f"RESULT present growth: destroy {1000 * (t_destroy - t_close):.0f} ms after the close, "
              f"{len(between)} frames presented in between")
        self.assertGreater(t_destroy - t_close, 1.0)  # 30 held polls of 50 ms
        self.assertGreater(len(between), 0)

    def native_names(self, scn):
        return [(c[0], c[1]) for c in STATE["scn_" + scn]["native"]]

    def test_overlapping_retirements_end_with_the_scene(self):
        """The region outgrew the session (the first is retired, held open for 2 s) and the viewport was left
        at once: the second session closes first. Both sessions are destroyed, the first after the second, and
        only then the scene, which the reaper still destroys."""
        r = STATE["scn_overlap"]
        names = self.native_names("overlap")
        print(f"RESULT present overlap native calls {[(n, 's0' if h == r['session'] else 'scene' if h == r['scene'] else 's1') for n, h in names]}")
        s0, scene = r["session"], r["scene"]
        s1 = next(h for n, h in names if n == "ar_session_close" and h != s0)
        self.assertEqual(names, [("ar_session_close", s0), ("ar_session_close", s1), ("ar_session_destroy", s1),
                                 ("ar_session_destroy", s0), ("ar_scene_destroy", scene)])

    def test_failed_timer_registration_destroys_once(self):
        """When the reaper cannot be registered at park, the job closes directly: each handle is closed and
        destroyed exactly once, the session before the scene."""
        r = STATE["scn_failpark"]
        names = self.native_names("failpark")
        print(f"RESULT present failed registration native calls {names}")
        s, scene = r["session"], r["scene"]
        self.assertEqual(names, [("ar_session_close", s), ("ar_session_destroy", s), ("ar_scene_destroy", scene)])

    def test_failed_create_then_park_destroys_session_first(self):
        """A resize whose ar_session_create fails leaves the old session retired and none current; parking then
        destroys that session, and only after it the scene."""
        r = STATE["scn_failcreate"]
        names = self.native_names("failcreate")
        print(f"RESULT present failed create native calls {names}, events {r['events']}")
        s, scene = r["session"], r["scene"]
        self.assertEqual(names, [("ar_session_close", s), ("ar_session_destroy", s), ("ar_scene_destroy", scene)])


def run_cases():
    loader = unittest.defaultTestLoader
    if "-k" in ARGS:  # land's canary runs one named case
        pat = ARGS[ARGS.index("-k") + 1]
        loader.testNamePatterns = [pat if "*" in pat else f"*{pat}*"]
    res = unittest.TextTestRunner(stream=sys.stdout, verbosity=2).run(loader.loadTestsFromTestCase(TestPresent))
    json.dump({k: v for k, v in STATE.items() if k in ("region", "screenshot", "seconds")},
              open(os.path.join(ROOT, "out", "evidence", "085", "run.json"), "w"), indent=1)
    finish(0 if res.wasSuccessful() else 1)


def main():
    work = smoke_a1.arg("--work", os.path.join(ROOT, "out", "smoke"))
    os.makedirs(work, exist_ok=True)
    os.makedirs(os.path.join(ROOT, "out", "evidence", "085"), exist_ok=True)
    blend = os.path.join(work, "a1_viewport.blend")
    p = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "scenes", "a1_furnace", "build.py"),
                        "--out", os.path.join(work, "a1_viewport.arrec"), "--blend", blend, "--sampler", "sobol"],
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
    scene.astroray.viewport_samples = 64
    scene.camera.data.passepartout_alpha = 0.0  # the whole region shows the render, undimmed
    driver = Driver(rf)
    bpy.app.timers.register(driver.tick, first_interval=1.0)


if __name__ == "__main__":
    try:
        main()
    except Exception:
        STATE["errors"]["main"] = traceback.format_exc()
        print(STATE["errors"]["main"], flush=True)
        run_cases()
