# SPDX-License-Identifier: GPL-3.0-or-later
"""Blender material nodes to host-neutral op graphs (0005 items 29, 40, 42; 0007 d8).

The addon walks `material.inline_shader_nodes()` (groups already inlined), lowers the M1a node set
into the typed ops of ops.def through one emitter, and sends the graph with `ar_edit_set_graph`
(SURFACE) and `ar_edit_set_material`. Anything without a lowering becomes an UNSUPPORTED op whose
string is the node type or "<node>.<socket>"; the core compiler turns every such op into one
NODE_UNSUPPORTED report entry and a diffuse magenta material, so nothing drops silently.

Ref: Blender source/blender/nodes/shader/nodes/node_shader_bsdf_principled.cc (GPL, Blender 5.3):
socket names and default values of the Principled BSDF; node_shader_tex_image.cc: interpolation,
extension and projection settings; node_shader_background.cc: Color and Strength.
Ref: docs/decisions/0005-material-model.md item 42 (Specular IOR Level L is specular_weight = 2L)
and the compiler contract: constant colours go to STORE_SLOT as F32X3 (the slot
lifts them), RGB_LIFT is used only on the texture path because it never folds, and one value never
feeds two EMIT_LE nodes, so constants are never shared between emitters.
Ref: src/host/material/graph.cpp (parameter counts, socket order), vm/ops/store_slot.h (RecordSlot).
Ref: Blender source/blender/nodes/shader/nodes/node_shader_emission.cc (Color, Strength) and
node_shader_blackbody.cc (Temperature, Color output); Cycles kernel/svm/convert.h NODE_CONVERT_CF
(a colour linked to a float socket becomes its scene-linear luminance, the COLOR_TO_F32 op).

No bpy import and no module state: `op_id` (the generated `_ops.OP_ID`) and the key map come from
the caller; the generated `_ops` is the package's staged module, which tests register as
`ar_addon._ops`.
"""
import ctypes
import time

from .. import _ops  # staged by the build, never committed
from ..sync import keys

# Graph targets, report codes and emission sampling come from the generated binding (`abi`,
# passed in by the callers). Record-slot ids, TEX_IMAGE mode bits and spectrum semantics come from
# the generated `_ops` (tools/opsgen.py parses them out of the kernel headers), never copied here.
SLOT_SPECULAR_WEIGHT = _ops.RECORD_SLOT["specular_weight"]
SLOT_SPECULAR_ROUGHNESS = _ops.RECORD_SLOT["specular_roughness"]
SLOT_BASE_WEIGHT = _ops.RECORD_SLOT["base_weight"]
SLOT_GEOMETRY_OPACITY = _ops.RECORD_SLOT["geometry_opacity"]
SLOT_BASE_COLOR = _ops.RECORD_SLOT["base_color"]
SEM_REFLECTANCE = _ops.SEMANTIC["REFLECTANCE"]
TEX_LINEAR, TEX_EXTEND = _ops.TEX_MODE["LINEAR"], _ops.TEX_MODE["EXTEND"]
# Blender's Cycles property group `material.cycles.emission_sampling` items; the ABI has
# AR_EMISSION_SAMPLING_<name> for each (a test checks the binding). Another value is reported.
EMISSION_SAMPLING_NAMES = ("NONE", "AUTO", "FRONT", "BACK", "FRONT_BACK")
_IMAGE_SOURCES = frozenset(("FILE", "GENERATED"))

# Principled inputs lowered by this module; every other one must be at its default and unlinked.
_LOWERED_INPUTS = frozenset(("Base Color", "Roughness", "Specular IOR Level", "Emission Color",
                             "Emission Strength", "Alpha"))
