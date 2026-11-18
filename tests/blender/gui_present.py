# SPDX-License-Identifier: Apache-2.0
"""The viewport present, in a windowed Blender (task 085): A1 in Rendered shading.

Run through `Invoke-ArBlender -Gui -Script tests/blender/gui_present.py` (the land canary adds `-k <case>`).
Everything is driven by timers (the viewport only draws while Blender's main loop runs) and the script ends
the process itself, with exit code 1 on any failed case. One run walks the whole sequence, then the cases
read what it recorded:
  switch to Rendered -> first frame -> move the sphere (twice) -> change the view -> leave Rendered.
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
WATCHDOG_S = 240.0
SETTLE_S = 2.0       # let the first generation render a while before the sphere moves
STEP_TIMEOUT_S = 90.0
T0 = time.monotonic()
STATE = {"error": None}


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

    def tick(self):
        try:
            if time.monotonic() - T0 > WATCHDOG_S:
                raise RuntimeError(f"watchdog: still in step {self.step_name}")
            return getattr(self, "step_" + self.step_name)()
        except Exception:
            STATE["error"] = traceback.format_exc()
            print(STATE["error"], flush=True)
            return self.end()

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
        STATE["all_frames"] = list(self.job.frames)
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
        self.go("cam_setup")
        return 0.1

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


class TestPresent(unittest.TestCase):
    def setUp(self):
        if STATE["error"]:
            self.fail("the viewport sequence did not complete:\n" + STATE["error"])

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

    def test_close_destroys(self):
        """Leaving Rendered closes the session at once and the timer destroys session, then scene, once
        CLOSED is reported."""
        ev = STATE["events_final"]
        print(f"RESULT present lifecycle {ev}")
        tail = [e for e in ev if e in ("session_close", "closed_seen", "destroy_session", "destroy_scene")]
        self.assertEqual(tail, ["session_close", "closed_seen", "destroy_session", "destroy_scene"])
        self.assertEqual(STATE["handles_final"], (None, None))


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
        STATE["error"] = traceback.format_exc()
        print(STATE["error"], flush=True)
        run_cases()
