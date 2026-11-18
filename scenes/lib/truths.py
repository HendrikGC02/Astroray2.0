# SPDX-License-Identifier: Apache-2.0
"""Closed-form truths of the analytic scenes (0009 d16, d20), named by meta.toml's [truth] formula.

A truth function takes the [truth] table and the image size and returns a list of checks, each
(name, weights, truth, kind): the measured channel is weights . (display R, G, B) per pixel and
truth is an (height, width) array in display units (working-space RGB, film exposure 1). kind
"pixels" tests every pixel, "image" tests the image mean; "pair_pixels" and "pair_image" compare
two renders instead (mode_agreement). Every check is one member of the
analytic family per device (tools/analytic.py).
Ref: docs/decisions/0009-build-test-process.md d16, d20; 0003 d11 (display Y, the unit anchor);
src/host/data_resource.h (observer.bin layout).
"""
import os
import struct

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OBSERVER_BIN = os.path.join(ROOT, "data", "spectral", "observer.bin")

# observer.bin layout from src/host/data_resource.h: magic[8], version, kind, hash[8], then (after
# the sampling words) the CMFs at byte 36, Y_n, K, the D65 SPD and M_ws (9 doubles, row-major).
_SAMPLES = 471
_CMF_AT = 36
_MWS_AT = _CMF_AT + 3 * 4 * _SAMPLES + 8 + 8 + 4 * _SAMPLES
_BYTES = _MWS_AT + 72


def working_luminance_row():
    """The row of M_ws^-1 that gives Y from working-space RGB: display Y = row . RGB, because
    working RGB = M_ws . XYZ (0003 d11). M_ws is read from the shipped observer.bin."""
    with open(OBSERVER_BIN, "rb") as f:
        raw = f.read()
    if len(raw) != _BYTES or raw[:6] != b"ARDATA":
        raise ValueError(f"{OBSERVER_BIN}: not the observer.bin layout of src/host/data_resource.h")
    m_ws = np.array(struct.unpack_from("<9d", raw, _MWS_AT)).reshape(3, 3)
    return np.linalg.inv(m_ws)[1]


def _emitted_rgb(t):
    colour = np.asarray(t["colour"], dtype=np.float64)
    if colour.shape != (3,):
        raise ValueError("truth colour must be an RGB triple")
    return colour * float(t["radiance"])


def emitter_radiance(t, width, height):
    """A5: a view-filling emitter through a pinhole; every pixel's display RGB is the emitted
    colour times radiance (0009 d20: pixel = L)."""
    rgb = _emitted_rgb(t)
    eye = np.eye(3)
    return [(n, eye[i], np.full((height, width), rgb[i]), "pixels") for i, n in enumerate("RGB")]


def display_luminance(t, width, height):
    """y_anchor: an RGB white emitter of strength L has display Y = L, the unit anchor (0003 d11,
    check 11: white of strength 1 gives Y = 1). The truth is that literal value; the measured Y is
    formed with the working space's luminance row, so an error in M_ws or the white point shows.
    Checked per pixel and, at the record's 1e-3 precision, as the image mean."""
    if list(t["colour"]) != [1.0, 1.0, 1.0]:
        raise ValueError("display_luminance is the unit anchor: its emitter colour must be white (1, 1, 1)")
    row = working_luminance_row()
    truth = np.full((height, width), 1.0 * float(t["radiance"]))
    return [("Y", row, truth, "pixels"), ("Y image mean", row, truth, "image")]


