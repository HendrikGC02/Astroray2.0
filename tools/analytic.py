# SPDX-License-Identifier: Apache-2.0
"""Analytic scene harness (0009 d16): renders out/scenes/<name>.arrec through `astroray_cli dump`
at 32x32, reads each pixel's mean and variance of the mean from the dump and compares every pixel
with the closed-form truth of scenes/lib/truths.py.

Usage: <python> -I tools/analytic.py --scene <name> | --gate | --moved --device cpu|cuda [--spp N] [--seed S] [--png P]
       <python> -I tools/analytic.py --scene <comparison scene> --device D --null PAIRS
Prints one line per scene channel and a final PASS or FAIL line with the worst pixel's z and a
reproduce command; exits non-zero on failure.

Rule per pixel: |mean - truth| <= z SE + eps_sys, SE = sqrt(VAR) (0008 d3, exact in the
sampler's independent mode, which the harness requires), eps_sys from meta.toml (default 0).
Also, per channel: not black where truth is not, not constant where truth varies.
Comparison scenes (truths.py mode_agreement, RULINGS.md 77) have no truth: the harness renders the
scene once per NEE mode named by its pairs (ASTRORAY_TEST_NEE, independent seeds) and compares two
renders per pixel block and as the image mean (size, spp and block from [truth], comparison_plan),
|mean_a - mean_b| <= z sqrt(VAR_a + VAR_b) + eps_sys.
Ref: docs/decisions/0009-build-test-process.md d15, d16; Sidak 1967 (thresholds); Veach 1997,
ch. 9 (MIS and each strategy alone estimate the same integral).
"""
import argparse
import functools
import importlib.util
import math
import os
import statistics
import struct
import subprocess
import sys
import tempfile
import time
import tomllib
import zlib

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLI = os.path.join(ROOT, "out", "bin", "astroray_cli.exe")
SCENES_OUT = os.path.join(ROOT, "out", "scenes")
FITS_READ = os.path.join(ROOT, "tests", "abi", "fits_read.py")
BUDGETS = os.path.join(ROOT, "tools", "budgets.toml")
sys.path.insert(0, os.path.join(ROOT, "scenes", "lib"))
import meta  # noqa: E402
import truths  # noqa: E402

SIZE = 32  # 0009 d11: analytic scenes at 32x32 in gate
# Calibrated minimum (072 review): the per-pixel normal threshold holds at 1024 spp, where the null
# run of A5 gave a worst |z| of 3.76 over 3 x 1024 pixels on CPU and CUDA (about 3.4 expected); at
# 64 spp the skewed spectral samples put the worst |z| at 5.48 against 5.50. Lower values need their
# own null run first. At 1024 spp a 1% emission error is about 7.7 SE in A5's green channel.
MIN_SPP = 1024
# 0009 d11: run by check.ps1 gate on both devices.
GATE_SCENES = ("a5_emitter", "y_anchor", "a1_furnace", "a1_table_furnace", "a1_world_half", "a4_point", "a4_blackbody_lamp", "a4_sphere", "a3_disk",
               "lit_furnace_small", "d1_nee_onoff", "d8_stacked_lamps", "d1_uv_chart", "d3_alpha_cards", "d3_alpha_nee", "v1_slab",
               "a9_one_texel", "d11_env_onoff")
# Gate scenes whose render on one device moved to `check full` (owner decision 38: named long-stable
# statistical cases leave the gate when concurrent grouping is used up; check.ps1 full runs them with
# --moved). The gate keeps the other device, and CPU-CUDA equivalence is the agreement tests' (0009
# d12). d11_env_onoff on the CPU: three renders, each baking the program world at 1024^2 on the CPU
# (host/light/env_bake.h kEnvBakeSize), 11 s against 1.3 s on CUDA.
# a4_sphere, a3_disk and lit_furnace_small (3.4-4.4 s on the CPU, 1.3-1.5 s on CUDA each) left on both
# devices for check full; the gate keeps their lamp and furnace siblings (a4_point, a4_blackbody_lamp,
# a1_furnace, d1_nee_onoff) and v1_slab, the volume guard.
MOVED_TO_FULL = {"cpu": ("d11_env_onoff", "a4_sphere", "a3_disk", "lit_furnace_small"),
                 "cuda": ("a4_sphere", "a3_disk", "lit_furnace_small")}
