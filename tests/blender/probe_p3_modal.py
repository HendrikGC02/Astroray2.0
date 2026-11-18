# SPDX-License-Identifier: Apache-2.0
"""Probe P3, object mode (task 097): `context.window.modal_operators` inside `view_update` during a modal transform.

Run through `Invoke-ArBlender -Gui -Script tests/blender/probe_p3_modal.py`. A UV sphere with a Wave modifier
(deform-only, so its evaluated mesh keeps its topology while its vertices move) is in a Rendered viewport.
The script starts the `G` operator (`transform.translate`, invoked, so it runs modally) and, while it runs,
changes the modifier every 50 ms so the depsgraph reports a geometry update on the object (event_simulate
needs a Blender flag the launcher does not pass, so no mouse events are sent). The probe wraps
`ViewportJob.view_update` and records, per call: the window's modal operator ids, the geometry updates in the
batch, and the evaluated mesh's counts and triangle index array against those of the first read. Cases:
  - the modal operator is listed inside view_update while the transform runs, and none before it started
  - on the geometry updates seen during the modal operator the counts and the triangle indices equal the
    first read's (so a positions-only update stays valid), and topology_cache.decide picks POSITIONS for the
    ids Blender reports (the spelling of the allow-list is the one Blender uses).
Edit mode is task M3's. The cases only record what Blender does; none of them can be satisfied by the addon.
Ref: docs/decisions/0007-blender-integration.md d22 (b) and Risks P3.
"""
import importlib
import json
import math
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
import gui_present  # noqa: E402
import smoke_a1  # noqa: E402

MODULE = smoke_a1.MODULE
ARGS = smoke_a1.ARGS
WATCHDOG_S = 200.0
STEP_TIMEOUT_S = 40.0
MODAL_S = 3.0
T0 = time.monotonic()
STATE = {"errors": {}, "calls": [], "first": None, "modal_started": None}
EVIDENCE = os.path.join(ROOT, "out", "evidence", "097")
OBJECT = "P3Sphere"


def finish(code):
    print(f"probe_p3_modal exit {code}", flush=True)
    sys.stdout.flush()
    os._exit(code)


def triangles(me):
    n = len(me.loop_triangles)
    tri = np.empty(n * 3, np.int32)
    me.loop_triangles.foreach_get("vertices", tri)
    return len(me.vertices), len(me.loops), len(me.polygons), n, tri


def instrument(viewport_mod):
    cls = viewport_mod.ViewportJob
    orig = cls.view_update
    mesh_mod = importlib.import_module(MODULE + ".sync.mesh")

    def view_update(self, engine, context, depsgraph):
        rec = {"t": time.monotonic() - T0, "window": context.window is not None,
               "modal": [op.bl_idname for op in context.window.modal_operators] if context.window else None,
               "geometry": [u.id.name for u in depsgraph.updates if u.is_updated_geometry],
               "transform": [u.id.name for u in depsgraph.updates if u.is_updated_transform]}
        ob = bpy.data.objects.get(OBJECT)
        if ob is not None and (rec["geometry"] or STATE["first"] is None):
            with mesh_mod.mesh_of(depsgraph.objects[OBJECT]) as me:
                v, c, f, n, tri = triangles(me)
            if STATE["first"] is None:
                STATE["first"] = (v, c, f, n, tri)
            fv, fc, ff, fn, ftri = STATE["first"]
            rec["counts"] = (v, c, f, n)
            rec["counts_equal"] = (v, c, f, n) == (fv, fc, ff, fn)
            rec["triangles_equal"] = bool(len(tri) == len(ftri) and np.array_equal(tri, ftri))
        STATE["calls"].append(rec)
        return orig(self, engine, context, depsgraph)
    cls.view_update = view_update


