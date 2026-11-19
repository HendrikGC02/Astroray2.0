# SPDX-License-Identifier: Apache-2.0
"""API-surface check (0007 d46): every bpy name the addon depends on, from blender/astroray/api_surface.txt.

Runs under the bpy wheel's Python (hosted CI) or inside Blender; prints OK/MISSING per line and exits 1 on
any MISSING. Line kinds (how each is checked is in the list's own comments):
  attr    Struct.path       RNA property or function; a path with indexing or several steps is walked on a
                            generated datablock (Mesh, Material, World, Image); roots `bpy` and `gpu` walk the module
  call    Struct.name       an RNA function (RenderEngine.begin_result) or a dotted module callable (gpu.types.GPUStorageBuf)
  module  openvdb member    the bundled module imports and has `member` on its grid classes
  pass    use_pass_z Depth  the ViewLayer flag exists and the pass name is a legal engine pass name
"""
import ast
import importlib
import os
import re
import sys
import warnings

# The addon still reads Material.use_nodes (deprecated for 6.0); the list names it, the noise is not the signal.
warnings.filterwarnings("ignore", category=DeprecationWarning)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIST = os.path.join(ROOT, "blender", "astroray", "api_surface.txt")
PASS_SOURCE = os.path.join(ROOT, "blender", "astroray", "render_final.py")


def _generate(struct):
    """A throwaway datablock of the named type, and how to remove it."""
    import bpy
    if struct == "Mesh":
        me = bpy.data.meshes.new("api_surface")
        me.from_pydata([(0, 0, 0), (1, 0, 0), (0, 1, 0)], [], [(0, 1, 2)])
        return me, lambda: bpy.data.meshes.remove(me)
    if struct == "Material":
        m = bpy.data.materials.new("api_surface")
        m.use_nodes = True
        return m, lambda: bpy.data.materials.remove(m)
    if struct == "World":
        w = bpy.data.worlds.new("api_surface")
        w.use_nodes = True
        return w, lambda: bpy.data.worlds.remove(w)
    if struct == "Image":
        im = bpy.data.images.new("api_surface", 1, 1)
        return im, lambda: bpy.data.images.remove(im)
    return None, None


def _walk(obj, path):
    """Follows `a.b['k'].c` from obj; raises on the first missing step."""
    for name, key in re.findall(r"\.?([A-Za-z_]\w*)|\[['\"]([^'\"]+)['\"]\]", path):
        obj = obj[key] if key else getattr(obj, name)
    return obj


def _rna_member(struct, name):
    import bpy
    cls = getattr(bpy.types, struct)
    rna = cls.bl_rna
    if name not in rna.properties and name not in rna.functions and not hasattr(cls, name):
        raise AttributeError(f"{struct} has no {name}")


def _resolve(spec):
    root, _, rest = spec.partition(".")
    if root in ("bpy", "gpu"):
        mod = importlib.import_module(root)
        _walk(mod, rest)
        return
    obj, remove = _generate(root)
    if obj is not None:
        try:
            _walk(obj, rest)
        finally:
            remove()
        return
    if not re.fullmatch(r"\w+", rest):
        raise AttributeError(f"{spec}: a path needs a generated {root}; add one to _generate")
    _rna_member(root, rest)


class NotInWheel(Exception):
    """A module Blender's own install bundles but the pip wheel does not (openvdb)."""


def _in_blender():
    import bpy
    return os.path.basename(bpy.app.binary_path).lower().startswith("blender")


def _check_module(spec):
    mod, _, member = spec.partition(" ")
    try:
        m = importlib.import_module(mod)
    except ImportError:
        if not _in_blender():
            raise NotInWheel(f"{mod} is bundled with Blender's install only; checked by `check.ps1 full` inside Blender")
        raise
    grids = [getattr(m, n) for n in dir(m) if n.endswith("Grid") and isinstance(getattr(m, n), type)]
    if not any(hasattr(g, member) for g in grids):
        raise AttributeError(f"no grid class of {mod} has {member}")


def _check_pass(spec):
    flag, name = spec.split()
    _rna_member("ViewLayer", flag)
    if not name or len(name.encode()) >= 64:
        raise ValueError(f"pass name {name!r} is not 1-63 bytes")


def _addon_pass_flags():
    """The (flag, name) pairs the addon itself requests, so a new pass cannot skip the list."""
    with open(PASS_SOURCE, encoding="utf-8") as f:
        for node in ast.walk(ast.parse(f.read())):
            if isinstance(node, ast.Assign) and any(getattr(t, "id", "") == "VIEW_LAYER_PASSES" for t in node.targets):
                return list(ast.literal_eval(node.value))
    raise RuntimeError("VIEW_LAYER_PASSES not found in render_final.py")


def parse(lines):
    out = []
    for raw in lines:
        s = raw.strip()
        if s and not s.startswith("#"):
            kind, _, spec = s.partition(" ")
            out.append((s, kind, spec.strip()))
    return out


def check(lines, addon_passes=None):
    """Returns (checked, missing, unchecked); missing lines are named as written."""
    checkers = {"attr": _resolve, "call": _resolve, "module": _check_module, "pass": _check_pass}
    entries = parse(lines)
    missing, unchecked = [], []
    for line, kind, spec in entries:
        try:
            if kind not in checkers:
                raise ValueError(f"unknown kind {kind!r}")
            checkers[kind](spec)
            print("OK", line)
        except NotInWheel as e:  # visible, counted apart, never silent
            print(f"UNCHECKED {line}  ({e})")
            unchecked.append(line)
        except Exception as e:  # any failure is a missing dependency, named with the reason
            print(f"MISSING {line}  ({type(e).__name__}: {e})")
            missing.append(line)
    listed = {spec for _, kind, spec in entries if kind == "pass"}
    for flag, name in addon_passes or ():
        if f"{flag} {name}" not in listed:
            print(f"MISSING pass {flag} {name}  (requested by render_final.py, not in api_surface.txt)")
            missing.append(f"pass {flag} {name}")
    return len(entries), missing, unchecked


def main(argv):
    path = argv[1] if len(argv) > 1 else LIST
    with open(path, encoding="utf-8") as f:
        lines = f.read().splitlines()
    import bpy
    print("bpy", bpy.app.version_string)
    checked, missing, unchecked = check(lines, _addon_pass_flags() if len(argv) <= 1 else None)
    print(f"api_surface: {checked} entries, {checked - len(unchecked)} checked, {len(unchecked)} unchecked, {len(missing)} missing")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
