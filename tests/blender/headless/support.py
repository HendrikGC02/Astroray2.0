# SPDX-License-Identifier: Apache-2.0
"""Loads the sync package without importing the addon (its __init__ needs the engine DLL)."""
import importlib.util
import os
import sys

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