class Driver(gui_present.Driver):
    def phase(self):
        return "modal"

    def tick(self):
        try:
            if time.monotonic() - T0 > WATCHDOG_S:
                STATE["errors"]["watchdog"] = f"watchdog: still in step {self.step_name}"
                print(STATE["errors"]["watchdog"], flush=True)
                return self.end()
            return getattr(self, "step_" + self.step_name)()
        except Exception:
            STATE["errors"]["modal"] = traceback.format_exc()
            print(f"phase modal failed:\n{STATE['errors']['modal']}", flush=True)
            return self.end()

    def timed_out(self, limit=STEP_TIMEOUT_S):
        return super().timed_out(limit)

    def end(self):
        self.rf.shutdown()
        run_cases()
        return None

    def step_setup(self):
        win, area, region = self.find_area()
        space = area.spaces.active
        space.shading.type = "RENDERED"
        space.region_3d.view_perspective = "CAMERA"
        self.go("first_frame")
        return 0.3

    def step_first_frame(self):
        self.job = self.job or self.find_job_any()
        if self.job is None or self.job.t_first_frame is None:
            if self.timed_out():
                raise RuntimeError("no first frame")
            self.find_area()[1].tag_redraw()
            return 0.2
        STATE["before"] = len(STATE["calls"])
        self.go("rest")
        return 1.0

    def find_job_any(self):
        held = self.rf._contexts.get("CPU")
        vp = importlib.import_module(MODULE + ".viewport")
        for j in (held.jobs if held else ()):
            if isinstance(j, vp.ViewportJob):
                return j
        return None

    def step_rest(self):
        """Quiet period: wiggle the modifier once with no modal operator (a baseline update), then start G."""
        bpy.data.objects[OBJECT].modifiers["Wave"].height = 0.3
        self.go("start_modal")
        return 0.5

    def step_start_modal(self):
        win, area, region = self.find_area()
        ob = bpy.data.objects[OBJECT]
        for o in bpy.data.objects:
            o.select_set(o is ob)
        bpy.context.view_layer.objects.active = ob
        STATE["modal_started"] = time.monotonic() - T0
        with bpy.context.temp_override(window=win, area=area, region=region):
            r = bpy.ops.transform.translate("INVOKE_DEFAULT")
        STATE["invoke_result"] = sorted(r)
        print(f"transform.translate INVOKE_DEFAULT -> {r}; modal operators now "
              f"{[op.bl_idname for op in win.modal_operators]}", flush=True)
        self.go("modal")
        self.n = 0
        return 0.05

    def step_modal(self):
        win, area, _ = self.find_area()
        self.n += 1
        ob = bpy.data.objects[OBJECT]
        ob.modifiers["Wave"].height = 0.3 + 0.1 * math.sin(self.n)  # a geometry update, same topology
        area.tag_redraw()
        STATE["modal_listed"] = [op.bl_idname for op in win.modal_operators]
        if time.monotonic() - self.step_t0 > MODAL_S:
            STATE["modal_ended"] = time.monotonic() - T0
            return self.end()
        return 0.05


