# SPDX-License-Identifier: Apache-2.0
"""Loads the sync package without importing the addon (its __init__ needs the engine DLL)."""
import importlib.util
import os
import sys
import types

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))


def load_sync():
    if "ar_sync" in sys.modules:
        return sys.modules["ar_sync"]
    path = os.path.join(ROOT, "blender", "astroray", "sync")
    spec = importlib.util.spec_from_file_location(
        "ar_sync", os.path.join(path, "__init__.py"), submodule_search_locations=[path])
    mod = importlib.util.module_from_spec(spec)
    sys.modules["ar_sync"] = mod
    spec.loader.exec_module(mod)
    return mod


GEN = os.path.join(ROOT, "out", "build", "gen")


def stage_addon(name):
    """The addon package `name` without its __init__ (which needs the engine DLL), with the build's generated
    `_abi` and `_ops` staged in it as the extension build does. A missing generated module raises: the
    headless tier runs after a build, and an addon without `_ops` must never import."""
    if name not in sys.modules:
        for mod in ("_abi", "_ops"):
            path = os.path.join(GEN, mod + ".py")
            if not os.path.exists(path):
                raise RuntimeError(f"{path} is missing: run tools/build.ps1 first")
        pkg = types.ModuleType(name)
        pkg.__path__ = [os.path.join(ROOT, "blender", "astroray")]
        sys.modules[name] = pkg
        for mod in ("_abi", "_ops"):
            spec = importlib.util.spec_from_file_location(f"{name}.{mod}", os.path.join(GEN, mod + ".py"))
            m = importlib.util.module_from_spec(spec)
            sys.modules[f"{name}.{mod}"] = m
            spec.loader.exec_module(m)
            setattr(pkg, mod, m)
    return sys.modules[name]


_props = {"module": None, "users": 0}


def acquire_properties():
    """Registers scene.astroray, as the addon does before any sync code runs (classify reads it). Counted, so
    a module and a class that both need it compose: the last release unregisters. The file is loaded bare,
    as the addon's __init__ needs the engine DLL."""
    if _props["module"] is None:
        path = os.path.join(ROOT, "blender", "astroray", "properties.py")
        spec = importlib.util.spec_from_file_location("ar_properties", path)
        _props["module"] = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(_props["module"])
    if _props["users"] == 0:
        _props["module"].register()
    _props["users"] += 1
    return _props["module"]


def release_properties():
    _props["users"] -= 1
    if _props["users"] == 0:
        _props["module"].unregister()


def reset_scene():
    import bpy

    bpy.ops.wm.read_factory_settings(use_empty=True)
    return bpy.context.scene


def depsgraph():
    import bpy

    return bpy.context.evaluated_depsgraph_get()


def in_update(edit, fn):
    """Runs edit, then returns fn(depsgraph) as computed inside depsgraph_update_post (None if
    Blender delivered no update).

    depsgraph.updates is only populated while Blender's update handlers run.
    """
    import bpy

    results = []

    def handler(scene, dg):
        if len(dg.updates):
            results.append(fn(dg))

    bpy.app.handlers.depsgraph_update_post.append(handler)
    try:
        edit()
        bpy.context.evaluated_depsgraph_get().update()
    finally:
        bpy.app.handlers.depsgraph_update_post.remove(handler)
    return results[-1] if results else None


def main():
    """Entry for a test file run alone (land's canary, Blender, or plain python with the wheel).

    Output goes to stdout because Invoke-ArBlender does not capture stderr; arguments are
    those after Blender's '--', or sys.argv[1:] outside Blender.
    """
    import unittest

    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    prog = os.path.basename(sys.argv[0])
    res = unittest.main(module="__main__", argv=[prog] + args, exit=False,
                        testRunner=unittest.TextTestRunner(stream=sys.stdout, verbosity=2))
    sys.exit(0 if res.result.wasSuccessful() else 1)
