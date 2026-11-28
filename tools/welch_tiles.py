# SPDX-License-Identifier: Apache-2.0
"""Tile Welch test (0009 d17(v)): do two renderers agree in mean, tile by tile?

Renders a scene through `astroray_cli dump` on N independent seeds per side, takes each seed's mean
over every 8x8-pixel tile of the display R, G and B planes, runs a Welch test per (tile, channel) on
the N against N tile means with a 1 % relative margin, and one Holm step-down over every (tile,
channel) of the image at family alpha 0.05. Pass = zero rejected tiles.

Usage:
  <python> -I tools/welch_tiles.py --self-test
  <python> -I tools/welch_tiles.py --scene d10_many_lights --device cpu|cuda [--png P] [--history F]
      the light tree (default sampler) against the importance alias table (the oracle behind
      ASTRORAY_TEST_LIGHT_SAMPLER=alias, 0004 D10 row "D10 tree"): prints the rejected tiles, the
      variance ratio alias / tree of the image mean of the per-pixel variance of the mean with its
      spread over seeds, and the wall seconds; exits non-zero on a rejected tile or a ratio under 2.
  <python> -I tools/welch_tiles.py --scene d10_many_lights --device cpu --null R
      null calibration (0009 Verification 17(v)): R repeats of N tree seeds against N other tree
      seeds; a repeat fails when any tile is rejected; exits non-zero above 2 failing repeats.
The scene's meta.toml [truth] gives size, spp, tile and seeds; its recording comes from
tools/scenes.py. Seeds come from tools/analytic.py seed_for (0009 d15, ASTRORAY_SEED_SHA rotates
them in full runs); the reducer fails closed on missing, duplicate or reused seeds, on legs of
different shape and on non-finite planes, as v1's did (docs/v1-lessons/research/testing.md).

Margin (PROVISIONAL lead ruling, task 190, pending v1's "candidate 1b" definition): per (tile,
channel), ref = (mean_a + mean_b) / 2, delta = 0.01 |ref|, t = max(|mean_a - mean_b| - delta, 0) /
SE, Welch's degrees of freedom unchanged; margin_t is the one place it lives.
Ref: docs/decisions/0009-build-test-process.md d15, d17(v) and Verification 17(v); 0004 D10 row;
B. L. Welch 1947, Biometrika 34:28 (unequal-variance t, SE^2 = s_a^2/n_a + s_b^2/n_b, degrees of
freedom SE^4 / sum (s^2/n)^2 / (n - 1)); S. Holm 1979, Scand. J. Statist. 6:65 (step-down);
Jung et al. 2020 (comparing renders by tile statistics, as the D10 row cites it).
"""
import argparse
import datetime
import importlib.util
import json
import math
import os
import subprocess
import sys
import tempfile
import time

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))  # -I leaves the script's own folder off the path
import machine_id  # noqa: E402
ALPHA = 0.05  # 0009 d17(v) family alpha
MARGIN = 0.01  # 0009 d17(v) relative margin
NULL_MAX_FAILING = 2  # 0009 Verification 17(v): at most 2 failing repeats per scene
RATIO_MIN = 1.70  # 0004 Verification "D10 tree": 1.8x accepted for d10_many_lights with full MIS (owner decision 43; floor RULINGS 109)
SIDES = ("tree", "alias")