# Blender 5.3 defaults (probed from a new node; a test compares this table with a fresh node, so a
# Blender update that moves one fails there). None: only a link matters (hidden vector sockets).
_DEFAULTS = {
    "Metallic": 0.0, "IOR": 1.5, "Thin Wall": False, "Normal": None,
    "Diffuse Roughness": 0.0, "Subsurface Weight": 0.0, "Subsurface Radius": (1.0, 0.2, 0.1),
    "Subsurface Scale": 0.005, "Subsurface IOR": 1.4, "Subsurface Anisotropy": 0.0,
    "Specular Tint": (1.0, 1.0, 1.0, 1.0), "Anisotropic": 0.0, "Anisotropic Rotation": 0.0,
    "Tangent": None, "Transmission Weight": 0.0, "Transmission Dispersion Scale": 0.0,
    "Transmission Dispersion Abbe Number": 20.0, "Coat Weight": 0.0, "Coat Roughness": 0.03,
    "Coat IOR": 1.5, "Coat Tint": (1.0, 1.0, 1.0, 1.0), "Coat Normal": None, "Sheen Weight": 0.0,
    "Sheen Roughness": 0.5, "Sheen Tint": (1.0, 1.0, 1.0, 1.0), "Thin Film Thickness": 0.0,
    "Thin Film IOR": 1.33,
}
# Nodes that carry no shading: skipped by the unsupported scan.
_STRUCTURAL = frozenset(("NodeFrame", "NodeReroute"))
_TARGETS_USED = frozenset(("ALL", "CYCLES"))


class GraphEmitter:
    """Builds the flat arrays of an ar_graph_desc: op nodes, links, parameter pools, strings."""

    def __init__(self, op_id):
        self._op = op_id
        self.nodes = []      # (op, f32_first, f32_count, i32_first, i32_count, string_first, string_count)
        self.links = []      # (from_node, from_socket, to_node, to_socket)
        self.f32 = []
        self.i32 = []
        self.strings = []
        self.emission_sampling = "AUTO"  # materials only
        self.inline_s = 0.0  # seconds spent in inline_shader_nodes() (M-8 measurement)

    def add(self, op, f32=(), i32=(), string=None):
        """Appends one op node and returns its index."""
        sf = len(self.strings)
        if string is not None:
            self.strings.append(string)
        self.nodes.append((self._op[op], len(self.f32), len(f32), len(self.i32), len(i32), sf,
                           0 if string is None else 1))
        self.f32.extend(f32)
        self.i32.extend(i32)
        return len(self.nodes) - 1

    def link(self, src, dst, socket):
        self.links.append((src, 0, dst, socket))

    def const_f32(self, v):
        return self.add("CONST_F32", f32=(float(v),))

    def const_rgb(self, rgb):
        return self.add("CONST_F32X3", f32=tuple(float(c) for c in rgb[:3]))

    def store(self, slot, src):
        n = self.add("STORE_SLOT", i32=(slot,))
        self.link(src, n, 0)

    def unsupported(self, label):
        self.add("UNSUPPORTED", string=label)

    def unsupported_count(self):
        return sum(1 for n in self.nodes if n[0] == self._op["UNSUPPORTED"])

    def to_desc(self, abi):
        """(ar_graph_desc, keepalive): the arrays stay referenced until the ABI call returns."""
        def arr(ctype, values):
            return (ctype * len(values))(*values)

        nodes = arr(abi.ar_graph_node, [abi.ar_graph_node(*n, 0) for n in self.nodes])
        links = arr(abi.ar_graph_link, [abi.ar_graph_link(*k) for k in self.links])
        f32, i32 = arr(ctypes.c_float, self.f32), arr(ctypes.c_int32, self.i32)
        pool = b"".join(s.encode("utf-8") + b"\0" for s in self.strings)
        sb = arr(ctypes.c_uint8, list(pool))
        d = abi.ar_graph_desc()
        d.node_count, d.link_count, d.f32_count, d.i32_count = len(nodes), len(links), len(f32), len(i32)
        d.string_bytes_count = len(pool)
        d.nodes = ctypes.cast(nodes, ctypes.POINTER(abi.ar_graph_node))
        d.links = ctypes.cast(links, ctypes.POINTER(abi.ar_graph_link))
        d.params_f32 = ctypes.cast(f32, ctypes.POINTER(ctypes.c_float))
        d.params_i32 = ctypes.cast(i32, ctypes.POINTER(ctypes.c_int32))
        d.string_bytes = ctypes.cast(sb, ctypes.POINTER(ctypes.c_uint8))
        return d, (nodes, links, f32, i32, sb)