# Scenes whose harness line also reports lamp crossings per ray (0004 D8 row), and the samples per
# pixel of the in-process render that reads the counters (one pass suffices: counters are per pass).
CROSSING_SCENES = ("d8_stacked_lamps",)
COUNTER_SPP = 4
# Comparison scenes (truths.py mode_agreement) set [truth] size, spp and block: a pass costs about
# 2.7 ms on both devices whatever the image size (launch-bound), so they render a larger image at
# fewer spp and test blocks of block x block pixels holding block^2 spp >= MIN_SPP samples
# (comparison_plan). The threshold stays Sidak at the family alpha; a null run only shows that the
# block statistic is normal enough for it. Null run of the pairs the gate checks (106, 64x64 at 64
# spp, 8x8 blocks, CPU, independent seeds; `--scene S --device cpu --null 100`), signed z = (a - b)
# / SE over blocks with SE > 0, one-sided normal count in brackets:
#   d1 on~off, 100 pairs: 0 block and 0 image alarms; Y z>3 / z<-3: 1 / 3 (8.6), |z|>4: 1 (0.4),
#     skew -0.01; G z>3 / z<-3: 7 / 4 (8.6). Normal.
#   d1 on~light_only, 100 pairs: 0 / 0 alarms; Y z>3 / z<-3: 2 / 4, |z|>4: 0, skew +0.04. Normal.
#   d8 on~off, 100 pairs: 52 block-check alarms (0.004 expected), 0 image alarms; Y z>4.99: 67 / 0
#     of 2400 blocks, skew +1.81. FAILS: the pixel row straddling the floor's near edge holds a
#     few floor samples, so BSDF-only (off) lighting there is a rare event and its VAR is
#     underestimated (mean block z +1 in that row). So d8 is tested on the image mean only, as the
#     0004 D8 row asks (lead ruling, [truth] image_mean_only). Its image-mean z over the same 100
#     pairs: 0 alarms at 4.11, min -2.16 max +1.99, mean -0.17 sd 0.93 skew +0.06; z>1 / z<-1: 10
#     / 19 (15.9 each), z>2 / z<-2: 0 / 3 (2.3), beyond 3: 0 (0.13). Normal.
DEVICES = ("cpu", "cuda")
FAMILY_ALPHA = 0.01  # 0009 d15
# tests/common/stat.cpp kRepoConstant and the FNV-1a / MixBits rule of seed_for() (task 020).
REPO_CONSTANT = 0x415354524159
MASK64 = (1 << 64) - 1


def fnv1a(text):
    # Ref: Fowler-Noll-Vo FNV-1a, 64-bit offset basis and prime (as tests/common/stat.cpp).
    h = 14695981039346656037
    for c in text.encode():
        h = ((h ^ c) * 1099511628211) & MASK64
    return h


def mix_bits(v):
    # Ref: pbrt-v4 src/pbrt/util/hash.h MixBits (as tests/common/stat.cpp).
    v ^= v >> 31
    v = (v * 0x7FB5D329728EA185) & MASK64
    v ^= v >> 27
    v = (v * 0x81DADEF4BC2DD44D) & MASK64
    v ^= v >> 33
    return v


def seed_for(name):
    """020's seed_for(name), ASTRORAY_SEED_SHA mixed in when set (full runs); the low 32 bits,
    because ar_settings.seed and the CLI's --seed are 32-bit."""
    h = mix_bits(fnv1a(name) ^ REPO_CONSTANT)
    sha = os.environ.get("ASTRORAY_SEED_SHA", "")
    if sha:
        h = mix_bits(h ^ fnv1a(sha))
    return h & 0xFFFFFFFF


def family_m():
    with open(BUDGETS, "rb") as f:
        b = tomllib.load(f)
    for table in ("family", "family_provisional"):
        if "analytic_scenes" in b.get(table, {}):
            return int(b[table]["analytic_scenes"])
    raise ValueError(f"{BUDGETS}: no analytic_scenes family size")


def sidak(alpha, n):
    """Per-test alpha so that n independent tests together have size alpha: 1 - (1 - alpha)^(1/n).
    Ref: Sidak 1967. expm1/log1p keep precision for tiny results."""
    return -math.expm1(math.log1p(-alpha) / n)


def pixel_z(m, pixels):
    """Two-sided per-pixel critical value. Each scene channel is one member of the analytic family
    (size m, family alpha 0.01), so its test has size alpha_t = sidak(0.01, m); the channel passes
    only if all its pixels pass, and its pixel means are independent, so each pixel is tested at
    sidak(alpha_t, pixels) (Sidak 1967 twice). m = 256, 1024 pixels: z = 5.50."""
    a = sidak(sidak(FAMILY_ALPHA, m), pixels)
    return -statistics.NormalDist().inv_cdf(a / 2.0)


