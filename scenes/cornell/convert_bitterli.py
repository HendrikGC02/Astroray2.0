# SPDX-License-Identifier: Apache-2.0
"""Reads Bitterli's Cornell box (CC0) from the fetched archive and returns its shapes, Lambertian
albedos, light rectangle and emission, camera included, in Astroray's frame (metres, +z up).
The archive holds no mesh files: scene_v3.xml (Mitsuba 3) places Mitsuba's unit `rectangle`
([-1, 1]^2 in xy, normal +z) and unit `cube` ([-1, 1]^3) with a 4x4 `to_world` each, so the
"meshes" are those primitives transformed. The scene is y-up with the camera on +z looking down -z;
the conversion is the proper rotation (x, y, z) -> (x, -z, y), so the camera ends up on -y looking
along +y. Units are the scene's own, used as metres (the box is 2 wide).
Usage as a module: load(path) with path the zip of `assets/manifest.toml` (cornell_box) or the folder
it extracts to. Anything the converter does not understand raises (CLAUDE.md rule 5).
Ref: assets/_cache/cornell_box/cornell-box.zip, scene_v3.xml; Mitsuba 3 documentation, shapes
`rectangle` and `cube` (normal of a rectangle = the transformed +z, the cube spans [-1, 1]^3),
bsdf `twosided`/`diffuse` (reflectance), emitter `area` (radiance), sensor `perspective` (fov
along x by default). Rendering resources: Bitterli, B. (2016), CC0 (assets/manifest.toml).
"""
import io
import os
import re
import xml.etree.ElementTree as ET
import zipfile
from dataclasses import dataclass, field

import numpy as np

SCENE_FILE = "scene_v3.xml"
# y-up (Mitsuba) to z-up (Blender): rotation about x by +90 degrees, determinant +1.
Y_UP_TO_Z_UP = np.array([[1.0, 0.0, 0.0], [0.0, 0.0, -1.0], [0.0, 1.0, 0.0]])


class ConvertError(ValueError):
    pass


@dataclass
class Shape:
    name: str
    kind: str             # "rectangle" or "cube"
    to_world: np.ndarray  # 4x4 in the converted frame (z up)
    albedo: tuple         # Lambertian reflectance (RGB), from the twosided diffuse bsdf


@dataclass
class Light:
    centre: np.ndarray    # (3,) metres
    x_axis: np.ndarray    # unit vectors of the rectangle's own x and y
    y_axis: np.ndarray
    normal: np.ndarray    # emission side
    size: tuple           # full extents along x_axis, y_axis (metres)
    radiance: tuple       # RGB, W m^-2 sr^-1 as authored


@dataclass
class Camera:
    position: np.ndarray
    target: np.ndarray    # position + view direction
    up: np.ndarray
    fov_deg: float        # along x
    width: int
    height: int


@dataclass
class Box:
    shapes: list = field(default_factory=list)
    light: Light = None
    camera: Camera = None


def _read_xml(path):
    if os.path.isdir(path):
        file = os.path.join(path, SCENE_FILE)
        if not os.path.exists(file):
            file = os.path.join(path, "cornell-box", SCENE_FILE)
        with open(file, "rb") as f:
            return f.read()
    with zipfile.ZipFile(path) as z:
        names = [n for n in z.namelist() if n.endswith("/" + SCENE_FILE) or n == SCENE_FILE]
        if len(names) != 1:
            raise ConvertError(f"{path}: expected one {SCENE_FILE}, found {names}")
        return z.read(names[0])


def _defaults(root):
    return {d.get("name"): d.get("value") for d in root.findall("default")}


def _sub(text, defaults):
    """Mitsuba's $name substitution for <default> values."""
    def repl(m):
        if m.group(1) not in defaults:
            raise ConvertError(f"scene_v3.xml uses ${m.group(1)} with no <default>")
        return defaults[m.group(1)]
    return re.sub(r"\$(\w+)", repl, text)


def _matrix(elem):
    m = elem.find("matrix")
    if m is None:
        raise ConvertError("a transform without a <matrix>")
    v = np.array([float(x) for x in m.get("value").split()])
    if v.size != 16:
        raise ConvertError("a matrix needs 16 numbers")
    return v.reshape(4, 4)


def _to_z_up(m):
    r = np.eye(4)
    r[:3, :3] = Y_UP_TO_Z_UP
    return r @ m


def _rgb(elem, name):
    e = next((c for c in elem if c.tag == "rgb" and c.get("name") == name), None)
    if e is None:
        raise ConvertError(f"missing <rgb name={name!r}>")
    v = tuple(float(x) for x in e.get("value").replace(",", " ").split())
    if len(v) != 3:
        raise ConvertError(f"<rgb name={name!r}> needs 3 numbers")
    return v


def _albedo(bsdf):
    if bsdf.get("type") != "twosided":
        raise ConvertError(f"bsdf {bsdf.get('id')}: type {bsdf.get('type')!r} is not 'twosided'")
    inner = bsdf.findall("bsdf")
    if len(inner) != 1 or inner[0].get("type") != "diffuse":
        raise ConvertError(f"bsdf {bsdf.get('id')}: the twosided bsdf must wrap one 'diffuse'")
    return _rgb(inner[0], "reflectance")


