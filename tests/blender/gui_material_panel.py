# SPDX-License-Identifier: Apache-2.0
"""Screenshot of the Material tab's "Astroray Extension" panel with the kind set to Diffraction Grating.

Run through `Invoke-ArBlender -Gui -Script tests/blender/gui_material_panel.py`. The extension is installed from this
tree's zip as tests/blender/smoke_a1.py does, a plane gets a material, the Properties editor shows the Material
tab maximised, and the Properties editor's window region is saved to out/evidence/187/material_panel.png. A timer
drives it (a windowed Blender only redraws while the main loop runs); the script ends the process.
Ref: tests/blender/gui_diagnostics.py (the driving pattern and the screenshot call).
"""
import os
import sys
import time
import traceback

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import smoke_a1  # noqa: E402  (the zip install)

SHOT = os.path.join(ROOT, "out", "evidence", "187", "material_panel.png")
T0 = time.monotonic()
STATE = {"step": "setup", "t": time.monotonic()}


def finish(code):
    print(f"gui_material_panel exit {code}", flush=True)
    os._exit(code)  # skips Blender's window teardown


def properties_area():
    for win in bpy.context.window_manager.windows:
        for area in win.screen.areas:
            if area.type == "PROPERTIES":
                return win, area
    raise RuntimeError("no PROPERTIES area")


def tick():
    try:
        if time.monotonic() - T0 > 120:
            raise RuntimeError(f"watchdog in step {STATE['step']}")
        win, area = properties_area()
        region = next(r for r in area.regions if r.type == "WINDOW")
        if STATE["step"] == "setup":
            area.spaces.active.context = "MATERIAL"
            with bpy.context.temp_override(window=win, area=area, region=region):
                bpy.ops.screen.screen_full_area()
            STATE["step"], STATE["t"] = "shot", time.monotonic()
            return 1.0
        if time.monotonic() - STATE["t"] < 2.0:
            for a in bpy.context.window.screen.areas:
                a.tag_redraw()
            return 0.5
        win, area = properties_area()
        region = next(r for r in area.regions if r.type == "WINDOW")
        os.makedirs(os.path.dirname(SHOT), exist_ok=True)
        with bpy.context.temp_override(window=win, area=area, region=region):
            bpy.ops.screen.screenshot_area(filepath=SHOT)
        print(f"region {region.width}x{region.height}, saved {SHOT}", flush=True)
        finish(0 if os.path.getsize(SHOT) > 1000 else 1)
    except Exception:
        print(traceback.format_exc(), flush=True)
        finish(1)


def main():
    smoke_a1.install_and_enable()
    scene = bpy.context.scene
    scene.render.engine = "ASTRORAY"
    me = bpy.data.meshes.new("Plane")
    me.from_pydata([(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)], [], [(0, 1, 2, 3)])
    ob = bpy.data.objects.new("Plane", me)
    scene.collection.objects.link(ob)
    mat = bpy.data.materials.new("Grating")
    ob.data.materials.append(mat)
    bpy.context.view_layer.objects.active = ob
    ob.select_set(True)
    mat.astroray.ext_kind = "GRATING"
    bpy.app.timers.register(tick, first_interval=1.0)


if __name__ == "__main__":
    try:
        main()
    except Exception:
        print(traceback.format_exc(), flush=True)
        finish(1)