def check_channel(mean, var, truth, z, eps_sys):
    """One scene channel. mean, var (variance of the mean) and truth are (h, w) arrays; a NaN truth
    marks a pixel the scene's truth excludes (A1's silhouette), which is not tested. Returns
    (pass, reasons, worst) with worst = (z of the worst pixel, (x, y)), the worst pixel being the one
    with the largest |mean - truth| / (z SE + eps_sys)."""
    mean, var, truth = (np.asarray(a, dtype=np.float64) for a in (mean, var, truth))
    reasons = []
    if not (np.isfinite(mean).all() and np.isfinite(var).all()) or (var < 0).any():
        return False, ["non-finite mean or variance, or negative variance"], (math.inf, (0, 0))
    tested = ~np.isnan(truth)
    if not tested.any():
        return False, ["the truth excludes every pixel"], (math.inf, (0, 0))
    truth = np.where(tested, truth, mean)  # an excluded pixel has d = 0
    se = np.sqrt(var)
    d = mean - truth
    tol = z * se + eps_sys
    with np.errstate(divide="ignore", invalid="ignore"):
        ratio = np.where(tol > 0, np.abs(d) / tol, np.where(d == 0, 0.0, np.inf))
        zs = np.where(se > 0, d / se, np.where(d == 0, 0.0, np.copysign(np.inf, d)))
    y, x = np.unravel_index(int(np.argmax(ratio)), ratio.shape)
    worst = (float(zs[y, x]), (int(x), int(y)))
    bad = int((ratio > 1).sum())
    if bad:
        reasons.append(f"{bad} pixels outside z SE + eps_sys")
    lit = tested & (truth != 0)
    if lit.any() and not (mean[lit] != 0).any():
        reasons.append("black where truth is not")
    tt, mt = truth[tested], mean[tested]
    if tt.max() != tt.min() and mt.max() == mt.min():
        reasons.append("constant where truth varies")
    return not reasons, reasons, worst


def load_dump(path, spp, device, size=SIZE):
    spec = importlib.util.spec_from_file_location("_fits_read_analytic", FITS_READ)
    fr = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fr)
    with open(path, "rb") as f:
        hdus = fr.read_hdus(f.read())
    head, planes = hdus[0][0], {h["EXTNAME"]: d for h, d in hdus[1:]}
    if head.get("SAMPLER") != "independent":
        raise RuntimeError(f"the dump's SAMPLER is {head.get('SAMPLER')!r}: the harness needs the exact "
                           "variance of independent mode (0008 d3); set sampler='independent' in the builder")
    if head.get("DEVICE") != device:
        raise RuntimeError(f"the dump's DEVICE is {head.get('DEVICE')!r}, asked for {device!r}: the render ran elsewhere")
    if head.get("SPP") != spp:
        raise RuntimeError(f"the dump has SPP {head.get('SPP')}, asked for {spp}")
    mean = np.stack([planes[f"display.{c}"] for c in range(3)]).astype(np.float64)
    var = np.stack([planes[f"display.{c}.VAR"] for c in range(3)]).astype(np.float64)
    nsamp = np.stack([planes[f"display.{c}.NSAMP"] for c in range(3)])
    if mean.shape != (3, size, size) or not (nsamp == spp).all():
        raise RuntimeError(f"the dump is {mean.shape[1:]} with NSAMP {np.unique(nsamp)}, expected {size}x{size} at {spp}")
    return mean, var


def write_png(path, rgb, scale, enlarge=8):
    """rgb (3, h, w), rows bottom-up (the film's order), times `scale`, sRGB-encoded 8-bit, each
    pixel enlarged to enlarge x enlarge. Ref: IEC 61966-2-1 (sRGB transfer); PNG ISO/IEC 15948."""
    v = np.clip(np.asarray(rgb, dtype=np.float64) * scale, 0.0, 1.0)
    v = np.where(v <= 0.0031308, 12.92 * v, 1.055 * v ** (1 / 2.4) - 0.055)
    img = np.round(v * 255).astype(np.uint8).transpose(1, 2, 0)[::-1]
    img = img.repeat(enlarge, axis=0).repeat(enlarge, axis=1)
    h, w = img.shape[:2]
    raw = b"".join(b"\x00" + img[r].tobytes() for r in range(h))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def channel(w, mean, var):
    """The channel w . (R, G, B) of (3, h, w) planes and a bound on its variance of the mean."""
    w = np.asarray(w, dtype=np.float64)
    cmean = np.tensordot(w, mean, axes=1)
    if np.count_nonzero(w) == 1:
        return cmean, np.tensordot(w * w, var, axes=1)
    # The dump has no covariance between channels, so the SE of a combination is bounded by the sum
    # of the parts' SEs (Minkowski inequality for standard deviations).
    return cmean, np.tensordot(np.abs(w), np.sqrt(var), axes=1) ** 2


