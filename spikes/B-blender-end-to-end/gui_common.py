"""Shared plumbing for the GUI spikes: isolated windowed Blender driven from a --python script.
Registers a RenderEngine whose view_update/view_draw delegate to a scenario object, switches the first 3D
viewport to RENDERED, optionally maximises it, and quits Blender by itself when the scenario is done."""
import json
import os
import sys
import time
import traceback

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

OUT = {"errors": []}
OUT_PATH = None
SCENARIO = None          # object with .view_update(engine, ctx, depg), .view_draw(engine, ctx, depg), .done
WATCHDOG_S = 240.0
_t_start = time.perf_counter()


def parse_args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    kv = {}
    for a in argv:
        if "=" in a:
            k, v = a.split("=", 1)
            kv[k] = v
        else:
            kv[a] = "1"
    return kv


def save():
    OUT["wall_s"] = time.perf_counter() - _t_start
    with open(OUT_PATH, "w") as f:
        json.dump(OUT, f, indent=1, default=str)


def quit_now(reason):
    OUT["quit_reason"] = reason
    try:
        save()
    finally:
        print("SPIKE_DONE", reason, flush=True)
        os._exit(0)  # hard exit of THIS process: avoids waiting on the GPU context / modal UI teardown


class Engine(bpy.types.RenderEngine):
    bl_idname = "SB_ENGINE"
    bl_label = "SB Spike Engine"
    bl_use_preview = False
    bl_use_shading_nodes_custom = False

    def render(self, depsgraph):
        pass

    def view_update(self, context, depsgraph):
        try:
            if SCENARIO:
                SCENARIO.view_update(self, context, depsgraph)
        except Exception:
            OUT["errors"].append(traceback.format_exc())
            quit_now("exception in view_update")
        self.tag_redraw()

    def view_draw(self, context, depsgraph):
        try:
            if SCENARIO:
                SCENARIO.view_draw(self, context, depsgraph)
        except Exception:
            OUT["errors"].append(traceback.format_exc())
            quit_now("exception in view_draw")
        if SCENARIO and SCENARIO.done:
            quit_now("scenario complete")


def _setup_viewport(kv):
    wm = bpy.context.window_manager
    win = wm.windows[0]
    scr = win.screen
    area = next(a for a in scr.areas if a.type == "VIEW_3D")
    space = area.spaces.active
    space.shading.type = "RENDERED"
    space.overlay.show_overlays = False
    space.show_gizmo = False
    if kv.get("full", "0") == "1":
        with bpy.context.temp_override(window=win, area=area, region=next(r for r in area.regions if r.type == "WINDOW")):
            bpy.ops.screen.screen_full_area(use_hide_panels=True)
    OUT["window"] = {"w": win.width, "h": win.height}
    return None


def _kick():
    # Report the actual region size we got, then request the first redraw.
    for win in bpy.context.window_manager.windows:
        for a in win.screen.areas:
            if a.type == "VIEW_3D":
                for r in a.regions:
                    if r.type == "WINDOW":
                        OUT["region"] = {"w": r.width, "h": r.height}
                a.tag_redraw()
    return None


def _watchdog():
    if time.perf_counter() - _t_start > WATCHDOG_S:
        quit_now("watchdog timeout")
    return 1.0


def start(scenario, out_path, kv):
    global SCENARIO, OUT_PATH
    SCENARIO, OUT_PATH = scenario, out_path
    OUT["blender"] = bpy.app.version_string
    OUT["args"] = kv
    bpy.utils.register_class(Engine)
    bpy.context.scene.render.engine = "SB_ENGINE"

    def stage1():
        try:
            _setup_viewport(kv)
        except Exception:
            OUT["errors"].append(traceback.format_exc())
            quit_now("viewport setup failed")
        bpy.app.timers.register(_kick, first_interval=1.0)
        return None

    bpy.app.timers.register(stage1, first_interval=0.5)
    bpy.app.timers.register(_watchdog, first_interval=5.0)