def sphere_pixel_classes(t, width, height):
    """For A1's pinhole view of a tessellated sphere: +1 where every ray of the pixel hits the mesh,
    0 where every ray misses it, -1 where the edge may cross (excluded and counted by the harness).
    The mesh (meshes.uv_sphere with the [truth] segments and rings) is a convex polyhedron with its
    vertices on the sphere of radius R, so it contains the ball of radius r_in (the smallest
    distance from the centre to a face plane) and lies inside the ball of radius R. A ray from the
    camera at distance D at angle theta to the axis hits a centred ball of radius r iff
    sin theta < r / D, i.e. its point on the film plane at unit distance lies within
    r / sqrt(D^2 - r^2) of the centre. A pixel (box filter of width 1: its rays cover exactly its
    square) is +1 when its farthest point is inside the r_in circle, 0 when its nearest point is
    outside the R circle. The film's half extent is sensor / (2 focal) on each axis of a square
    image (scenes/lib/ar.py projection_inverse: the sensor spans the larger side). Rows bottom-up."""
    import meshes  # scenes/lib, on the path of every caller of this module
    if width != height:
        raise ValueError("lambertian_furnace assumes a square image")
    radius, dist = float(t["sphere_radius"]), float(t["camera_distance"])
    if dist <= radius:
        raise ValueError("the camera is inside the sphere")
    m = meshes.uv_sphere(radius, int(t["segments"]), int(t["rings"]))
    p = np.asarray(m.positions, dtype=np.float64)[m.corner_vert.reshape(-1, 3)]
    n = np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0])
    r_in = float(np.min(np.abs(np.einsum("ij,ij->i", n / np.linalg.norm(n, axis=1)[:, None], p[:, 0]))))
    t_in, t_out = (r / np.sqrt(dist * dist - r * r) for r in (r_in, radius))
    half = float(t["sensor_mm"]) / (2.0 * float(t["focal_mm"]))
    edges = (np.arange(width + 1) / width * 2.0 - 1.0) * half
    lo, hi = edges[:-1], edges[1:]
    near = np.where((lo <= 0) & (hi >= 0), 0.0, np.minimum(np.abs(lo), np.abs(hi)))
    far = np.maximum(np.abs(lo), np.abs(hi))
    r_far = np.hypot(far[None, :], far[:, None])
    r_near = np.hypot(near[None, :], near[:, None])
    return np.where(r_far < t_in, 1, np.where(r_near > t_out, 0, -1))


def lambertian_furnace(t, width, height):
    """A1: a Lambertian sphere (grey rho, specular weight 0) in a constant white world of radiance
    Lw, camera outside. A convex reflector sees only the world, so every sphere pixel is rho Lw and
    every other pixel Lw (0009 d20, 0005 V1's lossless target). Pixels the silhouette may cross are
    NaN: the harness excludes and counts them (sphere_pixel_classes states the rule)."""
    rho, lw = float(t["rho"]), float(t["world_radiance"])
    cls = sphere_pixel_classes(t, width, height)
    truth = np.where(cls == 1, rho * lw, np.where(cls == 0, lw, np.nan))
    eye = np.eye(3)
    return [(c, eye[i], truth, "pixels") for i, c in enumerate("RGB")]


def lamp_irradiance(power, lamp, x, y):
    """Irradiance at (x, y, 0) on the plane z = 0 (normal +z) from a point lamp of power P at `lamp`,
    or from a sphere lamp of power P centred there that lies wholly above the plane: E = I cos
    theta / D^2 with I = P / (4 pi) for the point (0004 D6); E = pi L (R / D)^2 cos theta with L =
    P / (4 pi^2 R^2) for the sphere, which is the same P cos theta / (4 pi D^2) (a uniformly
    bright sphere above the tangent plane irradiates a point as a point source at its centre).
    cos theta = z_lamp / D, D the distance to the lamp's centre.
    Ref: docs/research/09-test-validation.md S4 (E = pi L (R/d)^2; E = I cos / d^2);
    docs/decisions/0004-integrator.md D6 (point/sphere row: I = P / 4 pi, L = P / (4 pi^2 r^2))."""
    lx, ly, lz = (float(v) for v in lamp)
    d2 = (x - lx) ** 2 + (y - ly) ** 2 + lz * lz
    return power / (4.0 * np.pi) * lz / d2 ** 1.5