def block(x, power, k):
    """Sum of x over k x k pixel blocks divided by k^(2 power): the block mean for power 1, the
    variance of the block mean (from pixel variances) for power 2."""
    h, w = x.shape
    return x.reshape(h // k, k, w // k, k).sum(axis=(1, 3)) / float(k) ** (2 * power)


def check_scene(mean, var, checks, m, eps_sys, max_relative=None, renders=None, block_size=1):
    """Every check of one scene on one device. mean and var are the (3, h, w) display planes of the
    render with NEE on, which truth checks read; renders maps an NEE mode to its (mean, var) for
    pair checks. checks come from truths.evaluate.
    max_relative (meta.toml [tolerance]) is the relative tolerance an image-mean check must be able
    to resolve: it fails unless z SE <= max_relative |truth|, so a pass bounds the relative error.
    Pair checks compare two independent renders: the difference has variance VAR_a + VAR_b.
    Returns [(name, pass, reasons, worst, measured mean, truth mean)]; for a pair, the means of the
    two renders."""
    out = []
    for name, w, truth, kind in checks:
        if kind in ("pair_pixels", "pair_image"):
            (ma, va), (mb, vb) = (channel(w, *renders[mode]) for mode in truth)
            if kind == "pair_pixels":
                # Blocks of block_size^2 pixels (comparison_plan): a single-strategy estimator of a
                # small light is a rare-event sum, too skewed per pixel for the normal threshold (a
                # pixel with no BSDF hit in all its samples has mean and variance 0); a block pools
                # the events of all its pixels. Block means of independent pixel means: variance
                # sum(VAR) / k^4.
                k = block_size
                ma, va, mb, vb = (block(x, s, k) for x, s in ((ma, 1), (va, 2), (mb, 1), (vb, 2)))
                passed, reasons, worst = check_channel(ma, va + vb, mb, pixel_z(m, ma.size), eps_sys)
            else:
                n2 = ma.size ** 2
                passed, reasons, worst = check_channel(np.full((1, 1), ma.mean()), np.full((1, 1), (va.sum() + vb.sum()) / n2),
                                                       np.full((1, 1), mb.mean()), pixel_z(m, 1), eps_sys)
            out.append((name, passed, reasons, worst, float(ma.mean()), float(mb.mean())))
            continue
        cmean, cvar = channel(w, mean, var)
        if kind == "pixels" and block_size > 1:
            # A truth scene with a block plan (comparison_plan): block means of independent pixel
            # means, variance sum(VAR) / k^4; the truth must be one value per block.
            if np.isnan(truth).any():
                raise ValueError(f"check {name}: a block check cannot exclude pixels")
            tb = block(truth, 1, block_size)
            if not (np.repeat(np.repeat(tb, block_size, axis=0), block_size, axis=1) == truth).all():
                raise ValueError(f"check {name}: the truth varies inside a {block_size}x{block_size} block")
            cmean, cvar, truth = block(cmean, 1, block_size), block(cvar, 2, block_size), tb
        if kind == "pixels":
            ntested = int(np.count_nonzero(~np.isnan(truth)))  # Sidak over the pixels tested
            passed, reasons, worst = check_channel(cmean, cvar, truth, pixel_z(m, max(ntested, 1)), eps_sys)
        elif kind == "image":
            if np.isnan(truth).any():
                raise ValueError(f"check {name}: an image-mean check cannot exclude pixels")
            # The image mean of independent pixel means: variance sum(VAR) / npix^2, one test at the
            # family's per-test alpha.
            se, tmean, z = math.sqrt(cvar.sum()) / cmean.size, float(truth.mean()), pixel_z(m, 1)
            passed, reasons, worst = check_channel(np.full((1, 1), cmean.mean()), np.full((1, 1), se * se),
                                                   np.full((1, 1), tmean), z, eps_sys)
            if max_relative is not None:
                if not z * se + eps_sys <= max_relative * abs(tmean):
                    passed = False
                    reasons.append(f"z SE + eps_sys = {(z * se + eps_sys) / abs(tmean):.2%} of the truth cannot "
                                   f"resolve the stated {max_relative:.2%}: raise the spp")
                else:
                    reasons.append(f"resolves {(z * se + eps_sys) / abs(tmean):.3%} <= {max_relative:.2%}")
        else:
            raise ValueError(f"check {name}: unknown kind {kind!r}")
        tested = ~np.isnan(truth)
        out.append((name, passed, reasons, worst, float(cmean[tested].mean()) if tested.any() else math.nan,
                    float(np.nanmean(truth)) if tested.any() else math.nan))
    return out


def gate_scenes(device):
    """The scenes `--gate` renders on `device`: GATE_SCENES less the ones moved to full there."""
    return [s for s in GATE_SCENES if s not in MOVED_TO_FULL[device]]


def gate_members():
    """Family members the gate runs: one per check of each scene the gate renders on each device,
    plus the truth-tested renders of tools/integrator_checks.py (also gate steps)."""
    # By path (python -I puts no script folder on sys.path), here because it imports this module.
    spec = importlib.util.spec_from_file_location("integrator_checks", os.path.join(ROOT, "tools", "integrator_checks.py"))
    ic = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ic)
    n = ic.gate_members()
    return n + sum(scene_members(gate_scenes(d)) for d in DEVICES)


def shard_scenes(scenes, i, n):
    """The scenes of shard i of n (1-based): every scene is in exactly one shard. Round robin, so
    neighbours in GATE_SCENES (often the same truth family and cost) are spread over the shards."""
    if not 1 <= i <= n:
        raise ValueError(f"--shard {i}/{n}: i must be in 1..n")
    return list(scenes)[i - 1::n]


