# SPDX-License-Identifier: Apache-2.0
"""Proof of the V7a capture (task 127): a windowed Blender whose 3D viewport background is switched between two
solid colours from a timer, while tools/ttff_poller.ps1 (DXGI Desktop Duplication) records the viewport. For
every switch the first record whose blocks are close to the new colour is the capture's detection; its delay
after the commanded switch is the latency of the whole chain (timer callback, redraw, present, duplication).
A measurement aid, not a test. Run:
  Invoke-ArBlender -Gui -Script tools/ttff_capture_proof.py -Args @('--out', '<dir>')
Ref: tools/ttff_poller.ps1; docs/decisions/0007-blender-integration.md d35.
"""
import json
import os
import subprocess
import sys
import time

import numpy as np

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
COLORS = [(0.9, 0.05, 0.05), (0.05, 0.1, 0.9)]
SWITCHES, PERIOD_S = 12, 1.0
out = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else os.path.join(HERE, "..", "out", "ttff")
os.makedirs(out, exist_ok=True)
prefix = os.path.join(out, "proof.cap")
stop = prefix + ".stop"
for p in (prefix + ".ready", prefix + ".bin", prefix + ".json", stop):
    if os.path.exists(p):
        os.remove(p)
DRAWS = [0]


def on_draw():
    DRAWS[0] += 1


bpy.types.SpaceView3D.draw_handler_add(on_draw, (), "WINDOW", "POST_PIXEL")
st = {"step": "setup", "t": time.monotonic(), "n": 0, "sw": []}


def resize_window(w, h):
    """Moves Blender's own window to (60, 60) with the client size w x h (experiment: a window that is not maximised)."""
    import ctypes
    from ctypes import wintypes
    u = ctypes.windll.user32
    found = []
    cb = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)(
        lambda hwnd, _: (found.append(hwnd) if u.IsWindowVisible(hwnd) and _owner(hwnd) == os.getpid() else None) or True)
    u.EnumWindows(cb, 0)
    for hwnd in found:
        u.ShowWindow(hwnd, 9)
        u.SetWindowPos(hwnd, 0, 60, 60, w, h, 0x0004)
    return len(found)


def _owner(hwnd):
    import ctypes
    pid = ctypes.c_ulong()
    ctypes.windll.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    return pid.value


def area():
    for win in bpy.context.window_manager.windows:
        for a in win.screen.areas:
            if a.type == "VIEW_3D":
                return win, a, next(r for r in a.regions if r.type == "WINDOW")
    raise RuntimeError("no 3D viewport")


def finish(code):
    sys.stdout.flush()
    os._exit(code)


def analyse(poller):
    text = poller.communicate(timeout=60)[0]
    meta = json.load(open(prefix + ".json"))
    raw = np.fromfile(prefix + ".bin", dtype=np.dtype([("t", "<f8"), ("g", "u1", (18, 32, 3))]))
    raw = raw[np.argsort(raw["t"], kind="stable")]
    t, g = raw["t"], raw["g"].astype(np.float32)
    res = {"poller": text.strip(), "meta": meta, "switches": []}
    # The settled colour of each phase (mean of the last records before the next switch) is its reference.
    ends = [s["t"] for s in st["sw"]] + [t[-1] + 1]
    refs = []
    for i, s in enumerate(st["sw"]):
        sel = (t >= ends[i + 1] - 0.3) & (t < ends[i + 1])
        refs.append(g[sel].mean(axis=(0, 1, 2)) if sel.any() else None)
        # mean BGR of the whole crop for the phase
    sel0 = (t >= st["sw"][0]["t"] - 0.3) & (t < st["sw"][0]["t"])
    prev = g[sel0].mean(axis=(0, 1, 2)) if sel0.any() else None
    if prev is None:
        raise RuntimeError("no records before the first switch")
    lat = []
    for i, s in enumerate(st["sw"]):
        ref = refs[i]
        if ref is None:
            res["switches"].append({"n": i, "error": "no records at the end of the phase"})
            continue
        sel = np.nonzero((t >= s["t"]) & (t < ends[i + 1]))[0]
        mean = g[sel].mean(axis=(1, 2))
        d = np.abs(mean - ref).max(axis=1)
        hit = np.nonzero(d < 6.0)[0]
        r = {"n": i, "bgr": [round(float(v), 1) for v in ref]}
        r["contrast"] = round(float(np.abs(ref - prev).max()), 1)
        if len(hit) and np.abs(ref - prev).max() > 40:
            r["latency_ms"] = round(float(t[sel[hit[0]]] - s["t"]) * 1000, 1)
            lat.append(r["latency_ms"])
        else:
            r["error"] = "no detection or no contrast"
        prev = ref
        res["switches"].append(r)
    res["latency_ms"] = {"n": len(lat), "median": float(np.median(lat)) if lat else None,
                         "min": min(lat) if lat else None, "max": max(lat) if lat else None}
    json.dump(res, open(os.path.join(out, "proof.json"), "w"), indent=1)
    print("PROOF viewport draws in Blender:", DRAWS[0], flush=True)
    print("PROOF poller:", text.strip(), flush=True)
    print("PROOF", json.dumps(res["latency_ms"]), json.dumps(meta), flush=True)
    for s in res["switches"]:
        print("PROOF switch", s, flush=True)
    return 0 if len(lat) == SWITCHES else 1


