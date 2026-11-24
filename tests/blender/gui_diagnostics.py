# SPDX-License-Identifier: Apache-2.0
"""The Diagnostics panel follows a Rendered viewport: an edit that adds an unsupported node changes the listed
count without the user restarting anything.

Run through `Invoke-ArBlender -Gui -Script tests/blender/gui_diagnostics.py`. A windowed Blender goes to Rendered
shading on the A1 scene with a Properties editor showing the Render tab. A plane whose material has a Wave
Texture linked into the shader (an unsupported node) is then added. The viewport's report revision changes,
view_draw tags the Properties editors for redraw, and the panel's draw must run again and see the new entry:
the script never tags a redraw itself. A screenshot of the Properties editor is saved to
out/evidence/199/diagnostics_panel.png.
A timer drives the sequence (the viewport only draws while the main loop runs); the script ends the process.
Ref: blender/astroray/diagnostics.py, ui/panels.py; tests/blender/gui_ortho.py (the driving pattern).
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
EVIDENCE = os.path.join(ROOT, "out", "evidence", "199")
WATCHDOG_S = 200.0
STEP_TIMEOUT_S = 40.0
T0 = time.monotonic()
STATE = {"errors": {}, "draws": [], "before": None, "after": None, "shot": None}


def finish(code):
    print(f"gui_diagnostics exit {code}", flush=True)
    os._exit(code)  # skips Blender's window teardown


def run_cases():
    res = unittest.TextTestRunner(stream=sys.stdout, verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(TestDiagnosticsPanel))
    finish(0 if res.wasSuccessful() else 1)


def unsupported(report):
    return [e for e in report.entries if e.code == "NODE_UNSUPPORTED"] if report else []


class Driver:
    def __init__(self):
        self.step_name, self.step_t0 = "setup", time.monotonic()
        self.diag = importlib.import_module(MODULE + ".diagnostics")

    def go(self, name):
        self.step_name, self.step_t0 = name, time.monotonic()

    def timed_out(self):
        return time.monotonic() - self.step_t0 > STEP_TIMEOUT_S

    def area(self, kind):
        for win in bpy.context.window_manager.windows:
            for area in win.screen.areas:
                if area.type == kind:
                    return win, area
        raise RuntimeError(f"no {kind} area")

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
        panels = importlib.import_module(MODULE + ".ui.panels")
        orig, diag = panels.ASTRORAY_PT_diagnostics.draw, self.diag

        def recording_draw(panel, context):
            report = diag.latest()
            STATE["draws"].append(len(unsupported(report)))
            return orig(panel, context)

        panels.ASTRORAY_PT_diagnostics.draw = recording_draw
        _, props = self.area("PROPERTIES")
        props.spaces.active.context = "RENDER"
        _, view = self.area("VIEW_3D")
        space = view.spaces.active
        space.shading.type = "RENDERED"
        space.overlay.show_overlays = False
        self.go("first")
        return 0.5

    def step_first(self):
        report = self.diag.latest()
        if report is not None and report.kind == "VIEWPORT" and STATE["draws"]:
            STATE["before"] = (len(unsupported(report)), len(STATE["draws"]))
            self.add_magenta_plane()
            self.go("edited")
            return 0.2
        if self.timed_out():
            return run_cases()
        return 0.2

    def add_magenta_plane(self):
        scene = bpy.context.scene
        me = bpy.data.meshes.new("DiagPlane")
        me.from_pydata([(-1, -1, 3), (1, -1, 3), (1, 1, 3), (-1, 1, 3)], [], [(0, 1, 2, 3)])
        ob = bpy.data.objects.new("DiagPlane", me)
        scene.collection.objects.link(ob)
        mat = bpy.data.materials.new("Magenta")
        mat.use_nodes = True
        ob.data.materials.append(mat)
        wave = mat.node_tree.nodes.new("ShaderNodeTexWave")
        mat.node_tree.links.new(wave.outputs[0], mat.node_tree.nodes["Principled BSDF"].inputs["Base Color"])
        bpy.context.view_layer.update()

    def step_edited(self):
        report = self.diag.latest()
        seen = unsupported(report)
        # The draw after the entry arrived: the redraw tag came from view_draw, not from this script.
        if seen and STATE["draws"] and STATE["draws"][-1] == len(seen):
            STATE["after"] = (len(seen), len(STATE["draws"]), seen[0].subject, seen[0].detail)
            os.makedirs(EVIDENCE, exist_ok=True)
            win, props = self.area("PROPERTIES")
            region = next(r for r in props.regions if r.type == "WINDOW")
            with bpy.context.temp_override(window=win, area=props, region=region):
                bpy.ops.screen.screenshot_area(filepath=os.path.join(EVIDENCE, "diagnostics_panel.png"))
            STATE["shot"] = os.path.join(EVIDENCE, "diagnostics_panel.png")
            return run_cases()
        if self.timed_out():
            return run_cases()
        return 0.2


class TestDiagnosticsPanel(unittest.TestCase):
    def test_viewport_edit_changes_the_listed_count(self):
        if "run" in STATE["errors"]:
            self.fail(STATE["errors"]["run"])
        self.assertIsNotNone(STATE["before"], "the viewport never reported or the panel never drew")
        self.assertIsNotNone(STATE["after"], f"the panel did not redraw with the new entry; draws {STATE['draws']}")
        self.assertEqual(STATE["before"][0], 0)
        self.assertEqual(STATE["after"][0], 1)
        self.assertGreater(STATE["after"][1], STATE["before"][1])  # a redraw happened after the edit
        self.assertIn("Magenta", STATE["after"][2])
        self.assertIn("ShaderNodeTexWave", STATE["after"][3])
        self.assertTrue(os.path.getsize(STATE["shot"]) > 1000)


def main():
    work = smoke_a1.arg("--work", os.path.join(ROOT, "out", "smoke"))
    os.makedirs(work, exist_ok=True)
    blend = os.path.join(work, "a1_diagnostics.blend")
    p = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "scenes", "a1_furnace", "build.py"),
                        "--out", os.path.join(work, "a1_diagnostics.arrec"), "--blend", blend, "--sampler", "sobol"],
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