def load(path):
    """The Box of the archive (or extracted folder) at `path`."""
    if not os.path.exists(path):
        raise ConvertError(f"{path} is missing: run tools/fetch.ps1 assets")
    root = ET.fromstring(_read_xml(path))
    d = _defaults(root)
    bsdfs = {b.get("id"): b for b in root.findall("bsdf")}
    box = Box()

    sensor = root.find("sensor")
    if sensor is None or sensor.get("type") != "perspective":
        raise ConvertError("expected one perspective sensor")
    fov = float(next(c for c in sensor if c.tag == "float" and c.get("name") == "fov").get("value"))
    film = sensor.find("film")
    wh = {c.get("name"): int(_sub(c.get("value"), d)) for c in film.findall("integer")}
    if wh["width"] != wh["height"]:
        raise ConvertError("a non-square film needs the sensor fit worked out; the Cornell box is square")
    cam = _to_z_up(_matrix(sensor.find("transform")))
    box.camera = Camera(position=cam[:3, 3], target=cam[:3, 3] + cam[:3, 2] / np.linalg.norm(cam[:3, 2]),
                        up=cam[:3, 1] / np.linalg.norm(cam[:3, 1]), fov_deg=fov, width=wh["width"], height=wh["height"])

    for sh in root.findall("shape"):
        kind = sh.get("type")
        if kind not in ("rectangle", "cube"):
            raise ConvertError(f"shape {sh.get('id')}: type {kind!r} is not 'rectangle' or 'cube'")
        m = _to_z_up(_matrix(sh.find("transform")))
        ref = sh.find("ref")
        if ref is None or ref.get("id") not in bsdfs:
            raise ConvertError(f"shape {sh.get('id')}: no bsdf reference")
        emitter = sh.find("emitter")
        if emitter is None:
            box.shapes.append(Shape(sh.get("id"), kind, m, _albedo(bsdfs[ref.get("id")])))
            continue
        if kind != "rectangle" or emitter.get("type") != "area" or box.light is not None:
            raise ConvertError(f"shape {sh.get('id')}: the one emitter must be an area emitter on a rectangle")
        if _albedo(bsdfs[ref.get("id")]) != (0.0, 0.0, 0.0):
            raise ConvertError("the light rectangle must be a black diffuse (it only emits)")
        sx, sy = np.linalg.norm(m[:3, 0]), np.linalg.norm(m[:3, 1])
        xa, ya = m[:3, 0] / sx, m[:3, 1] / sy
        n = np.cross(xa, ya)  # the rectangle's +z in a right-handed frame; the matrix's z column agrees (checked)
        if np.dot(n, m[:3, 2]) <= 0:
            raise ConvertError("the light's frame is left-handed; its emission side needs the z column")
        box.light = Light(m[:3, 3], xa, ya, n, (2.0 * sx, 2.0 * sy), _rgb(emitter, "radiance"))
    if box.light is None or not box.shapes:
        raise ConvertError("the scene needs shapes and one light")
    return box


def _tri(quads):
    """Quads (4 corner indices counter-clockwise) to triangles."""
    return [t for q in quads for t in ((q[0], q[1], q[2]), (q[0], q[2], q[3]))]


def shape_mesh(shape, inward_point):
    """(positions (V,3), corner_vert (C,), normals (C,3)) of a shape in metres, triangles
    counter-clockwise seen from the side the normals point to. A rectangle faces `inward_point`
    (the bsdf is two-sided, so its side is ours to choose; the interior is the useful one); a cube
    faces outward."""
    m = shape.to_world
    lin = m[:3, :3]
    normal_matrix = np.linalg.inv(lin).T  # normals transform with the inverse transpose
    flip_winding = np.linalg.det(lin) < 0  # a mirroring transform reverses the winding
    if shape.kind == "rectangle":
        local = np.array([[-1, -1, 0], [1, -1, 0], [1, 1, 0], [-1, 1, 0]], float)
        quads, nrm = [(0, 1, 2, 3)], [np.array([0.0, 0.0, 1.0])]
    else:
        local = np.array([[x, y, z] for z in (-1, 1) for y in (-1, 1) for x in (-1, 1)], float)
        # Same face table as scenes/lib/meshes.py box(): outward normal, corners counter-clockwise from outside.
        table = [((1, 0, 0), (1, 3, 7, 5)), ((-1, 0, 0), (0, 4, 6, 2)), ((0, 1, 0), (2, 6, 7, 3)),
                 ((0, -1, 0), (0, 1, 5, 4)), ((0, 0, 1), (4, 5, 7, 6)), ((0, 0, -1), (0, 2, 3, 1))]
        quads, nrm = [q for _, q in table], [np.array(n, float) for n, _ in table]
    pos = local @ lin.T + m[:3, 3]
    out_pos, out_n = [], []
    for q, n_local in zip(quads, nrm):
        q = tuple(reversed(q)) if flip_winding else tuple(q)
        geo = np.cross(pos[q[1]] - pos[q[0]], pos[q[2]] - pos[q[0]])
        geo /= np.linalg.norm(geo)
        if shape.kind == "cube":
            n = normal_matrix @ n_local
            n /= np.linalg.norm(n)
            if np.dot(geo, n) <= 0:
                raise ConvertError(f"{shape.name}: a cube face winds against its outward normal")
        else:
            if np.dot(geo, inward_point - pos[list(q)].mean(axis=0)) < 0:
                q = tuple(reversed(q))
                geo = -geo
            n = geo
        for t in _tri([q]):
            out_pos.append(t)
            out_n.extend([n] * 3)
    corner_vert = np.array(out_pos, np.int32).reshape(-1)
    return pos.astype(np.float32), corner_vert, np.array(out_n, np.float32)
