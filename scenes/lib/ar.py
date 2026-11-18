# SPDX-License-Identifier: Apache-2.0
"""Scene builders call the C ABI through the generated binding with recording on (0009 d19).

Every call is checked: a status other than AR_OK raises with the engine's last error text, so a
builder never writes a recording that the engine refused. Lengths are metres (0003 d9).
Needs a built tree: out/bin/astroray_core.dll and out/build/gen/_abi.py, _ops.py (tools/build.ps1).
Ref: docs/decisions/0009-build-test-process.md d19; 0007-appendix-abi.md (descriptors, TEX_IMAGE
parameters); src/kernel/vm/ops/store_slot.h (record slots, read from the file, never copied).
"""
import base64
import ctypes
import importlib.util
import json
import math
import os
import re
import tempfile
import zlib

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DLL = os.path.join(ROOT, "out", "bin", "astroray_core.dll")
GEN = os.path.join(ROOT, "out", "build", "gen")
STORE_SLOT_H = os.path.join(ROOT, "src", "kernel", "vm", "ops", "store_slot.h")
ALL_VISIBILITY = 0x3F  # AR_VISIBILITY_CAMERA .. AR_VISIBILITY_VOLUME

INSTANCE_KINDS = ("object", "collection", "points")

# TEX_IMAGE mode bits (0007 appendix, ar_image_desc row).
TEX_LINEAR, TEX_EXTEND = 1, 2


def _load(name, path):
    if not os.path.exists(path):
        raise RuntimeError(f"{path} is missing: run tools/build.ps1 first")
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _record_slots():
    """OpenPBR input name -> (RecordSlot number, kind), read from the kernel's own slot table so
    the numbers have one source. Kinds: scalar, colour, ior, frame (store_slot.h)."""
    with open(STORE_SLOT_H, encoding="utf-8") as f:
        text = f.read()
    scalars = re.search(r"#define AR_RECORD_SCALARS\(X\)(.*?)\n//", text, re.S).group(1)
    spectral = re.search(r"#define AR_RECORD_SPECTRAL\(X\)(.*?)\n#define", text, re.S).group(1)
    frames = re.search(r"#define AR_RECORD_FRAMES\(X\)(.*?)\n", text).group(1)
    slots = {}
    for n in re.findall(r"X\((\w+)\)", scalars):
        slots[n] = (len(slots), "scalar")
    for n in re.findall(r"X\((\w+),", spectral):
        slots[n] = (len(slots), "ior" if n == "specular_ior" else "colour")
    for n in re.findall(r"X\((\w+)\)", frames):
        slots[n] = (len(slots), "frame")
    if len(slots) != 37 or slots["base_color"][0] != 24 or slots["geometry_normal"][0] != 33:
        raise RuntimeError(f"{STORE_SLOT_H}: the slot table no longer parses to 24 + 9 + 4 slots")
    return slots


SLOTS = _record_slots()


class AbiError(RuntimeError):
    pass


def _arr(a, dtype):
    """A NumPy array as JSON: dtype, shape and base64 of the zlib-compressed bytes (a mesh is too
    large for a list, and B-5M's 2,000 meshes would otherwise be half a gigabyte of text)."""
    a = np.ascontiguousarray(a, dtype=dtype)
    return {"dtype": np.dtype(dtype).name, "shape": list(a.shape), "z": 1,
            "b64": base64.b64encode(zlib.compress(a.tobytes(), 1)).decode("ascii")}


def array_from_json(d):
    """The inverse of _arr, for scenes/lib/blend_writer.py."""
    raw = base64.b64decode(d["b64"])
    return np.frombuffer(zlib.decompress(raw) if d.get("z") else raw, dtype=d["dtype"]).reshape(d["shape"]).copy()


def name64(text, n=64):
    return (ctypes.c_uint8 * n)(*text.encode().ljust(n, b"\0"))


def xform(rows):
    """A 3x4 row-major float array from 12 numbers, or a 3x4 / 4x4 array."""
    a = np.asarray(rows, dtype=np.float64).reshape(-1)
    if a.size == 16:
        a = a[:12]
    if a.size != 12:
        raise ValueError("a transform needs 12 numbers (3x4 row-major)")
    return (ctypes.c_float * 12)(*a.astype(np.float32))


def translation(t, scale=1.0):
    return [scale, 0, 0, t[0], 0, scale, 0, t[1], 0, 0, scale, t[2]]