def lowered_types():
    """Node types this module lowers; equal to the rows of lowering_table.LOWERING."""
    return frozenset(_LOWER_SHADER) | frozenset(_LOWER_VALUE) | frozenset(
        ("ShaderNodeOutputMaterial", "ShaderNodeOutputWorld", "ShaderNodeBackground",
         "ShaderNodeBlackbody"))


def scan_unsupported(em, tree):
    """One UNSUPPORTED op per node of the tree without a lowering, even if nothing links to it
    (0005 item 40: any node without a lowering makes the material magenta, and every one is
    reported, 0001 V15). Outputs that are not the active one are ignored like Cycles does."""
    known = lowered_types()
    for n in tree.nodes:
        if n.bl_idname in _STRUCTURAL or n.bl_idname in known:
            continue
        em.unsupported(n.bl_idname)


def active_output(tree, idname):
    """The active output of the given type that Cycles would use (target ALL or CYCLES)."""
    for n in tree.nodes:
        if n.bl_idname == idname and n.is_active_output and n.target in _TARGETS_USED:
            return n
    return None


def _close(a, b):
    if isinstance(b, tuple):
        return len(a) >= len(b) and all(abs(x - y) <= 1e-6 for x, y in zip(a, b))
    if isinstance(b, bool):
        return bool(a) == b
    return abs(a - b) <= 1e-6


def _link_of(sock):
    """The one live link feeding a socket, or None."""
    for lk in sock.links:
        if not lk.is_muted:
            return lk
    return None


# Lowerings of non-shader nodes, by output socket: f(em, node, keymap, op_id) -> emitter node.
def _uv(em, node, keymap, socket_name):
    if node.from_instancer:
        # The instancer's UV comes from another object's mesh; the ABI has no such context.
        em.unsupported(f"{node.bl_idname}.from_instancer")
        return None
    if node.bl_idname == "ShaderNodeUVMap" and node.uv_map:
        # Only the default UV set is addressable (the ABI carries UV set 0); a named set could be
        # any index, so it is not guessed.
        em.unsupported("UV Map.uv_map")
        return None
    if socket_name != "UV":
        em.unsupported(f"{node.bl_idname}.{socket_name}")
        return None
    return em.add("LOAD_CTX_UV", i32=(0,))


def _tex_image(em, node, keymap, socket_name):
    image = node.image
    if socket_name != "Color":
        em.unsupported(f"Image Texture.{socket_name}")
        return None
    if image is None:
        em.unsupported("Image Texture.image")
        return None
    if image.source not in _IMAGE_SOURCES:
        em.unsupported("Image Texture.source")  # TILED, SEQUENCE, MOVIE and VIEWER need 110's work
        return None
    if node.interpolation not in ("Linear", "Closest"):
        em.unsupported("Image Texture.interpolation")
        return None
    if node.extension not in ("REPEAT", "EXTEND"):
        em.unsupported("Image Texture.extension")
        return None
    if node.projection != "FLAT":
        em.unsupported("Image Texture.projection")
        return None
    vec = node.inputs["Vector"]
    lk = _link_of(vec)
    if lk is None:
        uv = em.add("LOAD_CTX_UV", i32=(0,))  # Cycles reads the default UV set for an open Vector
    else:
        uv = _value(em, lk, keymap, "Image Texture.Vector")
        if uv is None:
            return None
    mode = (TEX_LINEAR if node.interpolation == "Linear" else 0) | \
           (TEX_EXTEND if node.extension == "EXTEND" else 0)
    key = keys.image_key(keymap, image)
    if key > 0xFFFFFFFF:
        # TEX_IMAGE carries the low 32 bits of the u64 key (0007 appendix); KeyMap is a counter,
        # so a wider key means 2^32 datablocks, and truncating would alias two images.
        em.unsupported("Image Texture.image key")
        return None
    n = em.add("TEX_IMAGE", i32=(key, mode))
    em.link(uv, n, 0)
    return n


