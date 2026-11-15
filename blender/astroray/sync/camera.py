# SPDX-License-Identifier: GPL-3.0-or-later
"""Camera and viewport view to ar_camera_desc / ar_view_desc (0007 d20, d49; V18).

Ref: Cycles intern/cycles/blender/camera.cpp (Apache-2.0, projects.blender.org/blender/blender,
read 2026-11-16) for what each parameter means:
 - blender_camera_from_object: clip_start/clip_end, lens, sensor_x/sensor_y, shift, ortho_scale,
   panorama parameters, DOF (fstop clamped at 1e-5, blades, rotation, ratio; the focus distance is
   `focus_distance`, or with a focus object the distance along the camera's view axis).
 - blender_camera_viewplane and blender_camera_sync: sensor fit (AUTO uses the larger side, with
   the sensor width; HORIZONTAL the width; VERTICAL the height), shift in units of the fitted
   side, field of view 2 atan(sensor / (2 lens aspect)).
 - blender_camera_from_view: the viewport's own clip, camera view and its zoom/offset.
Blender's `Object.calc_matrix_camera` (the F12 projection) and `RegionView3D.window_matrix` (the
viewport's) implement those same formulas, so `projection_inverse` is their inverse and nothing is
re-derived here (0007 appendix: "taken from Blender"). Every matrix is sent row-major.
Scene-origin translation is folded into cam_to_world (0003 d26).
Unsupported camera types and DOF are filled in and reported by the engine (053); the addon adds
a line only for what the engine cannot see (motion blur).
"""
import ctypes

import numpy as np

# include/astroray/astroray.h AR_CAMERA_*
PERSP, ORTHO, PANO = 0, 1, 2
_TYPES = {"PERSP": PERSP, "ORTHO": ORTHO, "PANO": PANO}
# The ABI's panorama_type is this list's index (the engine reports PANO unsupported; the numbers
# only have to be stable until a panorama camera exists).
PANORAMA_TYPES = ("EQUIRECTANGULAR", "EQUIANGULAR_CUBEMAP_FACE", "MIRRORBALL", "FISHEYE_EQUIDISTANT",
                  "FISHEYE_EQUISOLID", "FISHEYE_LENS_POLYNOMIAL", "CENTRAL_CYLINDRICAL")


def _rows3x4(matrix_world, origin):
    m = np.asarray(matrix_world, np.float64)[:3, :4].copy()
    m[:, 3] -= origin
    return tuple(m.reshape(12))


def _flat(m):
    return tuple(np.asarray(m, np.float64).reshape(16))


def _focus_distance(cam, ob):
    dof = cam.dof
    if dof.focus_object is None:
        return dof.focus_distance
    m = np.asarray(ob.matrix_world, np.float64)
    forward = -m[:3, 2] / np.linalg.norm(m[:3, 2])
    target = np.asarray(dof.focus_object.matrix_world, np.float64)[:3, 3]
    return abs(float(np.dot(forward, target - m[:3, 3])))


def _camera_fields(cam, ob, scene, lines):
    """Everything of ar_camera_desc except the two matrices."""
    ctype = _TYPES.get(cam.type)
    if ctype is None:
        ctype = PANO
        lines.append(f"camera '{ob.name}': type {cam.type} is not supported; reported as panoramic")
    f = {
        "type": ctype, "near_clip": cam.clip_start, "far_clip": cam.clip_end,
        "dof_enabled": int(cam.dof.use_dof), "fstop": max(cam.dof.aperture_fstop, 1e-5),
        "focus_distance": _focus_distance(cam, ob) if cam.dof.use_dof else 0.0,
        "blades": cam.dof.aperture_blades, "aperture_rotation": cam.dof.aperture_rotation,
        "aperture_ratio": cam.dof.aperture_ratio,
        "panorama_type": PANORAMA_TYPES.index(cam.panorama_type),
        "fisheye_fov": cam.fisheye_fov, "fisheye_lens": cam.fisheye_lens,
        "latitude_min": cam.latitude_min, "latitude_max": cam.latitude_max,
        "longitude_min": cam.longitude_min, "longitude_max": cam.longitude_max,
        "shutter_open": 0.0, "shutter_close": 0.0,
    }
    if scene.render.use_motion_blur:
        lines.append("motion blur lands in M8 and is ignored")
    return f


def final(depsgraph, scene, origin=(0.0, 0.0, 0.0)):
    """The F12 camera: (fields, lines). Raises when the scene has no camera."""
    ob = scene.camera
    if ob is None or ob.type != "CAMERA":
        raise RuntimeError("astroray sync: the scene has no active camera object")
    ob = ob.evaluated_get(depsgraph)
    r = scene.render
    w = max(1, int(r.resolution_x * r.resolution_percentage / 100))
    h = max(1, int(r.resolution_y * r.resolution_percentage / 100))
    lines = []
    f = _camera_fields(ob.data, ob, scene, lines)
    proj = ob.calc_matrix_camera(depsgraph, x=w, y=h, scale_x=r.pixel_aspect_x, scale_y=r.pixel_aspect_y)
    f["cam_to_world"] = _rows3x4(ob.matrix_world, np.asarray(origin, np.float64))
    f["projection_inverse"] = _flat(np.linalg.inv(np.asarray(proj, np.float64)))
    return f, lines