IDENTITY = translation((0, 0, 0))


def look_at(position, target, up=(0.0, 0.0, 1.0)):
    """Camera-to-world 3x4 for a Blender camera (looks down its -z, +y up in the image)."""
    p, t, u = (np.asarray(v, dtype=np.float64) for v in (position, target, up))
    fwd = t - p
    if np.linalg.norm(fwd) == 0:
        raise ValueError("camera position equals target")
    fwd /= np.linalg.norm(fwd)
    right = np.cross(fwd, u)
    if np.linalg.norm(right) < 1e-9:
        raise ValueError("camera up vector is parallel to the view direction; pass another up")
    right /= np.linalg.norm(right)
    up_c = np.cross(right, fwd)
    cols = np.stack([right, up_c, -fwd], axis=1)  # camera axes as columns
    return np.concatenate([cols, p[:, None]], axis=1).reshape(12)


def projection_inverse(width, height, focal_mm, sensor_mm):
    """NDC to camera space for a pinhole camera with Blender's automatic sensor fit: the sensor
    length is the larger image side. Row-major 4x4 mapping (nx, ny) to (x, y, -1) at w = 1."""
    view = sensor_mm / focal_mm  # full extent at z = -1 along the fitted side
    hx = 0.5 * view * (1.0 if width >= height else width / height)
    hy = 0.5 * view * (height / width if width >= height else 1.0)
    m = np.zeros((4, 4))
    m[0, 0], m[1, 1], m[2, 3], m[3, 3] = hx, hy, -1.0, 1.0
    return m.reshape(16)


class Node:
    def __init__(self, index):
        self.index = index


class Graph:
    """An op graph for ar_edit_set_graph (0007 d8; ops from src/kernel/vm/ops.def, ids from _ops.py)."""

    def __init__(self, op_id):
        self.op_id = op_id
        self.nodes, self.links, self.f32, self.i32 = [], [], [], []
        self._stored = set()

    def add(self, op, f32=(), i32=()):
        self.nodes.append((self.op_id[op], len(self.f32), len(f32), len(self.i32), len(i32)))
        self.f32.extend(float(v) for v in f32)
        self.i32.extend(int(v) for v in i32)
        return len(self.nodes) - 1

    def link(self, src, dst, socket):
        self.links.append((src, 0, dst, socket))

    def const(self, value):
        """A constant node: one number is CONST_F32, an RGB triple CONST_F32X3."""
        if np.isscalar(value):
            return Node(self.add("CONST_F32", f32=(value,)))
        v = tuple(value)
        if len(v) != 3:
            raise ValueError("a constant is one number or an RGB triple")
        return Node(self.add("CONST_F32X3", f32=v))

    def store(self, name, value):
        """Stores a constant (number or RGB triple) or a Node into an OpenPBR input by its record
        name (base_color, specular_weight, ...). A number stored into a colour slot is grey."""
        if name not in SLOTS:
            raise ValueError(f"unknown OpenPBR input '{name}'; known: {sorted(SLOTS)}")
        if name in self._stored:
            raise ValueError(f"OpenPBR input '{name}' is stored twice")
        slot, kind = SLOTS[name]
        if isinstance(value, Node):
            src = value
        else:
            if kind in ("scalar", "ior"):
                if not np.isscalar(value):
                    raise ValueError(f"'{name}' takes one number")
            else:
                if np.isscalar(value):
                    value = (value, value, value)
                if len(tuple(value)) != 3:
                    raise ValueError(f"'{name}' takes an RGB triple")
            src = self.const(value)
        n = self.add("STORE_SLOT", i32=(slot,))
        self.link(src.index, n, 0)
        self._stored.add(name)

    def uv(self, uvset=0):
        return Node(self.add("LOAD_CTX_UV", i32=(uvset,)))

    def tex_image(self, uv, image_key, linear=True, extend=False):
        """Samples image `image_key` (its low 32 bits cross the graph) at `uv`."""
        if not 0 <= image_key < 2**32:
            raise ValueError("a TEX_IMAGE image key must fit 32 bits")
        mode = (TEX_LINEAR if linear else 0) | (TEX_EXTEND if extend else 0)
        n = self.add("TEX_IMAGE", i32=(image_key, mode))
        self.link(uv.index, n, 0)
        return Node(n)

    def emission(self, colour, strength):
        """Emission colour (an RGB triple or a Node) and strength (a number or a Node)."""
        c = colour if isinstance(colour, Node) else self.const(tuple(colour))
        s = strength if isinstance(strength, Node) else self.const(float(strength))
        n = self.add("EMIT_LE")
        self.link(c.index, n, 0)
        self.link(s.index, n, 1)

    def desc(self, abi):
        """The ar_graph_desc and the arrays it points into (keep them alive across the call)."""
        d = abi.ar_graph_desc()
        d.node_count, d.link_count = len(self.nodes), len(self.links)
        d.f32_count, d.i32_count = len(self.f32), len(self.i32)
        nodes = (abi.ar_graph_node * max(1, len(self.nodes)))()
        for i, (op, ff, fc, fi, ic) in enumerate(self.nodes):
            nodes[i].op, nodes[i].f32_first, nodes[i].f32_count = op, ff, fc
            nodes[i].i32_first, nodes[i].i32_count = fi, ic
        links = (abi.ar_graph_link * max(1, len(self.links)))(*[abi.ar_graph_link(*lk) for lk in self.links])
        f32 = (ctypes.c_float * max(1, len(self.f32)))(*self.f32)
        i32 = (ctypes.c_int32 * max(1, len(self.i32)))(*self.i32)
        d.nodes, d.links, d.params_f32, d.params_i32 = nodes, links, f32, i32
        return d, (nodes, links, f32, i32)