_LOWER_VALUE = {
    "ShaderNodeTexImage": _tex_image,
    "ShaderNodeTexCoord": _uv,
    "ShaderNodeUVMap": _uv,
}


def report_link(em, sock, label):
    """Reports a linked socket that has no lowering. A source node type without a lowering is
    already reported by scan_unsupported, so only a lowered type (a BSDF into a colour socket, a
    texture into a roughness) adds an entry here."""
    if _link_of(sock).from_node.bl_idname in lowered_types():
        em.unsupported(label)


def _value(em, link, keymap, label, convert=False):
    """Lowers the colour or vector value a link carries; None after reporting why not. `convert`: the
    caller applies the type conversion itself (colour to float), so differing socket types pass."""
    src = link.from_node
    fn = _LOWER_VALUE.get(src.bl_idname)
    if fn is None:
        if src.bl_idname in lowered_types():
            em.unsupported(label)
        return None
    if not convert and link.from_socket.type != link.to_socket.type:
        # Blender converts vector to colour silently; the engine lowers no conversions in M1a.
        em.unsupported(label)
        return None
    return fn(em, src, keymap, link.from_socket.name)


def _blackbody(em, node):
    """A Blackbody node as the BLACKBODY op (constant Temperature, scale 1: the compiler folds in the
    1 / Y(Planck(T)) display normalisation); None after reporting a linked Temperature."""
    temp = node.inputs["Temperature"]
    if _link_of(temp) is not None:
        report_link(em, temp, "Blackbody.Temperature")
        return None
    return em.add("BLACKBODY", f32=(float(temp.default_value), 1.0))


def _colour(em, sock, keymap, label, lift, emission=False):
    """A colour input as an F32X3 node (or an RGB_LIFT spectrum node when `lift`). `emission`: the
    consumer is an emission colour, the only place a Blackbody value may go (0005 item 36); anywhere
    else it is reported with the consumer named, never converted to a colour."""
    lk = _link_of(sock)
    if lk is None:
        return em.const_rgb(tuple(sock.default_value))
    if lk.from_node.bl_idname == "ShaderNodeBlackbody":
        if emission and not lift:
            return _blackbody(em, lk.from_node)
        em.unsupported(label)
        return None
    v = _value(em, lk, keymap, label)
    if v is None or not lift:
        return v
    n = em.add("RGB_LIFT", i32=(SEM_REFLECTANCE,))
    em.link(v, n, 0)
    return n


def _scalar(em, sock, label):
    """An unlinked scalar input; a linked one has no lowering here (see _float)."""
    if _link_of(sock) is not None:
        report_link(em, sock, label)
        return None
    return float(sock.default_value)


def _float(em, sock, keymap, label):
    """A float input as (emitter node, constant or None); (None, None) after reporting why not.
    A colour linked to it is converted to its luminance like Blender does (COLOR_TO_F32, 0005 item
    36). A vector (Blender averages it) and a float-typed source have no lowering and are reported
    by consumer."""
    lk = _link_of(sock)
    if lk is None:
        v = float(sock.default_value)
        return em.const_f32(v), v
    if lk.from_socket.type != "RGBA":
        report_link(em, sock, label)
        return None, None
    v = _value(em, lk, keymap, label, convert=True)
    if v is None:
        return None, None
    n = em.add("COLOR_TO_F32")
    em.link(v, n, 0)
    return n, None