def _analytic():
    spec = importlib.util.spec_from_file_location("_analytic_welch", os.path.join(ROOT, "tools", "analytic.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# --- Student t tail ------------------------------------------------------------------------------

def betai(a, b, x):
    """Regularised incomplete beta I_x(a, b), elementwise over arrays.
    Ref: Press et al., Numerical Recipes 3rd ed. (2007) section 6.4: the continued fraction
    I_x(a, b) = x^a (1 - x)^b / (a B(a, b)) * 1 / (1 + d1 / (1 + d2 / (1 + ...))) with
    d_{2m+1} = -(a + m)(a + b + m) x / ((a + 2m)(a + 2m + 1)), d_{2m} = m (b - m) x / ((a + 2m - 1)(a + 2m)),
    which converges fast for x < (a + 1) / (a + b + 2); otherwise I_x(a, b) = 1 - I_{1-x}(b, a).
    Evaluated by the modified Lentz method (NR section 5.2), tiny = 1e-300."""
    a, b, x = np.broadcast_arrays(*(np.asarray(v, dtype=np.float64) for v in (a, b, x)))
    out = np.where(x <= 0.0, 0.0, 1.0)
    inner = (x > 0.0) & (x < 1.0)
    if not inner.any():
        return out
    flip = inner & (x >= (a + 1.0) / (a + b + 2.0))
    aa, bb, xx = np.where(flip, b, a)[inner], np.where(flip, a, b)[inner], np.where(flip, 1.0 - x, x)[inner]
    tiny, eps = 1e-300, 1e-15
    qab, qap, qam = aa + bb, aa + 1.0, aa - 1.0
    c = np.ones_like(xx)
    d = 1.0 - qab * xx / qap
    d = 1.0 / np.where(np.abs(d) < tiny, tiny, d)
    h = d.copy()
    done = np.zeros(xx.shape, dtype=bool)
    for m in range(1, 1000):
        m2 = 2.0 * m
        for num in (m * (bb - m) * xx / ((qam + m2) * (aa + m2)),
                    -(aa + m) * (qab + m) * xx / ((aa + m2) * (qap + m2))):
            d = 1.0 + num * d
            d = 1.0 / np.where(np.abs(d) < tiny, tiny, d)
            c = 1.0 + num / c
            c = np.where(np.abs(c) < tiny, tiny, c)
            delta = d * c
            h = np.where(done, h, h * delta)
        done |= np.abs(delta - 1.0) < eps
        if done.all():
            break
    else:
        raise ArithmeticError("betai: the continued fraction did not converge in 1000 terms")
    lnfront = (np.vectorize(math.lgamma)(qab) - np.vectorize(math.lgamma)(aa) - np.vectorize(math.lgamma)(bb)
               + aa * np.log(xx) + bb * np.log1p(-xx))
    val = np.exp(lnfront) * h / aa
    out[inner] = np.where(flip[inner], 1.0 - val, val)
    return out


def t_two_sided_p(t, nu):
    """P(|T| >= t) for Student's t with nu (real, > 0) degrees of freedom: I_{nu/(nu+t^2)}(nu/2, 1/2).
    Ref: Numerical Recipes 3rd ed. section 6.14 (Student's distribution through the incomplete beta,
    A(t|nu) = 1 - I_{nu/(nu+t^2)}(nu/2, 1/2)); the same A(t|nu) is Abramowitz and Stegun 26.7.1."""
    t, nu = np.broadcast_arrays(np.abs(np.asarray(t, dtype=np.float64)), np.asarray(nu, dtype=np.float64))
    return betai(nu / 2.0, 0.5, nu / (nu + t * t))


# --- the test ------------------------------------------------------------------------------------

def tile_means(img, tile):
    """(..., h, w) -> (..., h / tile, w / tile) means; h and w must be multiples of tile."""
    h, w = img.shape[-2:]
    if h % tile or w % tile:
        raise ValueError(f"image {w}x{h} is not a multiple of the {tile}-pixel tile")
    return img.reshape(*img.shape[:-2], h // tile, tile, w // tile, tile).mean(axis=(-3, -1))


def margin_t(diff, ref, se):
    """The PROVISIONAL margin (module docstring): t = max(|diff| - 0.01 |ref|, 0) / se, and for se = 0
    +inf when |diff| exceeds the margin, else 0. One place, so v1's answer changes only this."""
    excess = np.maximum(np.abs(diff) - MARGIN * np.abs(ref), 0.0)
    with np.errstate(divide="ignore", invalid="ignore"):
        return np.where(se > 0, excess / np.where(se > 0, se, 1.0), np.where(excess > 0, np.inf, 0.0))


def var_of_mean(x):
    """The estimated variance of the mean of the n samples along axis 0: s^2 / n, with s^2 the
    sample variance (n - 1 in its denominator), Welch 1947's per-side term."""
    n = x.shape[0]
    return x.var(axis=0, ddof=1) / n


def welch_df(qa, qb, na, nb):
    """Welch 1947's degrees of freedom (qa + qb)^2 / (qa^2 / (na - 1) + qb^2 / (nb - 1)), qa and qb
    the per-side variances of the mean; where both are 0 the statistic is decided by margin_t
    without a tail, and the pooled na + nb - 2 only fills the slot."""
    se2 = qa + qb
    with np.errstate(divide="ignore", invalid="ignore"):
        den = qa * qa / (na - 1) + qb * qb / (nb - 1)
        return np.where(den > 0, se2 * se2 / np.where(den > 0, den, 1.0), float(na + nb - 2))


def welch(a, b):
    """a, b: (n, ...) samples, n at least 2 and possibly different on the two sides. Returns
    (diff, ref, se, df, p) per element: Welch 1947's statistic with margin_t's margin."""
    na, nb = a.shape[0], b.shape[0]
    ma, mb = a.mean(axis=0), b.mean(axis=0)
    qa, qb = var_of_mean(a), var_of_mean(b)
    df = welch_df(qa, qb, na, nb)
    diff, ref, se = ma - mb, 0.5 * (ma + mb), np.sqrt(qa + qb)
    t = margin_t(diff, ref, se)
    p = np.where(np.isinf(t), 0.0, t_two_sided_p(np.where(np.isinf(t), 0.0, t), df))
    return diff, ref, se, df, p


def holm_bounds(m, alpha):
    """Holm 1979's bounds in rank order: the k-th smallest of m p-values (k from 1) is compared
    with alpha / (m - k + 1), so the smallest meets alpha / m and the largest alpha."""
    k = np.arange(1, m + 1)
    return alpha / (m - k + 1)


def holm(p, alpha=ALPHA):
    """Holm 1979 step-down at family alpha over the flat array p (one entry per (tile, channel)).
    Returns a boolean mask of the rejected entries."""
    p = np.asarray(p, dtype=np.float64).ravel()
    order = np.argsort(p, kind="stable")  # ascending: the smallest p meets the strictest bound
    bounds = holm_bounds(p.size, alpha)
    reject = np.zeros(p.size, dtype=bool)
    # Step down: the r-th smallest p (r from 0 here) is rejected when it is at most bounds[r] and
    # every smaller p was rejected; the first that is not ends the procedure and the rest stand.
    # A constant alpha / m would be Bonferroni and alpha / (m - r + 1) with r from 0 an off-by-one:
    # the self-test's four-value example rejects a p that both of those keep.
    for r, i in enumerate(order):
        if p[i] <= bounds[r]:
            reject[i] = True
        else:
            break
    return reject


def check_legs(side_a, side_b, n):
    """Fail closed (v1's reducer discipline): side = list of (seed, (3, h, w) mean). Each side holds
    exactly n distinct seeds (n at least 2: a sample variance), no seed on both sides, equal shapes,
    finite planes."""
    if n < 2:
        raise ValueError(f"{n} seeds a side: the per-tile sample variance needs at least 2")
    for name, side in (("a", side_a), ("b", side_b)):
        seeds = [s for s, _ in side]
        if len(seeds) != n:
            raise ValueError(f"side {name} holds {len(seeds)} seeds, {n} needed (missing seed)")
        if len(set(seeds)) != len(seeds):
            raise ValueError(f"side {name} repeats a seed: {sorted(s for s in set(seeds) if seeds.count(s) > 1)}")
        for s, img in side:
            if img.shape != side_a[0][1].shape:
                raise ValueError(f"side {name} seed {s}: shape {img.shape}, the first leg's is {side_a[0][1].shape}")
            if not np.isfinite(img).all():
                raise ValueError(f"side {name} seed {s}: non-finite pixels")
    reused = {s for s, _ in side_a} & {s for s, _ in side_b}
    if reused:
        raise ValueError(f"seeds used on both sides: {sorted(reused)}")


def compare(side_a, side_b, n, tile):
    """The tile test. Returns dict with per-(channel, ty, tx) arrays diff, ref, se, df, p, reject
    and the signed plain Welch t (no margin) for the map."""
    check_legs(side_a, side_b, n)
    a = tile_means(np.stack([img for _, img in side_a]), tile)  # (n, 3, th, tw)
    b = tile_means(np.stack([img for _, img in side_b]), tile)
    diff, ref, se, df, p = welch(a, b)
    if not np.isfinite(p).all():
        raise ArithmeticError(f"non-finite p in {int((~np.isfinite(p)).sum())} (tile, channel) entries: "
                              "the test cannot decide them (fewer than 2 seeds a side, or a non-finite tile mean)")
    reject = holm(p).reshape(p.shape)
    with np.errstate(divide="ignore", invalid="ignore"):
        t_plain = np.where(se > 0, diff / np.where(se > 0, se, 1.0), 0.0)
    return {"diff": diff, "ref": ref, "se": se, "df": df, "p": p, "reject": reject, "t": t_plain,
            "mean_a": a.mean(axis=0), "mean_b": b.mean(axis=0)}


def rejected_lines(res, tile):
    """One line per rejected (tile, channel): pixel box (x0, y0 from the bottom-left) and statistics."""
    out = []
    for c, ty, tx in zip(*np.nonzero(res["reject"])):
        out.append(f"rejected tile x {tx * tile}-{tx * tile + tile - 1} y {ty * tile}-{ty * tile + tile - 1} "
                   f"channel {'RGB'[c]}: a {res['mean_a'][c, ty, tx]:.6g} b {res['mean_b'][c, ty, tx]:.6g} "
                   f"diff {res['diff'][c, ty, tx]:+.3g} SE {res['se'][c, ty, tx]:.3g} df {res['df'][c, ty, tx]:.1f} "
                   f"p {res['p'][c, ty, tx]:.3g}")
    return out


# --- self-test -----------------------------------------------------------------------------------

def self_test():
    """Synthetic image sets with known means; the t tail against tables; Holm on a worked example;
    the reducer's fail-closed refusals."""
    fails = []
    # Abramowitz and Stegun Table 26.10, two-sided percentage points of t (Q = 0.10, 0.05, 0.01, 0.001).
    table = {5: (2.015, 2.571, 4.032, 6.869), 10: (1.812, 2.228, 3.169, 4.587), 30: (1.697, 2.042, 2.750, 3.646)}
    for nu, ts in table.items():
        for q, t in zip((0.10, 0.05, 0.01, 0.001), ts):
            got = float(t_two_sided_p(t, nu))
            # The table rounds t to 3 decimals: the p it implies is good to about 0.2 % of q.
            if abs(got - q) > 3e-3 * q:
                fails.append(f"t tail nu {nu} t {t}: p {got:.6f}, table {q}")
    # Closed forms at real nu: nu = 1 (Cauchy) and 2.
    for t in (0.3, 1.0, 4.0, 40.0):
        for nu, want in ((1.0, 1.0 - 2.0 / math.pi * math.atan(t)), (2.0, 1.0 - t / math.sqrt(2.0 + t * t))):
            if abs(float(t_two_sided_p(t, nu)) - want) > 1e-12 * max(want, 1e-300) + 1e-14:
                fails.append(f"t tail nu {nu} t {t}: {float(t_two_sided_p(t, nu)):.15g} vs {want:.15g}")
    # Holm 1979 on four p-values: 0.005 <= 0.05/4 and 0.015 <= 0.05/3 are rejected; 0.03 > 0.05/2 stops.
    # Bonferroni (0.05/4 for all) and an off-by-one first bound (0.05/5, then 0.05/4) both keep 0.015.
    want = [True, False, False, True]
    if holm([0.015, 0.04, 0.03, 0.005]).tolist() != want:
        fails.append(f"holm: {holm([0.015, 0.04, 0.03, 0.005]).tolist()} vs {want}")
    # Welch on a worked case with unequal variances and unequal n, constants by hand: a = 1..4
    # (mean 2.5, s^2 = 5/3, s^2/n = 5/12), b = 2, 4, ..., 12 (mean 7, s^2 = 14, s^2/n = 7/3);
    # SE^2 = 5/12 + 7/3 = 11/4; df = (11/4)^2 / ((5/12)^2 / 3 + (7/3)^2 / 5) = 16335/2477 = 6.5947
    # (the pooled 4 + 6 - 2 = 8 would differ).
    diff, _, se, df, _ = welch(np.array([1.0, 2.0, 3.0, 4.0]), np.array([2.0, 4.0, 6.0, 8.0, 10.0, 12.0]))
    for name, got, want_v in (("diff", diff, -4.5), ("se", se, math.sqrt(11.0 / 4.0)), ("df", df, 16335.0 / 2477.0)):
        if not abs(float(got) - want_v) <= 1e-12 * abs(want_v):
            fails.append(f"welch worked case: {name} {float(got):.15g}, by hand {want_v:.15g}")
    # Image sets: 8 seeds a side, 32x32 pixels, mean 1, pixel noise 0.05; tile means have SE 0.05/8/sqrt(8).
    rng = np.random.default_rng(190)
    n, size, tile, sigma = 8, 32, 8, 0.05

    def leg(base, offset=None):
        out = []
        for i in range(n):
            img = 1.0 + rng.normal(0.0, sigma, (3, size, size))
            if offset is not None:
                img[1, 8:16, 16:24] *= 1.0 + offset
            out.append((base + i, img))
        return out

    res = compare(leg(0), leg(100), n, tile)
    if res["reject"].any():
        fails.append(f"equal means: {int(res['reject'].sum())} rejected tiles")
    res = compare(leg(0), leg(100, 0.05), n, tile)
    if res["reject"].sum() != 1 or not res["reject"][1, 1, 2]:
        fails.append(f"5 % offset in one tile: rejected {list(zip(*np.nonzero(res['reject'])))}, expected [(1, 1, 2)]")
    # Under the margin: a 0.5 % offset with no noise is 0.5 % < 1 %, not rejected.
    quiet = [(i, np.ones((3, size, size))) for i in range(n)]
    shifted = [(100 + i, np.ones((3, size, size)) * 1.005) for i in range(n)]
    if compare(quiet, shifted, n, tile)["reject"].any():
        fails.append("a 0.5 % offset at SE 0 was rejected inside the 1 % margin")
    shifted = [(100 + i, np.ones((3, size, size)) * 1.02) for i in range(n)]
    if not compare(quiet, shifted, n, tile)["reject"].all():
        fails.append("a 2 % offset at SE 0 was not rejected everywhere")
    # Null false-positive rate: 40 repeats of equal-mean sets; a family-wise size of 0.05 (the margin
    # only lowers it) expects 2, and P(X >= 8 | 40, 0.05) is 7e-4.
    bad = sum(bool(compare(leg(0), leg(100), n, tile)["reject"].any()) for _ in range(40))
    if bad >= 8:
        fails.append(f"null: {bad} of 40 equal-mean repeats rejected a tile")
    # Fail closed.
    good = leg(0)
    for what, a, b in (("missing", good[:-1], leg(100)), ("duplicate", good[:-1] + [good[0]], leg(100)),
                       ("reused", good, leg(7)),
                       ("shape", good, [(100 + i, img[:, :16]) for i, (_, img) in enumerate(leg(0))]),
                       ("non-finite", good, [(100, np.full((3, size, size), np.nan))] + leg(101)[:-1]),
                       ("single", good[:1], leg(100)[:1])):
        try:
            compare(a, b, 1 if what == "single" else n, tile)
            fails.append(f"fail closed: {what} seed set accepted")
        except ValueError:
            pass
    for f in fails:
        print("FAIL welch_tiles self-test:", f)
    if not fails:
        print("PASS welch_tiles self-test: t tail against A&S 26.10 and closed forms, Holm example, equal means pass, "
              f"a 5 % tile offset rejected, margin holds 0.5 % and rejects 2 %, null {bad} of 40, fail closed on 6 seed faults, Welch worked case by hand")
    return not fails


# --- renders -------------------------------------------------------------------------------------

def scene_plan(scene):
    an = _analytic()
    m = an.meta.load(os.path.join(ROOT, "scenes", scene, "meta.toml"))
    t = m.get("truth", {})
    for k in ("size", "spp", "tile", "seeds"):
        if k not in t:
            raise ValueError(f"scenes/{scene}/meta.toml [truth] has no {k}")
    if int(t["seeds"]) < 2:
        raise ValueError(f"scenes/{scene}/meta.toml [truth] seeds = {t['seeds']}: a sample variance needs at least 2 seeds a side")
    rec = os.path.join(an.SCENES_OUT, scene + ".arrec")
    for p in (an.CLI, rec):
        if not os.path.exists(p):
            raise FileNotFoundError(f"{p} missing: run tools/build.ps1 and tools/scenes.py first")
    return an, rec, int(t["size"]), int(t["spp"]), int(t["tile"]), int(t["seeds"])


def render_side(an, rec, device, spp, size, sampler, names):
    """One dump per seed name with ASTRORAY_TEST_LIGHT_SAMPLER = sampler. Returns [(seed, mean, var)]."""
    out = []
    with tempfile.TemporaryDirectory() as tmp:
        for name in names:
            seed = an.seed_for(name)
            path = os.path.join(tmp, "dump.fits")
            r = subprocess.run([an.CLI, "dump", rec, "-o", path, "--device", device, "--spp", str(spp), "--seed", str(seed),
                                "--size", f"{size}x{size}"], capture_output=True, text=True, cwd=tmp, timeout=600,
                               env=an.hook_env(light_sampler=sampler))
            if r.returncode != 0:
                raise RuntimeError(f"astroray_cli exited {r.returncode} ({sampler}, seed {seed})\n{r.stdout}{r.stderr}")
            mean, var = an.load_dump(path, spp, device, size)
            out.append((seed, mean, var))
    return out


def variance_ratio(tree, alias):
    """Alias over tree of the image mean (pixels and channels) of the per-pixel variance of the mean;
    the spread is the sd and range of the per-seed ratios (seed i of each side, independent)."""
    vt = np.array([v.mean() for _, _, v in tree])
    va = np.array([v.mean() for _, _, v in alias])
    per = va / vt
    per_ch = [float(np.mean([v[c].mean() for _, _, v in alias]) / np.mean([v[c].mean() for _, _, v in tree])) for c in range(3)]
    return float(va.mean() / vt.mean()), per, per_ch


def write_evidence(path, scene, res, tree, alias, tile):
    """Tree mean | alias mean | signed tile t (largest |t| over channels; blue -, red +, white 0,
    full scale +-4; rejected tiles boxed in black), each lamp's rectangle as the camera sees it
    outlined in green in the first two panels. Rows bottom-up."""
    an = _analytic()
    spec = importlib.util.spec_from_file_location("_d10_build", os.path.join(ROOT, "scenes", scene, "build.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    k = 4
    mt = np.mean([m for _, m, _ in tree], axis=0)
    ma = np.mean([m for _, m, _ in alias], axis=0)
    h, w = mt.shape[1:]
    scale = 0.5 / max(float(np.percentile(mt.mean(axis=0), 99)), 1e-12)

    def srgb_panel(img):
        v = np.clip(img * scale, 0.0, 1.0)
        return np.where(v <= 0.0031308, 12.92 * v, 1.055 * v ** (1 / 2.4) - 0.055)

    def up(img):
        return img.repeat(k, axis=1).repeat(k, axis=2)

    pa, pb = up(srgb_panel(mt)), up(srgb_panel(ma))
    for x0, y0, x1, y1 in mod.lamp_boxes_px(w, h):  # pixel boxes, bottom-up rows
        xs, ys = np.clip([int(x0 * k), int(x1 * k)], 0, w * k - 1), np.clip([int(y0 * k), int(y1 * k)], 0, h * k - 1)
        for p in (pa, pb):
            for yy in ys:
                p[:, yy, xs[0]:xs[1] + 1] = np.array([0.0, 1.0, 0.0])[:, None]
            for xx in xs:
                p[:, ys[0]:ys[1] + 1, xx] = np.array([0.0, 1.0, 0.0])[:, None]
    t = res["t"]
    idx = np.abs(t).argmax(axis=0)
    ts = np.take_along_axis(t, idx[None], axis=0)[0]
    g = np.clip(ts / 4.0, -1.0, 1.0)
    tmap = np.stack([np.where(g > 0, 1.0, 1.0 + g), 1.0 - np.abs(g), np.where(g < 0, 1.0, 1.0 - g)])
    tmap = tmap.repeat(tile * k, axis=1).repeat(tile * k, axis=2)
    rej = res["reject"].any(axis=0)
    for ty, tx in zip(*np.nonzero(rej)):
        y0, x0 = ty * tile * k, tx * tile * k
        tmap[:, y0:y0 + tile * k, [x0, x0 + tile * k - 1]] = 0.0
        tmap[:, [y0, y0 + tile * k - 1], x0:x0 + tile * k] = 0.0
    gap = np.zeros((3, h * k, 2 * k))
    comp = np.concatenate([pa, gap, pb, gap, tmap], axis=2)
    lin = np.where(comp <= 0.04045, comp / 12.92, ((comp + 0.055) / 1.055) ** 2.4)
    an.write_png(path, lin, 1.0, enlarge=1)


def history_line(path, scene, device, ratio, per, n, spp):
    sha = subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True, text=True, cwd=ROOT).stdout.strip()
    line = {"sha": sha, "machine": machine_id.machine_id(), "driver": "unknown", "cuda": "none" if device == "cpu" else "cuda",
            "scene": scene, "metric": f"tree_alias_var_ratio.{device}", "unit": "ratio", "value": round(ratio, 4),
            "spread": round(float(per.std(ddof=1)), 4), "n": n, "spp": spp,
            "date": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")}
    with open(path, "a", newline="\n") as f:
        f.write(json.dumps(line, separators=(",", ":")) + "\n")


def run_compare(scene, device, png, history):
    t0 = time.perf_counter()
    an, rec, size, spp, tile, n = scene_plan(scene)
    tree = render_side(an, rec, device, spp, size, "tree", [f"welch_tiles:{scene}:tree:{i}" for i in range(n)])
    alias = render_side(an, rec, device, spp, size, "alias", [f"welch_tiles:{scene}:alias:{i}" for i in range(n)])
    res = compare([(s, m) for s, m, _ in tree], [(s, m) for s, m, _ in alias], n, tile)
    for line in rejected_lines(res, tile):
        print(line)
    ratio, per, per_ch = variance_ratio(tree, alias)
    rejected = int(res["reject"].sum())
    t = res["t"]
    print(f"tile t (plain Welch, no margin) per channel: " + "; ".join(
        f"{'RGB'[c]} mean {t[c].mean():+.3f} sd {t[c].std():.3f} max |t| {np.abs(t[c]).max():.2f}" for c in range(3)))
    print(f"variance ratio alias/tree {device}: {ratio:.3f} (per seed pair sd {per.std(ddof=1):.3f}, range "
          f"{per.min():.3f}-{per.max():.3f}; R {per_ch[0]:.3f} G {per_ch[1]:.3f} B {per_ch[2]:.3f}) against >= {RATIO_MIN}")
    if png:
        write_evidence(png, scene, res, tree, alias, tile)
        print(f"evidence: {png}")
    if history:
        history_line(history, scene, device, ratio, per, n, spp)
    ok = rejected == 0 and ratio >= RATIO_MIN
    print(f"{'PASS' if ok else 'FAIL'} welch_tiles {scene} {device} tree~alias: {rejected} of {res['p'].size} "
          f"(tile, channel) rejected after Holm at {ALPHA} with {MARGIN:.0%} margin, variance ratio {ratio:.3f}, "
          f"{size}x{size} at {spp} spp, {tile}x{tile} tiles, {n} seeds a side, {time.perf_counter() - t0:.1f} s; "
          f"reproduce: <python> -I tools/welch_tiles.py --scene {scene} --device {device}")
    return ok


def run_null(scene, device, repeats):
    t0 = time.perf_counter()
    an, rec, size, spp, tile, n = scene_plan(scene)
    failing, smallest = 0, []
    for r in range(repeats):
        a = render_side(an, rec, device, spp, size, "tree", [f"welch_tiles:null:{scene}:{r}:a:{i}" for i in range(n)])
        b = render_side(an, rec, device, spp, size, "tree", [f"welch_tiles:null:{scene}:{r}:b:{i}" for i in range(n)])
        res = compare([(s, m) for s, m, _ in a], [(s, m) for s, m, _ in b], n, tile)
        k = int(res["reject"].sum())
        failing += k > 0
        smallest.append(float(res["p"].min()))
        print(f"null {scene} {device} repeat {r + 1}/{repeats}: {k} rejected, smallest p {res['p'].min():.3g} "
              f"(Holm's first bound {ALPHA / res['p'].size:.3g})")
        for line in rejected_lines(res, tile):
            print("  " + line)
    ok = failing <= NULL_MAX_FAILING
    print(f"{'PASS' if ok else 'FAIL'} welch_tiles null {scene} {device}: {failing} of {repeats} repeats rejected a tile "
          f"(at most {NULL_MAX_FAILING}), {n} vs {n} tree seeds, {size}x{size} at {spp} spp, {tile}x{tile} tiles, "
          f"median smallest p {float(np.median(smallest)):.3g}, {time.perf_counter() - t0:.0f} s")
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 epilog="Usage and the decision rules are in the module docstring (tools/welch_tiles.py).")
    ap.add_argument("--self-test", action="store_true", help="synthetic sets, t tail, Holm, fail-closed checks")
    ap.add_argument("--scene", help="a scene with [truth] size, spp, tile and seeds (d10_many_lights)")
    ap.add_argument("--device", choices=("cpu", "cuda"))
    ap.add_argument("--null", type=int, metavar="REPEATS", help="null calibration: tree against tree, REPEATS times")
    ap.add_argument("--png", help="evidence image: tree mean, alias mean, tile t map")
    ap.add_argument("--history", help="append the variance-ratio line to this jsonl")
    a = ap.parse_args()
    if a.self_test:
        return 0 if self_test() else 1
    if not (a.scene and a.device):
        ap.error("give --self-test, or --scene and --device")
    if a.null:
        return 0 if run_null(a.scene, a.device, a.null) else 1
    return 0 if run_compare(a.scene, a.device, a.png, a.history) else 1


if __name__ == "__main__":
    sys.exit(main())