@functools.lru_cache(maxsize=None)
def scene_checks(scene):
    """The number of checks of one scene. Cached per process: the count evaluates the scene's truth,
    and a quadrature truth (A9's environment_texel, about 0.5 s) would otherwise be evaluated again by
    every count the gate's family and shard lines make."""
    t = meta.load(os.path.join(ROOT, "scenes", scene, "meta.toml"))["truth"]
    size = int(t.get("size", SIZE))
    return len(truths.evaluate(t, size, size))


def scene_members(scenes, device_count=1):
    """Family members (checks) of `scenes` on `device_count` devices, as gate_members counts them."""
    return sum(scene_checks(s) for s in scenes) * device_count


def check_family():
    """The gate's member count must equal budgets.toml [family_members] analytic_scenes, which lint
    holds to at most m (0009 d15)."""
    with open(BUDGETS, "rb") as f:
        declared = tomllib.load(f).get("family_members", {}).get("analytic_scenes")
    n, m = gate_members(), family_m()
    if declared != n or n > m:
        raise RuntimeError(f"analytic family: the gate runs {n} members, budgets.toml [family_members] "
                           f"declares {declared}, m = {m}; update the declaration (at most m)")


def check_spp(spp):
    if spp < MIN_SPP:
        raise ValueError(f"--spp {spp} is below the calibrated minimum {MIN_SPP}: the per-pixel normal "
                         "threshold is not calibrated there; run a null calibration before lowering it")