def lamp_over_plane(t, width, height):
    """A4: a point lamp (lamp_radius 0) or sphere lamp over a Lambertian plane z = 0 of grey albedo
    rho, seen by a pinhole camera on the +z axis looking straight down (scenes/lib/ar.py look_at
    with up +y: film x is world x, film y is world y, rows bottom-up), black world. A plane pixel is
    colour rho / pi E (0009 d20), E from lamp_irradiance averaged over the pixel's square (box filter
    of width 1) by 8 x 8 Gauss-Legendre quadrature, so the truth is the pixel's mean, not its centre.
    A sphere lamp must sit on the camera axis; its disc is seen directly with radiance L = P /
    (4 pi^2 R^2) (0004 D6) in front of the plane, so a pixel the disc covers is colour (L + rho / pi
    E). Lamps never occlude (0004 D8). Pixels the disc's edge may cross are NaN (excluded): the
    sphere is exact, so the ball test of sphere_pixel_classes applies with r_in = R.
    Ref: Golub and Welsch 1969 (Gauss-Legendre nodes, numpy.polynomial.legendre.leggauss)."""
    rho, power, radius = float(t["rho"]), float(t["power"]), float(t["lamp_radius"])
    lamp = [float(v) for v in t["lamp_position"]]
    cam = float(t["camera_height"])
    colour = np.asarray(t["colour"], dtype=np.float64)
    if colour.shape != (3,):
        raise ValueError("truth colour must be an RGB triple")
    if not lamp[2] > radius:
        raise ValueError("the lamp must lie wholly above the plane")
    x, y, wq = _pixel_quadrature(t, width, height)
    e_mean = np.einsum("hwab,a,b->hw", lamp_irradiance(power, lamp, x, y), wq, wq)
    half = float(t["sensor_mm"]) / (2.0 * float(t["focal_mm"]))
    edges = (np.arange(width + 1) / width * 2.0 - 1.0) * half
    lo, hi = edges[:-1], edges[1:]
    plane = rho / np.pi * e_mean
    truth = plane
    if radius > 0.0:
        if lamp[0] != 0.0 or lamp[1] != 0.0:
            raise ValueError("a sphere lamp must sit on the camera axis (the disc test assumes it)")
        dist = cam - lamp[2]
        if dist <= radius:
            raise ValueError("the camera is inside the sphere lamp")
        t_r = radius / np.sqrt(dist * dist - radius * radius)
        near = np.where((lo <= 0) & (hi >= 0), 0.0, np.minimum(np.abs(lo), np.abs(hi)))
        far = np.maximum(np.abs(lo), np.abs(hi))
        r_far = np.hypot(far[None, :], far[:, None])
        r_near = np.hypot(near[None, :], near[:, None])
        radiance = power / (4.0 * np.pi ** 2 * radius * radius)
        truth = np.where(r_far < t_r, radiance + plane, np.where(r_near > t_r, plane, np.nan))
    eye = np.eye(3)
    return [(c, eye[i], truth * colour[i], "pixels") for i, c in enumerate("RGB")]


def _pixel_quadrature(t, width, height):
    """World (x, y) on the plane z = 0 of the 8 x 8 Gauss-Legendre nodes of every pixel of the
    look-down pinhole view lamp_over_plane describes, and the per-axis weights (each summing to 1).
    Returns (x (1, w, 1, 8), y (h, 1, 8, 1), wq (8,)); rows bottom-up.
    Ref: Golub and Welsch 1969 (numpy.polynomial.legendre.leggauss)."""
    if width != height:
        raise ValueError("the look-down plane truths assume a square image")
    cam = float(t["camera_height"])
    half = float(t["sensor_mm"]) / (2.0 * float(t["focal_mm"]))
    edges = (np.arange(width + 1) / width * 2.0 - 1.0) * half
    nodes, weights = np.polynomial.legendre.leggauss(8)
    lo, hi = edges[:-1], edges[1:]
    f = (lo + hi)[:, None] / 2.0 + (hi - lo)[:, None] / 2.0 * nodes[None, :]
    return cam * f[None, :, None, :], cam * f[:, None, :, None], weights / 2.0


def disk_factor(a, h, r):
    """Configuration factor from a differential area to a parallel coaxial-offset disk: the disk
    (radius r) faces the element from height h, the element is a distance a from the disk's axis.
    F = 1/2 [1 - (a^2 + h^2 - r^2) / sqrt((a^2 + h^2 + r^2)^2 - 4 r^2 a^2)], which is the catalog
    form with H = h / a, R = r / a, Z = 1 + H^2 + R^2 multiplied through by a^2, so a = 0 (on the
    axis) is defined and gives r^2 / (r^2 + h^2).
    Ref: Howell, Menguc and Siegel, Thermal Radiation Heat Transfer (configuration factor catalog,
    differential planar element to a parallel disk, case B-13)."""
    a2, h2, r2 = a * a, h * h, r * r
    return 0.5 * (1.0 - (a2 + h2 - r2) / np.sqrt((a2 + h2 + r2) ** 2 - 4.0 * r2 * a2))


def disk_over_plane(t, width, height):
    """A3: a disk lamp of radius r at lamp_position, facing straight down (its emitting front, local
    -Z), over a Lambertian plane z = 0 of grey albedo rho, black world, the look-down pinhole camera
    of lamp_over_plane above the disk. The disk is a Lambertian emitter of radiance L = P / (pi^2
    r^2) (0004 D6 area row: L = P / (pi A)), so E = pi L F and a plane pixel is colour rho L F, F
    from disk_factor averaged over the pixel's square (8 x 8 Gauss-Legendre). The camera sees only
    the disk's back, which emits nothing, and lamps never occlude (0004 D8), so no pixel is
    excluded: a two-sided disk shows."""
    rho, power, radius = float(t["rho"]), float(t["power"]), float(t["lamp_radius"])
    lx, ly, lz = (float(v) for v in t["lamp_position"])
    colour = np.asarray(t["colour"], dtype=np.float64)
    if colour.shape != (3,) or not radius > 0.0:
        raise ValueError("disk_over_plane needs an RGB colour and a radius > 0")
    if not (0.0 < lz < float(t["camera_height"])):
        raise ValueError("the disk must lie between the plane and the camera")
    x, y, wq = _pixel_quadrature(t, width, height)
    f = disk_factor(np.hypot(x - lx, y - ly), lz, radius)
    radiance = power / (np.pi ** 2 * radius * radius)
    plane = rho * radiance * np.einsum("hwab,a,b->hw", f, wq, wq)
    eye = np.eye(3)
    return [(c, eye[i], plane * colour[i], "pixels") for i, c in enumerate("RGB")]