class Scene:
    """One recorded scene: context, scene, one edit transaction, then one session on `finish`.

    Use it as a context manager: a build that raises leaves no recording behind, because the
    recording is written beside its target and renamed only when `finish` succeeds."""

    def __init__(self, path, device="cpu", workers=2, description=None):
        self.path = os.path.abspath(path)
        # What the builder recorded, for scenes/lib/blend_writer.py (one writer for every builder,
        # task 084): written as JSON by finish() when `description` names a file. Only what a Blender
        # scene can say is kept: constant Principled inputs, lamps, a constant world, a pinhole camera.
        self.description_path = os.path.abspath(description) if description else None
        self._desc = {"meshes": [], "materials": [], "instances": [], "lights": [], "world": None,
                      "camera": None, "settings": None}
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        self._tmp = tempfile.TemporaryDirectory()
        self.abi = _load("_ar_abi", os.path.join(GEN, "_abi.py"))
        self.op_id = _load("_ar_ops", os.path.join(GEN, "_ops.py")).OP_ID
        self.lib = ctypes.CDLL(DLL)
        self.abi.check(self.lib)
        self.abi.bind(self.lib)
        a = self.abi
        self._partial = self.path + ".partial"
        self.ctx, self.scene_h, self.edit_h = ctypes.c_void_p(), ctypes.c_void_p(), ctypes.c_void_p()
        self.session_h = ctypes.c_void_p()
        self._keys, self._next_key = set(), 1
        self.size, self._camera, self._finished = None, None, False
        desc = a.ar_context_desc(a.AR_DEVICE_CPU if device == "cpu" else a.AR_DEVICE_CUDA, 0,
                                 os.path.join(self._tmp.name, "context.log").encode(), self._partial.encode())
        self._call(self.lib.ar_context_create(ctypes.byref(desc), ctypes.byref(self.ctx)), "ar_context_create")
        sd = a.ar_scene_desc(None, workers, 0)
        self._call(self.lib.ar_scene_create(self.ctx, ctypes.byref(sd), ctypes.byref(self.scene_h)), "ar_scene_create")
        self._call(self.lib.ar_scene_edit_begin(self.scene_h, ctypes.byref(self.edit_h)), "ar_scene_edit_begin")

    # -- plumbing ------------------------------------------------------------------------------
    def _last_error(self):
        buf = (ctypes.c_uint8 * 512)()
        for fn, h in ((self.lib.ar_edit_last_error, self.edit_h), (self.lib.ar_scene_last_error, self.scene_h),
                      (self.lib.ar_context_last_error, self.ctx)):
            if h.value and fn(h, 512, buf) == self.abi.AR_OK and buf[0]:
                return bytes(buf).split(b"\0")[0].decode(errors="replace")
        return ""

    def _call(self, status, what):
        if status != self.abi.AR_OK:
            raise AbiError(f"{what} returned {status}: {self._last_error()}")

    def key(self, key=None):
        """A fresh ar_key, or `key` after checking it is unused (keys never repeat in one scene)."""
        if key is None:
            while self._next_key in self._keys:
                self._next_key += 1
            key = self._next_key
        if key in self._keys or key <= 0:
            raise ValueError(f"ar_key {key} is invalid or already used")
        self._keys.add(key)
        return key

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        if not self._finished:
            self._teardown()
            if os.path.exists(self._partial):
                os.remove(self._partial)
        self._tmp.cleanup()
        return False

    def _teardown(self):
        lib = self.lib
        for fn, h in ((lib.ar_session_destroy, self.session_h), (lib.ar_scene_destroy, self.scene_h),
                      (lib.ar_context_destroy, self.ctx)):
            if h.value:
                fn(h)
                h.value = None

    # -- geometry and images -----------------------------------------------------------------------
    @staticmethod
    def _fill(slot, count, dtype, values):
        ptr = ctypes.cast(slot, ctypes.POINTER(ctypes.c_void_p))[0]
        values = np.ascontiguousarray(values, dtype=dtype).reshape(-1)
        if values.size != count:
            raise ValueError(f"staged array wants {count} values, got {values.size}")
        if count:
            dst = np.frombuffer((ctypes.c_char * (count * values.itemsize)).from_address(ptr), dtype=dtype)
            dst[:] = values

    def mesh(self, key, positions, corner_vert, face_start, normals, uv=None, name="mesh"):
        """A mesh in metres. positions (V,3); corner_vert (C,); face_start (F,); normals (C,3) per
        corner; uv (C,2) per corner or None."""
        a = self.abi
        positions = np.asarray(positions, dtype=np.float32).reshape(-1, 3)
        corner_vert = np.asarray(corner_vert, dtype=np.int32).reshape(-1)
        face_start = np.asarray(face_start, dtype=np.int32).reshape(-1)
        if corner_vert.size and (corner_vert.min() < 0 or corner_vert.max() >= len(positions)):
            raise ValueError("corner_vert indexes outside the positions")
        d = a.ar_mesh_desc()
        d.key, d.vertex_count, d.corner_count, d.face_count = key, len(positions), corner_vert.size, face_start.size
        d.uv_layer_count, d.motion_steps = (1 if uv is not None else 0), 1
        d.name = name64(name)
        b = a.ar_mesh_buffers()
        self._call(self.lib.ar_edit_mesh_begin(self.edit_h, ctypes.byref(d), ctypes.byref(b)), "ar_edit_mesh_begin")
        self._fill(b.positions, b.positions_count, np.float32, positions)
        self._fill(b.corner_vert, b.corner_vert_count, np.int32, corner_vert)
        self._fill(b.face_start, b.face_start_count, np.int32, face_start)
        self._fill(b.corner_normals, b.corner_normals_count, np.float32, normals)
        if uv is not None:
            self._fill(b.uv, b.uv_count, np.float32, uv)
        self._call(self.lib.ar_edit_item_commit(self.edit_h, key), "ar_edit_item_commit")
        self._desc["meshes"].append({
            "key": key, "name": name, "positions": _arr(positions, np.float32),
            "corner_vert": _arr(corner_vert, np.int32), "face_start": _arr(face_start, np.int32),
            "normals": _arr(np.asarray(normals).reshape(-1, 3), np.float32),
            "uv": None if uv is None else _arr(np.asarray(uv).reshape(-1, 2), np.float32)})

    def mesh_from(self, key, m, name="mesh"):
        """Submits a scenes/lib/meshes.py Mesh."""
        self.mesh(key, m.positions, m.corner_vert, m.face_start, m.normals, m.uv, name)

    def image(self, key, path=None, data=None, interop=None):
        """An image from a file or from packed bytes; `interop` is an AR_COLOR_INTEROP_* value
        (default sRGB)."""
        a = self.abi
        if (path is None) == (data is None):
            raise ValueError("give exactly one of path and data")
        d = a.ar_image_desc()
        d.key = key
        d.color_interop = a.AR_COLOR_INTEROP_SRGB_REC709 if interop is None else interop
        if path is not None:
            d.source = a.AR_IMAGE_SOURCE_FILE
            keep = os.path.abspath(path).encode()
            d.path = keep
        else:
            d.source = a.AR_IMAGE_SOURCE_MEMORY
            keep = (ctypes.c_uint8 * len(data)).from_buffer_copy(bytes(data))
            d.byte_count = len(data)
            d.bytes = ctypes.cast(keep, ctypes.POINTER(ctypes.c_uint8))
        self._call(self.lib.ar_edit_set_image(self.edit_h, ctypes.byref(d)), "ar_edit_set_image")
        del keep

    def instances(self, items):
        """items: dicts with geom (mesh key), materials (list of material keys that a face's
        material_index indexes), optional xf (3x4) and visibility. Each instance has its own
        slice of the material pool. Only the .blend description sees the optional `kind`: how a
        Blender scene makes the instance ("object": an object with its own or a shared mesh,
        "collection": an empty instancing a collection, "points": Instance on Points in a
        geometry-nodes modifier) and `group`, which names the source collection or the modifier's
        host for the last two kinds. The engine gets the same flat instance either way."""
        a = self.abi
        n = len(items)
        for i, it in enumerate(items):
            kind = it.get("kind", "object")
            if kind not in INSTANCE_KINDS:
                raise ValueError(f"instance {i}: kind {kind!r} is not one of {INSTANCE_KINDS}")
            if (kind == "object") != ("group" not in it):
                raise ValueError(f"instance {i}: `group` is for the collection and points kinds only")
        pool, offsets, counts = [], [], []
        for it in items:
            offsets.append(len(pool))
            counts.append(len(it["materials"]))
            pool.extend(it["materials"])
        d = a.ar_instances_desc()
        d.instance_count, d.motion_steps, d.transform_count, d.material_key_count = n, 1, n, len(pool)

        def arr(ctype, values):
            return (ctype * max(1, len(values)))(*values)

        keep = dict(
            geom=arr(ctypes.c_uint64, [it["geom"] for it in items]),
            xf=(a.ar_xform * n)(*[a.ar_xform(xform(it.get("xf", IDENTITY))) for it in items]),
            obj=arr(ctypes.c_uint64, [self.key() for _ in items]),
            rid=arr(ctypes.c_uint32, range(n)), pidx=arr(ctypes.c_uint32, [0] * n),
            co=arr(ctypes.c_uint32, [0] * n), ca=arr(ctypes.c_uint32, [0] * n),
            col=(a.ar_rgba * n)(*[a.ar_rgba((ctypes.c_float * 4)(1, 1, 1, 1)) for _ in items]),
            vis=arr(ctypes.c_uint32, [it.get("visibility", ALL_VISIBILITY) for it in items]),
            flags=arr(ctypes.c_uint32, [0] * n), lg=arr(ctypes.c_uint8, [0] * n),
            moff=arr(ctypes.c_uint32, offsets), mcnt=arr(ctypes.c_uint32, counts),
            mkeys=arr(ctypes.c_uint64, pool))
        d.geom_keys, d.xf, d.object_keys = keep["geom"], keep["xf"], keep["obj"]
        d.random_ids, d.pass_indices, d.crypto_objects, d.crypto_assets = keep["rid"], keep["pidx"], keep["co"], keep["ca"]
        d.colors, d.visibility, d.flags, d.light_groups = keep["col"], keep["vis"], keep["flags"], keep["lg"]
        d.material_offsets, d.material_counts, d.material_keys = keep["moff"], keep["mcnt"], keep["mkeys"]
        self._call(self.lib.ar_edit_set_instances(self.edit_h, ctypes.byref(d)), "ar_edit_set_instances")
        self._desc["instances"] = [
            {"geom": it["geom"], "materials": list(it["materials"]),
             "xf": [float(v) for v in np.asarray(it.get("xf", IDENTITY), np.float64).reshape(-1)[:12]],
             "visibility": it.get("visibility", ALL_VISIBILITY), "kind": it.get("kind", "object"),
             "group": it.get("group")} for it in items]

    # -- shading ---------------------------------------------------------------------------------
    def graph(self):
        return Graph(self.op_id)

    def set_graph(self, key, graph, target=None):
        a = self.abi
        d, keep = graph.desc(a)
        t = a.AR_TARGET_SURFACE if target is None else target
        self._call(self.lib.ar_edit_set_graph(self.edit_h, key, t, ctypes.byref(d)), "ar_edit_set_graph")
        del keep

    def material(self, inputs=None, emission=None, emission_sampling=None):
        """A surface material from constants: `inputs` maps OpenPBR input names to numbers or RGB
        triples; `emission` is (rgb, strength). Returns the material key."""
        g = self.graph()
        for name, value in (inputs or {}).items():
            g.store(name, value)
        if emission is not None:
            g.emission(*emission)
        key = self.material_from_graph(g, emission_sampling)
        self._desc["materials"].append({
            "key": key, "inputs": {k: ([float(x) for x in v] if not np.isscalar(v) else float(v))
                                   for k, v in (inputs or {}).items()},
            "emission": None if emission is None else {"rgb": [float(x) for x in emission[0]],
                                                       "strength": float(emission[1])}})
        return key

    def material_from_graph(self, graph, emission_sampling=None):
        """A material whose surface graph is `graph` (a Graph with textures or other nodes)."""
        a = self.abi
        gkey = self.key()
        self.set_graph(gkey, graph)
        m = a.ar_material_desc()
        m.surface_graph = gkey
        m.emission_sampling = a.AR_EMISSION_SAMPLING_AUTO if emission_sampling is None else emission_sampling
        key = self.key()
        self._call(self.lib.ar_edit_set_material(self.edit_h, key, ctypes.byref(m)), "ar_edit_set_material")
        return key

    def world(self, rgb, strength=1.0):
        """A constant world: colour times strength as the world graph's emission."""
        a = self.abi
        g = self.graph()
        g.emission(rgb, strength)
        gkey = self.key()
        self.set_graph(gkey, g, a.AR_TARGET_WORLD)
        w = a.ar_world_desc()
        w.surface_graph = gkey
        self._call(self.lib.ar_edit_set_world(self.edit_h, ctypes.byref(w)), "ar_edit_set_world")
        self._desc["world"] = {"rgb": [float(x) for x in rgb], "strength": float(strength)}

    def light(self, kind, xf=IDENTITY, rgb=(1.0, 1.0, 1.0), power=1.0, size=(0.0, 0.0), spread=math.pi,
              spot_size=0.0, spot_blend=0.0, sun_angle=0.0, normalize=1, cast_shadow=1):
        """A lamp of type 'point', 'spot', 'sun', 'rect' or 'ellipse'. Sizes are metres: POINT and
        SPOT radius = size[0]; RECT and ELLIPSE full extents (diameters); spread in radians."""
        a = self.abi
        types = {"point": a.AR_LIGHT_POINT, "spot": a.AR_LIGHT_SPOT, "sun": a.AR_LIGHT_SUN,
                 "rect": a.AR_LIGHT_AREA_RECT, "ellipse": a.AR_LIGHT_AREA_ELLIPSE}
        if kind not in types:
            raise ValueError(f"unknown lamp type '{kind}'")
        d = a.ar_light_desc()
        d.color.kind = a.AR_SPECTRUM_RGB
        d.color.rgb = (ctypes.c_float * 3)(*rgb)
        d.color.scale = 1.0
        d.xf = a.ar_xform(xform(xf))
        d.type, d.visibility = types[kind], ALL_VISIBILITY
        d.size_x, d.size_y, d.spot_size, d.spot_blend = size[0], size[1], spot_size, spot_blend
        d.sun_angle, d.spread, d.power, d.normalize, d.cast_shadow = sun_angle, spread, power, normalize, cast_shadow
        key = self.key()
        self._call(self.lib.ar_edit_set_light(self.edit_h, key, ctypes.byref(d)), "ar_edit_set_light")
        self._desc["lights"].append({
            "kind": kind, "xf": [float(v) for v in np.asarray(xf, np.float64).reshape(-1)[:12]],
            "rgb": [float(x) for x in rgb], "power": float(power), "size": [float(x) for x in size],
            "spread": float(spread), "normalize": int(normalize), "cast_shadow": int(cast_shadow)})
        return key

    # -- camera and settings --------------------------------------------------------------------------
    def camera(self, position, target, focal_mm, sensor_mm, width, height, up=(0.0, 0.0, 1.0)):
        """A pinhole camera at `position` looking at `target`; focal length and sensor in mm (the
        sensor length is the larger image side)."""
        a = self.abi
        c = a.ar_camera_desc()
        c.cam_to_world = a.ar_xform(xform(look_at(position, target, up)))
        c.projection_inverse = (ctypes.c_float * 16)(*projection_inverse(width, height, focal_mm, sensor_mm))
        c.type, c.near_clip, c.far_clip = a.AR_CAMERA_PERSP, 0.01, 1000.0
        self._call(self.lib.ar_edit_set_camera(self.edit_h, ctypes.byref(c)), "ar_edit_set_camera")
        self._camera = c
        self.size = (width, height)
        self._desc["camera"] = {
            "cam_to_world": [float(v) for v in look_at(position, target, up)], "focal_mm": float(focal_mm),
            "sensor_mm": float(sensor_mm), "width": width, "height": height, "near": 0.01, "far": 1000.0}

    def settings(self, spp, width, height, seed=1, max_bounces=8, pixel_filter="box", filter_width=1.0,
                 sampler="sobol", min_bounces=0, passes=()):
        """Samples per pixel, seed, total bounce limit, minimum bounces before Russian roulette (at or
        above max_bounces disables it, as in Cycles), pixel filter (box, gaussian,
        blackman_harris), output size and sampler mode (sobol, or independent where a test needs
        the exact variance of the mean, 0008 d3). `passes` names the extra film passes (0008 d10)."""
        a = self.abi
        filters = {"box": a.AR_FILTER_BOX, "gaussian": a.AR_FILTER_GAUSSIAN, "blackman_harris": a.AR_FILTER_BLACKMAN_HARRIS}
        if pixel_filter not in filters:
            raise ValueError(f"unknown pixel filter '{pixel_filter}'")
        samplers = {"sobol": a.AR_SAMPLER_SOBOL, "independent": a.AR_SAMPLER_INDEPENDENT}
        if sampler not in samplers:
            raise ValueError(f"unknown sampler mode '{sampler}'")
        s = a.ar_settings()
        s.samples = s.viewport_samples = spp
        s.output_width, s.output_height, s.resolution_percent = width, height, 100
        s.max_bounces, s.min_bounces, s.seed = max_bounces, min_bounces, seed
        s.pixel_filter, s.pixel_filter_width, s.film_exposure = filters[pixel_filter], filter_width, 1.0
        s.sampler_mode = samplers[sampler]
        names = (a.ar_name * max(1, len(passes)))()
        for i, p in enumerate(passes):
            raw = p.encode()
            if len(raw) >= 64:
                raise ValueError(f"pass name '{p}' is longer than 63 bytes")
            names[i].text[:len(raw) + 1] = list(raw) + [0]
        s.passes, s.pass_count = ctypes.cast(names, ctypes.POINTER(a.ar_name)), len(passes)
        self._call(self.lib.ar_edit_set_settings(self.edit_h, ctypes.byref(s)), "ar_edit_set_settings")
        self.size = (width, height)
        self._desc["settings"] = {
            "spp": spp, "width": width, "height": height, "seed": seed, "max_bounces": max_bounces,
            "min_bounces": min_bounces, "pixel_filter": pixel_filter, "filter_width": float(filter_width),
            "sampler": sampler, "passes": list(passes)}

    # -- end ----------------------------------------------------------------------------------------
    def finish(self):
        """Commits the edit, creates the one session at the camera's view and starts it, then
        destroys every handle so the recording is complete (the CLI replays it)."""
        a, lib = self.abi, self.lib
        if self._camera is None or self.size is None:
            raise AbiError("finish() needs camera() and settings() first")
        gen = ctypes.c_uint64()
        self._call(lib.ar_edit_commit(self.edit_h, ctypes.byref(gen)), "ar_edit_commit")
        w, h = self.size
        sd = a.ar_session_desc(a.AR_SESSION_MODE_FINAL, w, h, a.AR_DENOISER_NONE)
        self._call(lib.ar_session_create(self.scene_h, ctypes.byref(sd), ctypes.byref(self.session_h)), "ar_session_create")
        v = a.ar_view_desc()
        ctypes.memmove(ctypes.byref(v.camera), ctypes.byref(self._camera), ctypes.sizeof(a.ar_camera_desc))
        v.region_width, v.region_height, v.render_scale = w, h, 1.0
        self._call(lib.ar_session_set_view(self.session_h, ctypes.byref(v)), "ar_session_set_view")
        self._call(lib.ar_session_start(self.session_h), "ar_session_start")
        self._teardown()
        self._finished = True
        os.replace(self._partial, self.path)
        if self.description_path:
            os.makedirs(os.path.dirname(self.description_path), exist_ok=True)
            with open(self.description_path, "w", encoding="utf-8") as f:
                json.dump(self._desc, f)