def _principled(em, node, keymap):
    ins = node.inputs
    # `distribution` (GGX or Multiscatter GGX) and `subsurface_method` are not checked: 0005 item
    # 42 maps Distribution to "compensation always on, ignored (info)", and the subsurface method
    # only matters when Subsurface Weight is non-default, which is reported by name below.
    for s in ins:
        name = s.name
        if name in _LOWERED_INPUTS or name == "Weight" or not s.enabled:
            continue
        label = f"Principled BSDF.{name}"
        if name not in _DEFAULTS:
            em.unsupported(label)  # an input of a newer Blender: never guessed
        elif _link_of(s) is not None:
            report_link(em, s, label)
        elif _DEFAULTS[name] is not None and not _close(s.default_value, _DEFAULTS[name]):
            em.unsupported(label)
    # Compiler contract: constants go straight into STORE_SLOT, lifted only on the texture path.
    base = _colour(em, ins["Base Color"], keymap, "Principled BSDF.Base Color", lift=True)
    if base is not None:
        em.store(SLOT_BASE_COLOR, base)
    rough, _ = _float(em, ins["Roughness"], keymap, "Principled BSDF.Roughness")
    if rough is not None:
        em.store(SLOT_SPECULAR_ROUGHNESS, rough)
    level_sock = ins["Specular IOR Level"]
    if _link_of(level_sock) is None:
        em.store(SLOT_SPECULAR_WEIGHT, em.const_f32(2.0 * float(level_sock.default_value)))  # 0005 item 42
    else:
        # specular_weight = 2L per point: the level (a colour becomes its luminance) times the constant 2.
        level, _ = _float(em, level_sock, keymap, "Principled BSDF.Specular IOR Level")
        if level is not None:
            twice = em.add("MUL_F32")
            em.link(level, twice, 0)
            em.link(em.const_f32(2.0), twice, 1)
            em.store(SLOT_SPECULAR_WEIGHT, twice)
    alpha = ins["Alpha"]
    if _link_of(alpha) is not None or alpha.default_value != 1.0:
        a, _ = _float(em, alpha, keymap, "Principled BSDF.Alpha")
        if a is not None:
            em.store(SLOT_GEOMETRY_OPACITY, a)  # 1.0 stores nothing: the opacity stays constant 1
    _emit(em, ins["Emission Color"], ins["Emission Strength"], keymap, "Principled BSDF")


def _emit(em, color_sock, strength_sock, keymap, who):
    """One EMIT_LE from an emission colour and a strength input; nothing for a constant strength 0."""
    if _link_of(strength_sock) is None and strength_sock.default_value == 0.0:
        return False
    strength, const = _float(em, strength_sock, keymap, f"{who}.{strength_sock.name}")
    if strength is None or const == 0.0:
        return False
    col = _colour(em, color_sock, keymap, f"{who}.{color_sock.name}", lift=False, emission=True)
    if col is None:
        return False
    e = em.add("EMIT_LE")
    em.link(col, e, 0)
    em.link(strength, e, 1)
    return True


def _emission(em, node, keymap):
    """An Emission node: only emits, so the base and specular lobes are switched off."""
    em.store(SLOT_BASE_WEIGHT, em.const_f32(0.0))
    em.store(SLOT_SPECULAR_WEIGHT, em.const_f32(0.0))
    _emit(em, node.inputs["Color"], node.inputs["Strength"], keymap, "Emission")


_LOWER_SHADER = {"ShaderNodeBsdfPrincipled": _principled, "ShaderNodeEmission": _emission}


def _surface(em, tree, keymap, out_type, label):
    out = active_output(tree, out_type)
    if out is None:
        em.unsupported(f"{out_type} (no active output)")
        return None
    for s in out.inputs:
        if s.name != "Surface" and _link_of(s) is not None:
            report_link(em, s, f"{label}.{s.name}")
    lk = _link_of(out.inputs["Surface"])
    return None if lk is None else lk.from_node


def _emission_sampling(em, material):
    """The material's Emission Sampling name; one the ABI enum lacks is reported (Cycles'
    NONE, AUTO, FRONT, BACK and FRONT_BACK all exist there)."""
    name = material.cycles.emission_sampling
    if name not in EMISSION_SAMPLING_NAMES:
        em.unsupported(f"Material.emission_sampling {name}")
        return "AUTO"
    return name


def translate_material(material, keymap, op_id):
    """The SURFACE graph of one evaluated material, as a GraphEmitter."""
    em = GraphEmitter(op_id)
    # The tree dies with the InlineShaderNodes object that owns it: keep `inlined` alive.
    t0 = time.perf_counter()
    inlined = material.inline_shader_nodes()
    em.inline_s = time.perf_counter() - t0
    tree = inlined.node_tree if inlined else None
    if tree is None:
        em.unsupported("Material without nodes")
        return em
    scan_unsupported(em, tree)
    em.emission_sampling = _emission_sampling(em, material)
    src = _surface(em, tree, keymap, "ShaderNodeOutputMaterial", "Material Output")
    if src is None:
        return em
    fn = _LOWER_SHADER.get(src.bl_idname)
    if fn is not None:
        fn(em, src, keymap)
    elif src.bl_idname in lowered_types():
        em.unsupported("Material Output.Surface")  # a lowered node that is not a shader
    return em