def crossing_report(scene, device):
    """Prints lamp crossings per closest-hit ray and steps per pass of `scene` (0004 D8 row: reported
    against 0002's budgets, not tested). The CLI's dump carries no counters, so the scene is built
    in this process through the ABI binding (scenes/<scene>/build.py populate), rendered at
    COUNTER_SPP with NEE on, and ar_session_read_counters gives the last pass's words in
    src/host/session/readback.cpp counter_words order: rays_closest, rays_shadow, steps, launches,
    kStageCount stage items, then 11 words of which lamp_crossings is the ninth, so it is word
    n - 3 of n. Ref: tests/abi/test_session_api.py test_counters (the word count from the error)."""
    import ctypes
    import ar  # scenes/lib, on the path above
    spec = importlib.util.spec_from_file_location(f"_build_{scene}", os.path.join(ROOT, "scenes", scene, "build.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    os.environ.pop("ASTRORAY_TEST_NEE", None)
    with tempfile.TemporaryDirectory() as tmp, ar.Scene(os.path.join(tmp, "counters.arrec"), device=device) as s:
        a, lib = s.abi, s.lib
        mod.populate(s, spp=COUNTER_SPP, size=SIZE)
        gen = ctypes.c_uint64()
        s._call(lib.ar_edit_commit(s.edit_h, ctypes.byref(gen)), "ar_edit_commit")
        sd = a.ar_session_desc(a.AR_SESSION_MODE_FINAL, SIZE, SIZE, a.AR_DENOISER_NONE)
        s._call(lib.ar_session_create(s.scene_h, ctypes.byref(sd), ctypes.byref(s.session_h)), "ar_session_create")
        v = a.ar_view_desc()
        ctypes.memmove(ctypes.byref(v.camera), ctypes.byref(s._camera), ctypes.sizeof(a.ar_camera_desc))
        v.region_width, v.region_height, v.render_scale = SIZE, SIZE, 1.0
        s._call(lib.ar_session_set_view(s.session_h, ctypes.byref(v)), "ar_session_set_view")
        s._call(lib.ar_session_start(s.session_h), "ar_session_start")
        s._call(lib.ar_session_wait(s.session_h, 60000), "ar_session_wait")
        p = a.ar_progress()
        s._call(lib.ar_session_get_progress(s.session_h, ctypes.byref(p)), "ar_session_get_progress")
        if p.state != a.AR_STATE_CONVERGED:
            raise RuntimeError(f"crossings {scene} {device}: the session did not converge (state {p.state})")
        probe = (ctypes.c_uint64 * 1)()
        if lib.ar_session_read_counters(s.session_h, 0, probe) != a.AR_ERR_INVALID_ARGUMENT:
            raise RuntimeError("ar_session_read_counters accepted a zero-length buffer")
        err = (ctypes.c_uint8 * 256)()
        lib.ar_session_last_error(s.session_h, 256, err)
        n = int(bytes(err).split(b"\0")[0].decode().rsplit("need ", 1)[1])
        words = (ctypes.c_uint64 * n)()
        s._call(lib.ar_session_read_counters(s.session_h, n, words), "ar_session_read_counters")
        rays, steps, crossings = words[0], words[2], words[n - 3]
    if rays == 0:
        raise RuntimeError(f"crossings {scene} {device}: the last pass traced no closest-hit rays")
    print(f"crossings analytic {scene} {device}: {crossings / rays:.3f} lamp crossings per closest-hit ray "
          f"({crossings} of {rays}), {steps} steps per pass, one pass of {SIZE}x{SIZE} at NEE on")


def marker_check(scene, device, t, mean, block_size):
    """D1's orientation marker (0009 d20, Blender's bottom-up rows): the block of largest display
    luminance must be [truth] marker_block, (x, y) counted from the bottom-left of the bottom-up
    image, in the render and in the truth. A deterministic assertion, not a family member."""
    row = truths.working_luminance_row()
    want = tuple(int(v) for v in t["marker_block"])
    got = []
    for img in (mean, truths.chart_linear(t, mean.shape[2], mean.shape[1])):
        y = block(np.tensordot(row, img, axes=1), 1, block_size)
        by, bx = np.unravel_index(int(np.argmax(y)), y.shape)
        got.append((int(bx), int(by)))
    ok = got[0] == want and got[1] == want
    print(f"analytic {scene} {device} marker: {'ok' if ok else 'FAIL'} brightest block at {got[0]} in the render, "
          f"{got[1]} in the chart, expected {want} (bottom-left, rows bottom-up)")
    return ok


def comparison_plan(t, spp=None):
    """(size, spp, block) of a scene: SIZE at spp (default MIN_SPP) with per-pixel tests for a truth
    scene; [truth] size, spp (or `spp`) and block for a comparison scene or a truth scene that sets
    a block (D1's colour cells, lead ruling in task 077), refused unless every block holds at least
    MIN_SPP samples (the calibrated minimum per tested unit)."""
    if t["formula"] != "mode_agreement" and "block" not in t:
        spp = MIN_SPP if spp is None else spp
        check_spp(spp)
        return SIZE, spp, 1
    size, block_size = int(t["size"]), int(t["block"])
    spp = int(t["spp"]) if spp is None else spp
    if size % block_size:
        raise ValueError(f"[truth] size {size} is not a multiple of block {block_size}")
    if block_size * block_size * spp < MIN_SPP:
        raise ValueError(f"block {block_size}^2 x {spp} spp = {block_size * block_size * spp} samples per block, "
                         f"below the calibrated minimum {MIN_SPP}")
    return size, spp, block_size


def hook_env(nee="on", light_sampler="tree"):
    """The CLI's environment with the test hooks (src/host/session/test_hooks.cpp) set to these
    values and nothing inherited for them: ASTRORAY_TEST_NEE ("on" leaves it unset) and
    ASTRORAY_TEST_LIGHT_SAMPLER ("tree", the default, leaves it unset; "alias" selects the oracle)."""
    env = {k: v for k, v in os.environ.items() if k not in ("ASTRORAY_TEST_NEE", "ASTRORAY_TEST_LIGHT_SAMPLER")}
    if nee != "on":
        env["ASTRORAY_TEST_NEE"] = nee
    if light_sampler != "tree":
        env["ASTRORAY_TEST_LIGHT_SAMPLER"] = light_sampler
    return env


def render_mode(rec, device, spp, seed, size, mode):
    """One CLI dump of `rec` with ASTRORAY_TEST_NEE = mode ("on" leaves it unset); returns
    (mean, var) or the CLI's output as a string on failure."""
    env = hook_env(nee=mode)
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "dump.fits")
        r = subprocess.run([CLI, "dump", rec, "-o", out, "--device", device, "--spp", str(spp),
                            "--seed", str(seed), "--size", f"{size}x{size}"],
                           capture_output=True, text=True, cwd=tmp, timeout=600, env=env)
        if r.returncode != 0:
            return f"astroray_cli exited {r.returncode} (NEE {mode})\n{r.stdout}{r.stderr}"
        return load_dump(out, spp, device, size)


def mode_seed(seed, mode):
    """Each mode gets its own seed, so pair renders are independent and their variances add."""
    return seed if mode == "on" else mix_bits(seed ^ fnv1a(mode)) & 0xFFFFFFFF


def scene_paths(scene):
    m = meta.load(os.path.join(ROOT, "scenes", scene, "meta.toml"))
    # Class D scenes whose truth is tested by the z SE rule (D1, lead ruling in task 077) run here too.
    if not (m["class"] == "A" or (m["class"] == "D" and m.get("tolerance", {}).get("kind") == "z_se"
                                  and "formula" in m.get("truth", {}))):
        raise ValueError(f"scene {scene} is class {m['class']}, not analytic (nor class D with a z_se truth)")
    rec = os.path.join(SCENES_OUT, scene + ".arrec")
    for p in (CLI, rec):
        if not os.path.exists(p):
            raise FileNotFoundError(f"{p} missing: run tools/build.ps1 and tools/scenes.py first")
    return m, rec


def run(scene, device, spp, seed, png=None):
    """Renders and checks one scene; prints its lines and returns True on PASS. spp None: the
    scene's default (comparison_plan)."""
    t0 = time.perf_counter()
    m, rec = scene_paths(scene)
    size, spp, block_size = comparison_plan(m["truth"], spp)
    repro = (f"<python> -I tools/analytic.py --scene {scene} --device {device} --spp {spp} --seed {seed}")
    checks = truths.evaluate(m["truth"], size, size)
    # The NEE modes the checks need (ASTRORAY_TEST_NEE, RULINGS.md 77): "on" for truth checks.
    modes = sorted({mode for c in checks if c[3].startswith("pair_") for mode in c[2]} | {"on"})
    renders = {}
    for mode in modes:
        r = render_mode(rec, device, spp, mode_seed(seed, mode), size, mode)
        if isinstance(r, str):
            print(r)
            print(f"FAIL analytic {scene} {device}: {r.splitlines()[0]}; reproduce: {repro}")
            return False
        renders[mode] = r
    results = check_scene(*renders["on"], checks, family_m(), m["eps_sys"], m.get("tolerance", {}).get("max_relative"),
                          renders, block_size)
    ok = all(r[1] for r in results)
    worst_all = None
    for (name, passed, reasons, worst, cmean, tmean), check in zip(results, checks):
        if worst_all is None or abs(worst[0]) > abs(worst_all[0]):
            worst_all = (worst[0], worst[1], name)
        if check[3].startswith("pair_"):
            label = f"{check[2][0]} {cmean:.6f} {check[2][1]} {tmean:.6f}"
            excluded = 0
        else:
            label = f"mean {cmean:.6f} truth {tmean:.6f}"
            excluded = int(np.isnan(check[2]).sum())
        print(f"analytic {scene} {device} {name}: {'ok' if passed else 'FAIL'} {label} worst z {worst[0]:+.2f} "
              f"at {worst[1]}" + (f", {excluded} excluded pixels" if excluded else "")
              + ("; " + "; ".join(reasons) if reasons else ""))
    if scene in CROSSING_SCENES:
        crossing_report(scene, device)
    chart = m["truth"]["formula"] == "texture_chart"
    if chart:
        ok = marker_check(scene, device, m["truth"], renders["on"][0], block_size) and ok
    if png:
        mean = renders["on"][0]
        if chart:
            # The render beside the decoded source chart, both at display scale 1, two black columns between.
            ref = truths.chart_linear(m["truth"], size, size)
            write_png(png, np.concatenate([mean, np.zeros((3, size, 2)), ref], axis=2), 1.0)
        else:
            tmax = max((float(np.nanmax(t)) for _, _, t, k in checks if not k.startswith("pair_")),
                       default=float(mean.max()))
            write_png(png, mean, 0.5 / tmax if tmax > 0 else 1.0)
    secs = time.perf_counter() - t0
    units = (size // block_size) ** 2
    unit = "block" if block_size > 1 else "pixel"
    print(f"{'PASS' if ok else 'FAIL'} analytic {scene} {device}: worst z {worst_all[0]:+.2f} "
          f"({worst_all[2]} at {worst_all[1]}) against per-{unit} z {pixel_z(family_m(), units):.2f} "
          f"+ eps_sys {m['eps_sys']:g}, {size}x{size} at {spp} spp"
          + (f" in {block_size}x{block_size} blocks" if block_size > 1 else "")
          + f", seed {seed}, {secs:.1f} s; reproduce: {repro}")
    return ok


def null_calibration(scene, device, pairs, pair, spp=None):
    """Null run of a comparison scene's block test for the mode pair (a, b) the gate checks:
    `pairs` independent-seed render pairs. Every mode is unbiased, so with correct code every
    difference is noise, and a cross-mode pair keeps the skew of the single-strategy estimator (a
    same-mode pair would cancel it). Counts the checks that would alarm at the Sidak thresholds the
    gate uses (block check: any block beyond z; image-mean check) and prints the tails of the signed
    z = (a - b) / SE of the blocks with SE > 0 (black background blocks test nothing) against the
    normal, two-sided and each side, for the tested channel Y (its SE is the Minkowski bound, so its
    z is conservative) and for G alone (exact variance). A calibration, not a fitted threshold."""
    m, rec = scene_paths(scene)
    size, spp, block_size = comparison_plan(m["truth"], spp)
    rows = {"Y": truths.working_luminance_row(), "G": np.array([0.0, 1.0, 0.0])}
    fam = family_m()
    zb_crit, zi_crit = pixel_z(fam, (size // block_size) ** 2), pixel_z(fam, 1)
    zb = {k: [] for k in rows}
    zi, alarms = [], 0
    t0 = time.perf_counter()
    for i in range(pairs):
        got = []
        for mode in pair:
            r = render_mode(rec, device, spp, mix_bits(fnv1a(f"null:{scene}:{pair}:{i}:{mode}")) & 0xFFFFFFFF, size, mode)
            if isinstance(r, str):
                raise RuntimeError(r)
            got.append(r)
        for k, row in rows.items():
            (ma, va), (mb, vb) = (channel(row, *g) for g in got)
            bm = [block(x, s, block_size) for x, s in ((ma, 1), (va, 2), (mb, 1), (vb, 2))]
            se = np.sqrt(bm[1] + bm[3])
            z = (bm[0] - bm[2])[se > 0] / se[se > 0]
            zb[k].extend(z)
            if k == "Y":
                alarms += int((np.abs(z) > zb_crit).any())
                zi.append((ma.mean() - mb.mean()) / (math.sqrt(va.sum() + vb.sum()) / ma.size))
    nd = statistics.NormalDist()
    tails = []
    for k, v in zb.items():
        v = np.asarray(v)
        cuts = (2.0, 3.0, 4.0, round(zb_crit, 2))
        tails.append(f"{k} over {v.size} blocks (normal one side): " + ", ".join(
            f"z>{c}: {int((v > c).sum())} z<-{c}: {int((v < -c).sum())} ({v.size * nd.cdf(-c):.2f})" for c in cuts)
            + f", min {v.min():+.2f} max {v.max():+.2f}, skew {float(((v - v.mean()) ** 3).mean() / v.std() ** 3):+.3f}")
    zi = np.asarray(zi)
    itail = ", ".join(f"z>{c}: {int((zi > c).sum())} z<-{c}: {int((zi < -c).sum())} ({zi.size * nd.cdf(-c):.2f})"
                      for c in (1.0, 2.0, 3.0))
    iskew = float(((zi - zi.mean()) ** 3).mean() / zi.std() ** 3)
    print(f"null {scene} {device} {pair[0]}~{pair[1]}: {pairs} pairs, {size}x{size} at {spp} spp, "
          f"{block_size}x{block_size} blocks; block-check alarms {alarms} of {pairs} at z {zb_crit:.2f} (expected "
          f"{pairs * sidak(FAMILY_ALPHA, fam):.4f}); image-mean alarms {int((np.abs(zi) > zi_crit).sum())} at z "
          f"{zi_crit:.2f}, min {zi.min():+.2f} max {zi.max():+.2f}, mean {zi.mean():+.3f} sd {zi.std():.3f} skew "
          f"{iskew:+.3f}, {itail}; tails: {'; '.join(tails)}; {time.perf_counter() - t0:.0f} s")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--scene", action="append", help="scene name (repeatable)")
    ap.add_argument("--gate", action="store_true", help="the gate's scenes (GATE_SCENES), after checking "
                    "the declared family size")
    ap.add_argument("--shard", metavar="I/N", help="with --gate: run shard I of N of the gate's scenes "
                    "(1-based, round robin); the family size check still counts every scene")
    ap.add_argument("--device", required=True, choices=DEVICES)
    ap.add_argument("--spp", type=int, help=f"default: {MIN_SPP}, or a comparison scene's [truth] spp")
    ap.add_argument("--seed", type=int, help="default: seed_for(scene name), task 020's rule")
    ap.add_argument("--png", help="write the render here (one scene only), display scaled so truth is 0.5")
    ap.add_argument("--moved", action="store_true", help="the gate scenes moved to check full on --device "
                    "(MOVED_TO_FULL)")
    ap.add_argument("--null", type=int, metavar="PAIRS", help="null calibration of a comparison scene: PAIRS "
                    "render pairs for each mode pair its [truth] pairs name (null_calibration), no test")
    a = ap.parse_args()
    if bool(a.scene) + a.gate + a.moved != 1:
        ap.error("give one of --scene, --gate and --moved")
    if a.null:
        for s in a.scene or ap.error("--null takes --scene"):
            for pair in meta.load(os.path.join(ROOT, "scenes", s, "meta.toml"))["truth"]["pairs"]:
                null_calibration(s, a.device, a.null, tuple(pair), a.spp)
        return 0
    if a.gate:
        check_family()
    scenes = gate_scenes(a.device) if a.gate else list(MOVED_TO_FULL[a.device]) if a.moved else a.scene
    if a.shard:
        if not a.gate:
            ap.error("--shard goes with --gate")
        si, sn = (int(x) for x in a.shard.split("/"))
        scenes = shard_scenes(scenes, si, sn)
        # check.ps1 sums the k of every shard against the total: each scene runs in exactly one shard.
        print(f"members analytic {a.device} shard {si}/{sn}: {scene_members(scenes)} of {scene_members(gate_scenes(a.device))}")
    if a.png and len(scenes) != 1:
        ap.error("--png takes one scene")
    ok = True
    for s in scenes:
        ok = run(s, a.device, a.spp, seed_for(s) if a.seed is None else a.seed, a.png) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())