def _sphere_outside_view(t, centre, radius):
    """True if a sphere lies wholly outside the look-down camera's view pyramid: beyond one of its
    four side planes by more than its radius."""
    cam = float(t["camera_height"])
    half = float(t["sensor_mm"]) / (2.0 * float(t["focal_mm"]))
    cx, cy, cz = centre
    norm = np.sqrt(1.0 + half * half)
    return any((abs(c) - half * (cam - cz)) / norm > radius for c in (cx, cy))


def lit_furnace(t, width, height):
    """Small lit furnace (0004 D1 row): a Lambertian plane z = 0 of grey albedo rho in a constant
    world of radiance Lw, lit by a small sphere lamp of radius R outside the camera's view, rendered
    with max_depth = 1, black elsewhere. The plane sees the world over its whole upper hemisphere
    (lamps never occlude, 0004 D8), so a plane pixel is colour (rho Lw + rho / pi E) with E =
    pi Le (R / D)^2 cos theta, Le = P / (4 pi^2 R^2), i.e. L_r = rho Le (R / d)^2 below the lamp
    (lamp_irradiance), averaged over the pixel by 8 x 8 Gauss-Legendre. The plane cannot see
    itself, so max_depth = 1 drops nothing. Checks: R, G, B per pixel and the image mean of display
    Y, whose z SE the harness holds to [tolerance] max_relative (0.5%) of the truth."""
    rho, power, radius = float(t["rho"]), float(t["power"]), float(t["lamp_radius"])
    lamp = [float(v) for v in t["lamp_position"]]
    lw = float(t["world_radiance"])
    colour = np.asarray(t["colour"], dtype=np.float64)
    if colour.shape != (3,):
        raise ValueError("truth colour must be an RGB triple")
    if not lamp[2] > radius:
        raise ValueError("the lamp must lie wholly above the plane")
    if not _sphere_outside_view(t, lamp, radius):
        raise ValueError("the lamp must lie outside the camera's view")
    x, y, wq = _pixel_quadrature(t, width, height)
    e = np.einsum("hwab,a,b->hw", lamp_irradiance(power, lamp, x, y), wq, wq)
    plane = rho * lw + rho / np.pi * e
    eye = np.eye(3)
    checks = [(c, eye[i], plane * colour[i], "pixels") for i, c in enumerate("RGB")]
    row = working_luminance_row()
    return checks + [("Y image mean", row, plane * float(row @ colour), "image")]


def mode_agreement(t, width, height):
    """d1, d8: no closed form; renders of the same scene under different NEE test modes
    (ASTRORAY_TEST_NEE, RULINGS.md 77) must agree in the mean (Veach 1997, ch. 9: MIS and each
    strategy alone are unbiased estimators of one integral). For every pair in [truth] pairs, display
    Y per pixel block ([truth] size, spp, block) and as the image mean, or as the image mean only
    when [truth] image_mean_only is true; the third field is the pair of modes, not a truth array."""
    row = working_luminance_row()
    blocks = not t.get("image_mean_only", False)
    checks = []
    for a, b in t["pairs"]:
        for m in (a, b):
            if m not in ("on", "off", "light_only"):
                raise ValueError(f"unknown NEE mode {m!r}")
        if blocks:
            checks.append((f"Y {a}~{b}", row, (a, b), "pair_pixels"))
        checks.append((f"Y image mean {a}~{b}", row, (a, b), "pair_image"))
    return checks


FORMULAS = {"emitter_radiance": emitter_radiance, "display_luminance": display_luminance,
            "lambertian_furnace": lambertian_furnace, "lamp_over_plane": lamp_over_plane,
            "disk_over_plane": disk_over_plane, "lit_furnace": lit_furnace, "mode_agreement": mode_agreement}


def evaluate(truth_table, width, height):
    name = truth_table["formula"]
    if name not in FORMULAS:
        raise ValueError(f"unknown truth formula '{name}'; known: {sorted(FORMULAS)}")
    return FORMULAS[name](truth_table, width, height)