def translate_world(world, keymap, op_id):
    """The WORLD graph: constant Background colour times strength (0005 item 32, M1a)."""
    em = GraphEmitter(op_id)
    inlined = world.inline_shader_nodes()  # owns the tree
    tree = inlined.node_tree if inlined else None
    if tree is None:
        color, strength = tuple(world.color), 1.0
    else:
        scan_unsupported(em, tree)
        src = _surface(em, tree, keymap, "ShaderNodeOutputWorld", "World Output")
        if src is None:
            return em
        if src.bl_idname != "ShaderNodeBackground":
            if src.bl_idname in lowered_types():
                em.unsupported("World Output.Surface")
            return em
        strength = _scalar(em, src.inputs["Strength"], "Background.Strength")
        csock = src.inputs["Color"]
        if _link_of(csock) is not None:  # a constant world only; an environment is M1b
            report_link(em, csock, "Background.Color")
            return em
        color = tuple(csock.default_value)
        if strength is None:
            return em
    e = em.add("EMIT_LE")
    em.link(em.const_rgb(color), e, 0)
    em.link(em.const_f32(strength), e, 1)
    return em


class TranslationCache:
    """Translated graphs keyed by the original material's session_uid (0005 item 29).

    A material is re-translated only when classification marks it or a node tree in its raw tree
    (its own, or a group it uses) changed; `deps` records those tree uids at translation time.
    """

    def __init__(self):
        self._graphs = {}   # material uid -> GraphEmitter
        self._deps = {}     # material uid -> frozenset of node tree uids
        self.translations = 0

    @staticmethod
    def tree_uids(material):
        """Original session_uids of the material's raw tree and every group below it."""
        out, stack = set(), []
        if material.node_tree is not None:
            stack.append(material.node_tree)
        while stack:
            t = stack.pop().original
            if t.session_uid in out:
                continue
            out.add(t.session_uid)
            stack.extend(n.node_tree for n in t.nodes
                         if n.bl_idname == "ShaderNodeGroup" and n.node_tree is not None)
        return frozenset(out)

    def invalidate(self, actions):
        """Drops what the MATERIALS actions of one classification batch touched."""
        for a in actions:
            if a.id_type == "MATERIAL":
                self._graphs.pop(a.uid, None)
                self._deps.pop(a.uid, None)
            elif a.id_type == "NODETREE":
                for uid in [u for u, d in self._deps.items() if a.uid in d]:
                    del self._graphs[uid], self._deps[uid]

    def forget_all(self):
        self._graphs.clear()
        self._deps.clear()

    def get(self, material, keymap, op_id):
        """The translation of an evaluated material, translating on a miss."""
        uid = material.original.session_uid
        em = self._graphs.get(uid)
        if em is None:
            em = translate_material(material, keymap, op_id)
            self._graphs[uid] = em
            self._deps[uid] = self.tree_uids(material.original)
            self.translations += 1
        return em


def set_material(edit, abi, keymap, uid, em, name=""):
    """Sends the SURFACE graph and the material that names it; returns the material's ar_key."""
    material_key = keymap.key(("material", uid))
    graph_key = keymap.key(("graph", "SURFACE", uid))
    desc, keep = em.to_desc(abi)
    edit.call("ar_edit_set_graph", graph_key, abi.AR_TARGET_SURFACE, ctypes.byref(desc))
    m = abi.ar_material_desc()
    m.surface_graph = graph_key
    m.emission_sampling = getattr(abi, "AR_EMISSION_SAMPLING_" + em.emission_sampling)
    edit.call("ar_edit_set_material", material_key, ctypes.byref(m))
    del keep
    return material_key