class TestModal(unittest.TestCase):
    def setUp(self):
        if "watchdog" in STATE["errors"]:
            self.fail(STATE["errors"]["watchdog"])
        if STATE["errors"]:
            self.fail("the probe did not complete:\n" + "\n".join(STATE["errors"].values()))

    def during(self):
        t0 = STATE["modal_started"]
        return [c for c in STATE["calls"] if c["t"] >= t0]

    def test_modal_operators_listed_in_view_update(self):
        """P3 / d22 (b): while the transform runs, window.modal_operators names it inside view_update, and it
        is empty in the updates before the operator started."""
        during, before = self.during(), [c for c in STATE["calls"] if c["t"] < STATE["modal_started"]]
        ids = sorted({i for c in during for i in (c["modal"] or [])})
        print(f"PROBE P3 modal: invoke returned {STATE['invoke_result']}, window.modal_operators from the timer "
              f"{STATE['modal_listed']}; view_update calls before {len(before)}, during {len(during)}, "
              f"with the operator listed {sum(1 for c in during if c['modal'])}, ids {ids}")
        self.assertEqual(STATE["invoke_result"], ["RUNNING_MODAL"])
        self.assertGreater(len(during), 0, "no view_update ran while the modal operator was active")
        self.assertTrue(any(c["modal"] for c in during), "modal_operators was empty in every view_update during G")
        self.assertFalse(any(c["modal"] for c in before), "modal_operators was populated before G started")

    def test_positions_only_update_keeps_triangles(self):
        """P3: a geometry update during the transform leaves counts and loop_triangles' indices equal to the
        first read's, and the d22 decision function picks the positions path for the ids Blender reports."""
        tc = importlib.import_module(MODULE + ".sync.topology_cache")
        geo = [c for c in self.during() if "counts" in c and c["modal"]]
        ids = sorted({i for c in geo for i in c["modal"]})
        modes = {tuple(c["modal"]): tc.decide(True, False, True, modal_ids=c["modal"]) for c in geo}
        print(f"PROBE P3 triangles: {len(geo)} geometry updates under a modal operator; counts {geo[0]['counts'] if geo else None}, "
              f"equal to the first read in {sum(c['counts_equal'] for c in geo)}, triangle indices equal in "
              f"{sum(c['triangles_equal'] for c in geo)}; ids {ids}; decide() -> {modes}")
        self.assertGreater(len(geo), 0, "no geometry update arrived under a modal operator")
        self.assertTrue(all(c["counts_equal"] and c["triangles_equal"] for c in geo))
        self.assertTrue(all(m == tc.POSITIONS for m in modes.values()), f"decide() did not choose POSITIONS: {modes}")


def run_cases():
    loader = unittest.defaultTestLoader
    if "-k" in ARGS:
        pat = ARGS[ARGS.index("-k") + 1]
        loader.testNamePatterns = [pat if "*" in pat else f"*{pat}*"]
    res = unittest.TextTestRunner(stream=sys.stdout, verbosity=2).run(loader.loadTestsFromTestCase(TestModal))
    slim = [{k: v for k, v in c.items()} for c in STATE["calls"]]
    json.dump(slim, open(os.path.join(EVIDENCE, "p3_calls.json"), "w"), indent=1, default=str)
    finish(0 if res.wasSuccessful() else 1)


def main():
    work = smoke_a1.arg("--work", os.path.join(ROOT, "out", "smoke"))
    os.makedirs(work, exist_ok=True)
    os.makedirs(EVIDENCE, exist_ok=True)
    blend = os.path.join(work, "a1_p3.blend")
    p = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "scenes", "a1_furnace", "build.py"),
                        "--out", os.path.join(work, "a1_p3.arrec"), "--blend", blend, "--sampler", "sobol"],
                       capture_output=True, text=True, timeout=300)
    if p.returncode != 0:
        raise RuntimeError("scenes/a1_furnace/build.py failed: " + p.stdout + p.stderr)
    smoke_a1.install_and_enable()
    rf = importlib.import_module(MODULE + ".render_final")
    instrument(importlib.import_module(MODULE + ".viewport"))
    blend_writer.build(blend + ".json", blend)
    bpy.ops.wm.open_mainfile(filepath=blend, load_ui=False)
    scene = bpy.context.scene
    scene.render.engine = "ASTRORAY"
    scene.astroray.device = "CPU"
    scene.astroray.viewport_samples = 1024
    scene.camera.data.passepartout_alpha = 0.0
    # The sphere under test, off to the side of the furnace's own geometry.
    import bmesh
    me = bpy.data.meshes.new(OBJECT)
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=48, v_segments=24, radius=0.3)
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new(OBJECT, me)
    scene.collection.objects.link(ob)
    mod = ob.modifiers.new("Wave", "WAVE")
    mod.height = 0.1
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