def viewport(scene, space, rv3d, depsgraph=None, origin=(0.0, 0.0, 0.0)):
    """The viewport's camera for a SpaceView3D and RegionView3D: (fields, lines).

    `rv3d.window_matrix` and `view_matrix` already contain camera view, zoom, offset and the
    viewport's own lens, so they are the whole projection.
    """
    lines = []
    ob = space.camera if space.use_local_camera else scene.camera
    if rv3d.view_perspective == "CAMERA" and ob is not None and ob.type == "CAMERA":
        if depsgraph is not None:
            ob = ob.evaluated_get(depsgraph)
        f = _camera_fields(ob.data, ob, scene, lines)
    else:
        f = _camera_fields(_ViewportLens(space), _ViewportLens.holder, scene, lines)
        f["type"] = PERSP if rv3d.is_perspective else ORTHO
    view = np.asarray(rv3d.view_matrix, np.float64)
    f["cam_to_world"] = _rows3x4(np.linalg.inv(view), np.asarray(origin, np.float64))
    f["projection_inverse"] = _flat(np.linalg.inv(np.asarray(rv3d.window_matrix, np.float64)))
    return f, lines


class _NoDof:
    use_dof = False
    focus_object = None
    focus_distance = 0.0
    aperture_fstop = 2.8
    aperture_blades = 0
    aperture_rotation = 0.0
    aperture_ratio = 1.0


class _ViewportLens:
    """The viewport has a lens and clip range but no camera datablock: DOF off, no panorama."""

    holder = type("Holder", (), {"name": "viewport"})()
    dof = _NoDof()
    type = "PERSP"
    panorama_type = PANORAMA_TYPES[0]
    fisheye_fov = fisheye_lens = 0.0
    latitude_min = latitude_max = longitude_min = longitude_max = 0.0

    def __init__(self, space):
        self.clip_start, self.clip_end = space.clip_start, space.clip_end


def view_hash(rv3d, region_width, region_height, camera_view, border=None):
    """What set_view must see change (0007 d49): view matrix, projection, region size, whether the
    camera view is active and the border. Orbit and pan change only the matrices."""
    return hash((tuple(np.asarray(rv3d.view_matrix, np.float64).reshape(16)),
                 tuple(np.asarray(rv3d.window_matrix, np.float64).reshape(16)),
                 int(region_width), int(region_height), bool(camera_view),
                 tuple(border) if border else None))


def set_camera(edit, abi, f):
    edit.call("ar_edit_set_camera", ctypes.byref(camera_desc(abi, f)))


def camera_desc(abi, f):
    c = abi.ar_camera_desc()
    c.cam_to_world = abi.ar_xform((ctypes.c_float * 12)(*f["cam_to_world"]))
    c.projection_inverse = (ctypes.c_float * 16)(*f["projection_inverse"])
    for k in ("type", "near_clip", "far_clip", "dof_enabled", "fstop", "focus_distance", "blades",
              "aperture_rotation", "aperture_ratio", "panorama_type", "fisheye_fov", "fisheye_lens",
              "latitude_min", "latitude_max", "longitude_min", "longitude_max", "shutter_open",
              "shutter_close"):
        setattr(c, k, f[k])
    c.motion_steps = 0  # one transform: cam_to_world; set_view refuses a count with no motion_xf
    return c


def view_desc(abi, f, region_width, region_height, render_scale=1.0, border=None):
    v = abi.ar_view_desc()
    c = camera_desc(abi, f)
    ctypes.memmove(ctypes.byref(v.camera), ctypes.byref(c), ctypes.sizeof(abi.ar_camera_desc))
    v.region_width, v.region_height, v.render_scale = region_width, region_height, render_scale
    if border:
        v.border_enabled = 1
        v.border_min_x, v.border_min_y, v.border_max_x, v.border_max_y = border
    return v


class ViewSync:
    """Calls ar_session_set_view only when the view hash changed."""

    def __init__(self):
        self._last = None

    def update(self, set_view, abi, session, f, h, region_width, region_height, render_scale=1.0,
               border=None):
        """set_view(session, byref(desc)) is the bound ar_session_set_view; returns True if sent."""
        if h == self._last:
            return False
        v = view_desc(abi, f, region_width, region_height, render_scale, border)
        st = set_view(session, ctypes.byref(v))
        if st:
            raise RuntimeError(f"astroray sync: ar_session_set_view returned status {st}")
        self._last = h
        return True
