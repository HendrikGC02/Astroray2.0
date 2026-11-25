# SPDX-License-Identifier: Apache-2.0
"""Closed-form truths of the analytic scenes (0009 d16, d20), named by meta.toml's [truth] formula.

A truth function takes the [truth] table and the image size and returns a list of checks, each
(name, weights, truth, kind): the measured channel is weights . (display R, G, B) per pixel and
truth is an (height, width) array in display units (working-space RGB, film exposure 1). kind
"pixels" tests every pixel, "image" tests the image mean; "pair_pixels" and "pair_image" compare
two renders instead (mode_agreement). Every check is one member of the
analytic family per device (tools/analytic.py; tools/integrator_checks.py for uniform_enclosure).
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


def observer_tables():
    """(cmf, d65, y_n, m_ws) from the shipped observer.bin: the three colour matching functions and
    the D65 SPD (Y = 1) on the 1 nm grid 360..830, the normaliser Y_n, and XYZ -> linear Rec.709."""
    with open(OBSERVER_BIN, "rb") as f:
        raw = f.read()
    if len(raw) != _BYTES or raw[:6] != b"ARDATA":
        raise ValueError(f"{OBSERVER_BIN}: not the observer.bin layout of src/host/data_resource.h")
    cmf = np.frombuffer(raw, dtype="<f4", count=3 * _SAMPLES, offset=_CMF_AT).reshape(3, _SAMPLES).astype(np.float64)
    y_n = struct.unpack_from("<d", raw, _CMF_AT + 12 * _SAMPLES)[0]
    d65 = np.frombuffer(raw, dtype="<f4", count=_SAMPLES, offset=_MWS_AT - 4 * _SAMPLES).astype(np.float64)
    m_ws = np.array(struct.unpack_from("<9d", raw, _MWS_AT)).reshape(3, 3)
    return cmf, d65, y_n, m_ws


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


def alpha_transmittance(t, width, height):
    """d3 alpha cards: a view-filling emitter seen through `cards` stacked cards of opacity a that
    only pass or stop a ray; each card is a null event with probability 1 - a on its own uniform
    (0004 D3, D14), so the image mean of every display channel is the emitted colour times
    radiance times (1 - a)^cards. Image means only: at that transmittance most pixels see no event
    in their samples, so a pixel's variance estimate is 0 and a per-pixel test is invalid."""
    a = float(t["opacity"])
    if not 0.0 <= a <= 1.0:
        raise ValueError("truth opacity must lie in [0, 1]")
    rgb = _emitted_rgb(t) * (1.0 - a) ** int(t["cards"])
    eye = np.eye(3)
    return [(f"{n} image mean", eye[i], np.full((height, width), rgb[i]), "image") for i, n in enumerate("RGB")]


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


def lambertian_furnace_table(t, width, height):
    """A1 with a TABLE base colour: the A1 scene (convex Lambertian sphere, constant white world of
    radiance Lw, camera outside) whose reflectance is a piecewise-linear spectrum rho(l), clamped to
    [0, 1] as a REFLECTANCE slot is (0005 item 10), end values outside its points (0003 d8). One
    bounce on a convex body is exact: a sphere pixel is Lw rho(l) D65(l) through the observer,
    colour = M_ws . (sum over the 1 nm grid of rho(l) D65(l) cmf(l)) / Y_n times Lw, per display
    channel (0003 d11: white, rho = 1, is (1, 1, 1) to the 2.4e-4 of the working space's white
    point). Outside pixels are Lw as in A1; silhouette pixels are NaN (sphere_pixel_classes).
    Ref: 0005 item 9 and Verification 4; 0003 d8, d11."""
    nm = np.asarray(t["table_nm"], dtype=np.float64)
    val = np.asarray(t["table_value"], dtype=np.float64)
    if nm.shape != val.shape or nm.ndim != 1 or nm.size < 2 or np.any(np.diff(nm) <= 0) or np.any(val < 0):
        raise ValueError("truth table_nm must be strictly increasing with one non-negative table_value each")
    lw = float(t["world_radiance"])
    cmf, d65, y_n, m_ws = observer_tables()
    rho = np.clip(float(t["table_scale"]) * np.interp(np.arange(360, 831, dtype=np.float64), nm, val), 0.0, 1.0)
    colour = lw * (m_ws @ ((cmf * (rho * d65)).sum(axis=1) / y_n))
    cls = sphere_pixel_classes(t, width, height)
    eye = np.eye(3)
    return [(c, eye[i], np.where(cls == 1, colour[i], np.where(cls == 0, lw, np.nan)), "pixels") for i, c in enumerate("RGB")]