def tick():
    try:
        now = time.monotonic()
        if st["step"] == "setup":
            if "--size" in sys.argv and not st.get("sized"):
                w, h = (int(v) for v in sys.argv[sys.argv.index("--size") + 1].split("x"))
                print("PROOF resized windows:", resize_window(w, h), flush=True)
                st["sized"] = True
                return 1.0
            win, a, region = area()
            sp = a.spaces[0]
            sp.shading.type = "SOLID"
            sp.shading.background_type = "VIEWPORT"
            sp.shading.background_color = COLORS[0]
            sp.overlay.show_overlays = False
            sp.show_gizmo = False
            r = [region.x, win.height - region.y - region.height, region.width, region.height]
            if "--full" in sys.argv:   # debugging: the whole client area of Blender's window in the dump
                r = [0, 0, 0, 0]
            st["poller"] = subprocess.Popen(
                ["pwsh", "-NoProfile", "-File", os.path.join(HERE, "ttff_poller.ps1"), "-ProcessId", str(os.getpid()),
                 "-OutPrefix", prefix, "-StopFile", stop, "-RegionX", str(r[0]), "-RegionY", str(r[1]),
                 "-RegionW", str(r[2]), "-RegionH", str(r[3]), "-CropW", "1920", "-CropH", "1080", "-DumpPng", os.path.join(out, "proof_last_crop.png")], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            st["step"], st["t"] = "ready", now
        elif st["step"] == "ready":
            if os.path.exists(prefix + ".ready") and now - st["t"] > 4.0:
                st["step"], st["t"] = "run", now
            elif st["poller"].poll() is not None:
                raise RuntimeError("poller exited: " + st["poller"].communicate()[0])
        elif st["step"] == "run":
            if now - st["t"] >= PERIOD_S:
                win, a, region = area()
                c = COLORS[(st["n"] + 1) % 2]   # the first switch leaves the initial colour
                a.spaces[0].shading.background_color = c
                a.tag_redraw()
                if "--swap" in sys.argv:   # experiment: force a draw with a buffer swap of the whole window
                    with bpy.context.temp_override(window=win, area=a, region=region):
                        bpy.ops.wm.redraw_timer(type="DRAW_WIN_SWAP", iterations=1)
                st["sw"].append({"t": time.time(), "color": c})
                st["t"] = now
                st["n"] += 1
                if st["n"] >= SWITCHES:
                    st["step"] = "end"
        elif st["step"] == "end":
            if now - st["t"] >= PERIOD_S:
                open(stop, "w").close()
                finish(analyse(st["poller"]))
    except Exception:
        import traceback
        traceback.print_exc()
        finish(1)
    return 0.002


bpy.app.timers.register(tick, first_interval=1.0)