# CODATA 2018 exact SI constants, as src/kernel/spectral/planck.h's c1L and c2 are derived.
_H, _C, _KB = 6.62607015e-34, 299792458.0, 1.380649e-23


def blackbody_working_rgb(kelvin, scale=1.0):
    """The display RGB of a Blackbody emitter of Planck(T) / Y(Planck(T)) times `scale` (0003 d11):
    M_ws . XYZ over the 1 nm grid 360..830 with the shipped observer, no illuminant multiply (the
    kernel resolves a BLACKBODY ref analytically, 0005 item 39), so its Y is `scale` and the colour
    is the working-space RGB of the Planck chromaticity. Planck in W m^-2 sr^-1 nm^-1, double.
    Ref: 0003 d11, d25; src/host/spectral/blackbody.cpp (the same sum)."""
    cmf, _, y_n, m_ws = observer_tables()
    lam = np.arange(360, 831, dtype=np.float64) * 1e-9
    planck = 2.0 * _H * _C * _C / lam ** 5 / np.expm1(_H * _C / (lam * _KB * float(kelvin))) * 1e-9
    xyz = (cmf * planck).sum(axis=1) / y_n
    return float(scale) * (m_ws @ (xyz / xyz[1]))


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
    """A4: a point lamp (lamp_radius 0) or sphere lamp, coloured by `colour` or by a blackbody_kelvin
    (blackbody_working_rgb), over a Lambertian plane z = 0 of grey albedo rho, seen by a pinhole camera on the +z axis looking straight down (scenes/lib/ar.py look_at
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
    if ("blackbody_kelvin" in t) == ("colour" in t):
        raise ValueError("a lamp truth names its colour by exactly one of `colour` (RGB) or `blackbody_kelvin`")
    if "blackbody_kelvin" in t:
        colour = blackbody_working_rgb(t["blackbody_kelvin"], t.get("blackbody_scale", 1.0))
    else:
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


def uniform_enclosure(t, width, height):
    """d2, d4: a camera inside a closed enclosure every surface of which is grey Lambertian of albedo
    rho and emits white radiance Le, black world. The radiance leaving every point is then the same,
    and with at most D = max_depth bounces a path that has made k bounces adds Le rho^k in
    expectation (emission is counted at the hit before the bounce limit ends the path), so every
    pixel is Le sum_{k=0..D} rho^k = Le (1 - rho^(D+1)) / (1 - rho), which tends to Le / (1 - rho)
    (0004 D2 row) as D grows. It holds with Russian roulette on or off and whatever opaque occluders
    the enclosure holds, as long as they carry the same material. One check: display Y per pixel,
    which tools/integrator_checks.py tests in [truth] block x block blocks.
    Ref: docs/decisions/0004-integrator.md D2 (Verification row); Pharr, Jakob and Humphreys,
    Physically Based Rendering 4th ed., sec. 13.1 (the Neumann series of the rendering equation)."""
    rho, le, depth = float(t["rho"]), float(t["radiance"]), int(t["max_depth"])
    if not 0.0 <= rho < 1.0 or depth < 0:
        raise ValueError("uniform_enclosure needs 0 <= rho < 1 and max_depth >= 0")
    row = working_luminance_row()
    value = le * (1.0 - rho ** (depth + 1)) / (1.0 - rho) * float(row @ np.ones(3))
    return [("Y", row, np.full((height, width), value), "pixels")]


def read_png_rgb8(path):
    """(h, w, 3) uint8 of an 8-bit RGB, non-interlaced PNG whose rows all use filter type 0 (what
    scenes/d1_uv_chart/build.py writes), rows in file order (top first); anything else raises.
    Ref: PNG, ISO/IEC 15948 (chunk layout, IHDR fields, filter byte per scanline)."""
    import zlib
    with open(path, "rb") as f:
        raw = f.read()
    if raw[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos, idat, head = 8, b"", None
    while pos < len(raw):
        n, tag = struct.unpack(">I4s", raw[pos:pos + 8])
        data = raw[pos + 8:pos + 8 + n]
        if tag == b"IHDR":
            head = struct.unpack(">IIBBBBB", data)
        elif tag == b"IDAT":
            idat += data
        pos += 12 + n
    if head is None or head[2:] != (8, 2, 0, 0, 0):
        raise ValueError(f"{path}: not 8-bit RGB, filter method 0, no interlace")
    w, h = head[0], head[1]
    rows = np.frombuffer(zlib.decompress(idat), dtype=np.uint8).reshape(h, 1 + 3 * w)
    if (rows[:, 0] != 0).any():
        raise ValueError(f"{path}: a scanline uses a filter other than 0")
    return rows[:, 1:].reshape(h, w, 3)


def srgb_to_linear(c):
    """The sRGB electro-optical transfer of encoded values in [0, 1]. Ref: IEC 61966-2-1:1999."""
    c = np.asarray(c, dtype=np.float64)
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def chart_linear(t, width, height):
    """D1's chart as display RGB (3, height, width), rows bottom-up: each texel's 8-bit sRGB value
    decoded to linear, times the emission strength."""
    img = read_png_rgb8(os.path.join(ROOT, t["chart"]))
    if img.shape[:2] != (height, width):
        raise ValueError(f"the chart is {img.shape[1]}x{img.shape[0]}; texture_chart needs one texel per pixel "
                         f"({width}x{height})")
    return srgb_to_linear(img[::-1].astype(np.float64) / 255.0).transpose(2, 0, 1) * float(t["strength"])


def texture_chart(t, width, height):
    """D1: an emission-only plane textured with the chart through Closest filtering fills the view,
    one texel per pixel, rows bottom-up (0008 d1). Closest picks the texel in its stored 8-bit sRGB
    encoding, which is then decoded (0005 item 34), and the RGB emission returns as the same
    working-space RGB in the mean (0003 d12), so every pixel's display RGB is the decoded texel
    colour times the strength. One check per channel, tested per pixel or, with a [truth] block,
    per block (tools/analytic.py)."""
    rgb = chart_linear(t, width, height)
    eye = np.eye(3)
    return [(c, eye[i], rgb[i], "pixels") for i, c in enumerate("RGB")]


RGB2SPEC_BIN = os.path.join(ROOT, "data", "spectral", "rec709.jh")


def rgb2spec_coeffs(rgb):
    """Jakob-Hanika sigmoid coefficients (c0, c1, c2) of a reflectance RGB in [0, 1], from the shipped
    rec709.jh: grey is analytic, anything else a trilinear blend of the 8 surrounding entries indexed
    by the largest component, as src/kernel/color/rgb2spec.h fetch_coeffs (its .jh layout comment:
    header 64 B, res u32 at byte 24, z_nodes f32[res], coeffs f32[3][res][res][res][3]).
    Ref: Jakob and Hanika 2019; pbrt-v4 RGBToSpectrumTable::operator() (Apache-2.0)."""
    with open(RGB2SPEC_BIN, "rb") as f:
        raw = f.read()
    if raw[:6] != b"ARDATA":
        raise ValueError(f"{RGB2SPEC_BIN}: not a .jh table")
    res = struct.unpack_from("<I", raw, 24)[0]
    z_nodes = np.frombuffer(raw, dtype="<f4", count=res, offset=64).astype(np.float64)
    coeffs = np.frombuffer(raw, dtype="<f4", count=3 * res ** 3 * 3, offset=64 + 4 * res).reshape(3, res, res, res, 3)
    r = [float(v) for v in rgb]
    if r[0] == r[1] == r[2]:
        v = r[0]
        return (0.0, 0.0, -np.inf if v <= 0 else (np.inf if v >= 1 else (v - 0.5) / np.sqrt(v * (1 - v))))
    m = (0 if r[0] > r[2] else 2) if r[0] > r[1] else (1 if r[1] > r[2] else 2)
    z = r[m]
    x, y = r[(m + 1) % 3] * (res - 1) / z, r[(m + 2) % 3] * (res - 1) / z
    xi, yi = min(int(x), res - 2), min(int(y), res - 2)
    zi = int(np.clip(np.searchsorted(z_nodes, z, side="left") - 1, 0, res - 2))
    dx, dy, dz = x - xi, y - yi, (z - z_nodes[zi]) / (z_nodes[zi + 1] - z_nodes[zi])
    c = coeffs[m, zi:zi + 2, yi:yi + 2, xi:xi + 2, :]  # [dz][dy][dx][coefficient]
    cx = (1 - dx) * c[:, :, 0] + dx * c[:, :, 1]
    cy = (1 - dy) * cx[:, 0] + dy * cx[:, 1]
    return tuple((1 - dz) * cy[0] + dz * cy[1])


def sigmoid_spectrum(coeffs, nm):
    """S(c0 l^2 + c1 l + c2) with S(x) = 1/2 + x / (2 sqrt(1 + x^2)), exact 0 and 1 at -inf and +inf."""
    c0, c1, c2 = coeffs
    if np.isinf(c2):
        return np.full_like(nm, 1.0 if c2 > 0 else 0.0)
    x = (c0 * nm + c1) * nm + c2
    return 0.5 + x / (2.0 * np.sqrt(1.0 + x * x))


def medium_absorption(sigma_a, nm):
    """sigma_a(l) of a test medium (ar_medium_desc) with absorption triple s in 1/m: src/host/scene/
    media.cpp folds it to density D = max(s) and colour c = 1 - s / D, and the absorber is D (1 - S_c(l))
    = D S(-poly_c(l)) (vm/ops/volume_coeffs.h: the complement by negated coefficients, 0006 A.1)."""
    s = np.asarray(sigma_a, dtype=np.float64)
    d = float(s.max())
    if d <= 0:
        return np.zeros_like(nm)
    c = rgb2spec_coeffs(1.0 - s / d)
    return d * sigmoid_spectrum(tuple(-v for v in c), nm)


def beer_lambert_slab(t, width, height):
    """V1: a pinhole camera looks down at a white emitter (radiance L, its spectrum D65 with Y = 1,
    0003 d12) across a closed box of thickness d holding an absorbing medium that covers the left half
    of the view (world x < 0; image column 0 is world -x). A ray at angle theta to the axis that crosses
    the box sees L T(l) with T(l) = exp(-sigma_a(l) d / cos theta), every lane alike (0006 V1); the
    right half sees L. Display colour = M_ws . sum over the observer's 1 nm grid of cmf(l) D65(l) T(l)
    / Y_n times L, per pixel averaged over 8 x 8 Gauss-Legendre directions of its footprint (the
    look-down pinhole of _pixel_quadrature at unit height, so a node's direction is (fx, fy, -1)).
    sigma_a(l) is the medium's lift (medium_absorption), transcribed from the engine's fold.
    Ref: 0006 V1, A.1; 0003 d11, d16; Beer-Lambert."""
    cmf, d65, y_n, m_ws = observer_tables()
    nm = np.arange(360, 831, dtype=np.float64)
    sigma = medium_absorption(t["sigma_a"], nm)
    if np.any(np.asarray(t["sigma_s"], dtype=np.float64) != 0.0):
        raise ValueError("beer_lambert_slab is the absorbing slab: sigma_s must be 0")
    d, radiance = float(t["thickness"]), float(t["radiance"])
    if list(t["colour"]) != [1.0, 1.0, 1.0]:
        raise ValueError("beer_lambert_slab: the emitter must be white")
    fx, fy, wq = _pixel_quadrature(dict(t, camera_height=1.0), width, height)
    w2 = wq[:, None] * wq[None, :]  # (8 y nodes, 8 x nodes)
    weight = cmf * d65  # (3, 471)
    truth = np.empty((3, height, width))
    for row in range(height):
        # Path length factor 1 / cos theta = sqrt(1 + fx^2 + fy^2) at every node of the row's pixels.
        xs, ys = fx[0, :, 0, :], fy[row, 0, :, 0]  # (w, 8 x nodes), (8 y nodes)
        sec = np.sqrt(1.0 + xs[:, None, :] ** 2 + ys[None, :, None] ** 2)  # (w, 8 y, 8 x)
        trans = np.exp(-np.multiply.outer(sec * d, sigma))  # (w, 8, 8, 471)
        xyz = np.einsum("wabl,cl,ab->cw", trans, weight, w2) / y_n
        slab = m_ws @ xyz  # (3, w)
        white = m_ws @ (weight.sum(axis=1) / y_n)
        left = (np.arange(width) + 0.5) < width / 2
        truth[:, row, :] = radiance * np.where(left[None, :], slab, white[:, None])
    eye = np.eye(3)
    return [(c, eye[i], truth[i], "pixels") for i, c in enumerate("RGB")]


FORMULAS = {"emitter_radiance": emitter_radiance, "alpha_transmittance": alpha_transmittance,
            "display_luminance": display_luminance,
            "lambertian_furnace": lambertian_furnace, "lambertian_furnace_table": lambertian_furnace_table,
            "lamp_over_plane": lamp_over_plane,
            "disk_over_plane": disk_over_plane, "lit_furnace": lit_furnace, "mode_agreement": mode_agreement,
            "uniform_enclosure": uniform_enclosure, "texture_chart": texture_chart, "beer_lambert_slab": beer_lambert_slab}


def evaluate(truth_table, width, height):
    name = truth_table["formula"]
    if name not in FORMULAS:
        raise ValueError(f"unknown truth formula '{name}'; known: {sorted(FORMULAS)}")
    return FORMULAS[name](truth_table, width, height)
