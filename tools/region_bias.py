# SPDX-License-Identifier: Apache-2.0
"""Region-mean bias (task 222): does the VAR plane give the error of a many-pixel mean, and is a
region mean unbiased? Renders scenes/a5_emitter (a view-filling constant emitter, black world)
through `astroray_cli dump` for S seeds, reads the display mean and VAR planes and, per channel and
per split of the image (left/right, top/bottom halves), prints
  - the half-difference d_s = mean(half A) - mean(half B) in standard errors from VAR for each seed
    (SE_s^2 = sum VAR_A / n^2 + sum VAR_B / n^2, n pixels per half: 0008 d3, pixels independent);
  - var ratio = sample variance of d_s across seeds / mean_s SE_s^2, with its chi-squared interval
    for S seeds (1 means VAR is the error of a region mean; above 1 means pixels correlate);
  - bias = mean_s d_s with its standard error from the seeds, in those standard errors (z) and in
    SE from VAR (a positional bias shows as |z| beyond 3).
Usage: <python> -I tools/region_bias.py [--device cpu|cuda] [--sampler independent|sobol|both]
         [--seeds 64] [--spp 64] [--size 256] [--evidence DIR] [--history FILE] [--self-test]
Ref: textbook statistics. The variance of the mean of n independent pixel means is sum VAR / n^2;
(S - 1) s^2 / sigma^2 is chi-squared with S - 1 degrees of freedom for normal d_s, so the interval of the
ratio's true value is [(S-1) r / q_hi, (S-1) r / q_lo]; quantiles by the Wilson-Hilferty
cube-root normal approximation (Wilson and Hilferty 1931, PNAS 17:684), checked in the self-test
against the CDF (Abramowitz and Stegun 6.5.29) at S - 1 = 63.
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
CLI = os.path.join(ROOT, "out", "bin", "astroray_cli.exe")
FITS_READ = os.path.join(ROOT, "tests", "abi", "fits_read.py")
SCENE = "a5_emitter"
CHANNELS = ("red", "green", "blue")
SPLITS = ("left-right", "top-bottom")
Z_BIAS_LIMIT = 3.0  # the decision table's 3 standard errors
Z975 = 1.959963984540054  # normal 97.5 percent point


def chi2_quantile(p, dof):
    """Wilson and Hilferty 1931: chi2_p ~ dof (1 - 2/(9 dof) + z_p sqrt(2/(9 dof)))^3."""
    z = {0.025: -Z975, 0.975: Z975, 0.5: 0.0}[p]
    a = 2.0 / (9.0 * dof)
    return dof * (1.0 - a + z * np.sqrt(a)) ** 3


def split_halves(plane, split):
    """The two halves of (..., h, w) planes: columns for left-right, rows for top-bottom."""
    h, w = plane.shape[-2:]
    if split == "left-right":
        return plane[..., :, : w // 2], plane[..., :, w // 2:]
    return plane[..., : h // 2, :], plane[..., h // 2:, :]


def analyse(mean, var, split):
    """mean, var: (S, 3, h, w). Returns a dict per channel: z per seed, ratio, interval, bias."""
    S = mean.shape[0]
    ma, mb = split_halves(mean, split)
    va, vb = split_halves(var, split)
    n = ma.shape[-1] * ma.shape[-2]
    d = ma.mean(axis=(-2, -1)) - mb.mean(axis=(-2, -1))  # (S, 3)
    se2 = va.sum(axis=(-2, -1)) / n ** 2 + vb.sum(axis=(-2, -1)) / n ** 2
    out = []
    for c in range(3):
        sd2 = d[:, c].var(ddof=1)
        ratio = sd2 / se2[:, c].mean()
        lo = (S - 1) * ratio / chi2_quantile(0.975, S - 1)
        hi = (S - 1) * ratio / chi2_quantile(0.025, S - 1)
        bias = d[:, c].mean()
        bias_se = np.sqrt(sd2 / S)
        out.append({"z_seed": d[:, c] / np.sqrt(se2[:, c]), "ratio": ratio, "lo": lo, "hi": hi, "bias": bias,
                    "bias_se": bias_se, "bias_z": bias / bias_se if bias_se > 0 else 0.0,
                    "bias_in_var_se": bias / np.sqrt(se2[:, c].mean()) if se2[:, c].mean() > 0 else 0.0})
    return out


def row(split, c, r, sampler):
    """One report line. Class: the decision table's row for this split and channel."""
    inside = r["lo"] <= 1.0 <= r["hi"]
    unbiased = abs(r["bias_z"]) <= Z_BIAS_LIMIT
    cls = "(c) positional bias" if not unbiased else ("(a) VAR holds" if inside else
          ("(b) correlated: VAR too small" if r["lo"] > 1.0 else "VAR too large"))
    zs = r["z_seed"]
    return (f"{sampler:11s} {split:10s} {CHANNELS[c]:5s} z/seed mean {zs.mean():+6.2f} rms {np.sqrt((zs ** 2).mean()):5.2f} | "
            f"var ratio {r['ratio']:6.3f} [{r['lo']:6.3f}, {r['hi']:6.3f}] | bias {r['bias']:+.3e} +- {r['bias_se']:.1e} "
            f"= {r['bias_z']:+5.2f} seed-SE, {r['bias_in_var_se']:+5.2f} VAR-SE | {cls}")


def report(mean, var, sampler):
    lines, results = [], {}
    for split in SPLITS:
        for c, r in enumerate(analyse(mean, var, split)):
            lines.append(row(split, c, r, sampler))
            results[(split, c)] = r
    return lines, results


def load_planes(path, spp, device, size, sampler):
    spec = importlib.util.spec_from_file_location("_fits_read_region", FITS_READ)
    fr = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fr)
    with open(path, "rb") as f:
        hdus = fr.read_hdus(f.read())
    head, planes = hdus[0][0], {h["EXTNAME"]: d for h, d in hdus[1:]}
    for key, want in (("SAMPLER", sampler), ("DEVICE", device), ("SPP", spp)):
        if head.get(key) != want:
            raise RuntimeError(f"the dump's {key} is {head.get(key)!r}, asked for {want!r}")
    mean = np.stack([planes[f"display.{c}"] for c in range(3)]).astype(np.float64)
    var = np.stack([planes[f"display.{c}.VAR"] for c in range(3)]).astype(np.float64)
    nsamp = np.stack([planes[f"display.{c}.NSAMP"] for c in range(3)])
    if mean.shape != (3, size, size) or not (nsamp == spp).all():
        raise RuntimeError(f"the dump is {mean.shape[1:]} with NSAMP {np.unique(nsamp)}, expected {size}x{size} at {spp}")
    return mean, var


def build_recording(path, sampler):
    """The A5 recording with the sampler mode replaced: the builder fixes `independent`, the scene
    and everything else stay the committed ones."""
    scenes = os.path.join(ROOT, "scenes")
    sys.path.insert(0, os.path.join(scenes, "lib"))
    import ar  # noqa: E402
    spec = importlib.util.spec_from_file_location("_a5_build", os.path.join(scenes, SCENE, "build.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    orig = ar.Scene.settings

    def settings(self, *a, **kw):
        kw["sampler"] = sampler
        return orig(self, *a, **kw)

    ar.Scene.settings = settings
    try:
        mod.build(path)
    finally:
        ar.Scene.settings = orig


def render_all(sampler, device, seeds, spp, size, deadline):
    tmp = tempfile.mkdtemp(prefix="region_bias_")
    rec = os.path.join(tmp, f"{SCENE}_{sampler}.arrec")
    build_recording(rec, sampler)
    means, vars_ = [], []
    for s in range(1, seeds + 1):
        if time.perf_counter() > deadline:
            raise RuntimeError(f"capture passed its time limit after {s - 1} of {seeds} seeds")
        out = os.path.join(tmp, "dump.fits")
        r = subprocess.run([CLI, "dump", rec, "-o", out, "--device", device, "--spp", str(spp), "--seed", str(s),
                            "--size", f"{size}x{size}"], capture_output=True, text=True, cwd=tmp, timeout=1200)
        if r.returncode != 0:
            raise RuntimeError(f"astroray_cli exited {r.returncode}\n{r.stdout}{r.stderr}")
        m, v = load_planes(out, spp, device, size, sampler)
        means.append(m)
        vars_.append(v)
    return np.stack(means), np.stack(vars_)


def write_signed_png(path, plane):
    """plane (h, w), signed: mid grey is zero, full scale is the largest |value|. Rows bottom-up as
    the film stores them. Ref: PNG ISO/IEC 15948 (written through tools/analytic.py's writer)."""
    spec = importlib.util.spec_from_file_location("_analytic", os.path.join(ROOT, "tools", "analytic.py"))
    an = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(an)
    m = float(np.abs(plane).max()) or 1.0
    g = 0.5 + 0.5 * plane / m
    # analytic.write_png applies the sRGB curve; undo it so grey levels stay linear in the value.
    lin = np.where(g <= 0.04045, g / 12.92, ((g + 0.055) / 1.055) ** 2.4)
    an.write_png(path, np.stack([lin, lin, lin]), 1.0, enlarge=2)
    return m


def map_stats(mean, c=2):
    """Per-pixel mean over seeds minus the image mean, one channel: row and column mean statistics."""
    pm = mean[:, c].mean(axis=0)
    dev = pm - pm.mean()
    return dev, dev.mean(axis=1), dev.mean(axis=0)


def history_lines(results, sampler, device, S):
    sha = subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True, text=True, cwd=ROOT).stdout.strip()
    date = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    lines = []
    for (split, c), r in sorted(results.items()):
        base = {"sha": sha, "machine": machine_id.machine_id(), "driver": "unknown", "cuda": "none" if device == "cpu" else "cuda",
                "scene": f"{SCENE}.{split}", "unit": "ratio", "n": S, "date": date}
        spread = (r["hi"] - r["lo"]) / 2
        lines.append({**base, "metric": f"region_var_ratio.{CHANNELS[c]}.{sampler}.{device}", "value": float(r["ratio"]),
                      "spread": float(spread)})
        lines.append({**base, "metric": f"region_bias_se.{CHANNELS[c]}.{sampler}.{device}", "unit": "se",
                      "value": float(r["bias_z"]), "spread": 1.0})
    return lines


def self_test():
    """Synthetic (S, 3, h, w) planes with known correlation: independent pixels give a ratio interval
    holding 1 and no bias; a shared per-half term (variance k x the independent part) gives ratio 1 + k;
    an offset on one half shows as a bias beyond 3 seed-SE; the VAR a plane reports is the independent part."""
    fails = []
    # The quantiles against the chi-squared CDF, the regularised lower incomplete gamma P(dof/2, x/2)
    # by its power series (Abramowitz and Stegun 6.5.29), summed until the terms vanish.
    def cdf(x, dof):
        a, x = dof / 2.0, x / 2.0
        term = total = 1.0 / a
        for k in range(1, 2000):
            term *= x / (a + k)
            total += term
            if term < 1e-14 * total:
                break
        return total * math.exp(-x + a * math.log(x) - math.lgamma(a))

    for p in (0.025, 0.5, 0.975):
        if abs(cdf(chi2_quantile(p, 63), 63) - p) > 2e-3:
            fails.append(f"chi2 quantile {p}: CDF there is {cdf(chi2_quantile(p, 63), 63):.4f}")
    # A fixed draw; each interval holds the truth with 95 percent probability, so some seed misses one of nine.
    # The draw depends on numpy's Generator stream, which is not guaranteed stable across numpy versions:
    # a new numpy that turns this red may need another seed, not a code fix (check the null calibration first).
    rng = np.random.default_rng(2)
    S, h, w, sigma = 64, 32, 32, 0.1
    n = h * w // 2

    def planes(k, offset):
        noise = rng.normal(0.0, sigma, (S, 3, h, w))
        tau = sigma * np.sqrt(k / n)  # k: extra variance of a half mean, in units of its independent variance
        noise[..., :, : w // 2] += rng.normal(0.0, tau, (S, 3, 1, 1))
        noise[..., :, w // 2:] += rng.normal(0.0, tau, (S, 3, 1, 1))
        noise[..., :, : w // 2] += offset
        return 1.0 + noise, np.full((S, 3, h, w), sigma ** 2)

    # offset in seed-SE of the half difference: SE_bias = sqrt(2 sigma^2 / n / S) for independent pixels.
    se_bias = sigma * math.sqrt(2.0 / n / S)
    cases = ((0.0, 0.0, 1.0, False), (3.0, 0.0, 4.0, False), (0.0, 0.02, 1.0, True),
             (0.0, 4.5 * se_bias, 1.0, True), (0.0, 1.0 * se_bias, 1.0, False))
    for k, offset, expect_ratio, biased in cases:
        m, v = planes(k, offset)
        for c, r in enumerate(analyse(m, v, "left-right")):
            if not (r["lo"] <= expect_ratio <= r["hi"]):
                fails.append(f"k={k} offset={offset} channel {c}: ratio {r['ratio']:.3f} [{r['lo']:.3f}, {r['hi']:.3f}] misses {expect_ratio}")
            if k == 3.0 and r["lo"] <= 1.0:
                fails.append(f"k=3 channel {c}: interval [{r['lo']:.3f}, {r['hi']:.3f}] holds 1")
            if biased != (abs(r["bias_z"]) > Z_BIAS_LIMIT):
                fails.append(f"k={k} offset={offset} channel {c}: bias z {r['bias_z']:+.2f}, biased={biased}")
            if k == 0.0 and not (0.75 * se_bias <= r["bias_se"] <= 1.25 * se_bias):
                # The sample sd of 64 values has a relative sd of 1/sqrt(2 x 63) = 9 percent: 25 percent is 2.8 sigma.
                fails.append(f"k={k} offset={offset} channel {c}: bias_se {r['bias_se']:.3e} vs expected {se_bias:.3e}")
    # Null calibration: bias_z of unbiased planes is a Student t with S - 1 = 63 dof, variance 63/61, and
    # z^2 has variance about 2 (63/61)^2 x (1 + 6/(63 - 4)) ~ 2.4. Over R replicates the mean z^2 must lie within
    # 4 standard errors of 63/61: R = 300, so 1.033 +- 4 sqrt(2.4 / 300) = +-0.36.
    reps = 100
    z2 = []
    for _ in range(reps):
        noise = rng.normal(0.0, sigma, (S, 3, 8, 8))
        z2 += [r["bias_z"] ** 2 for r in analyse(1.0 + noise, np.full((S, 3, 8, 8), sigma ** 2), "left-right")]
    bound = 4.0 * math.sqrt(2.4 / len(z2))
    if abs(np.mean(z2) - 63.0 / 61.0) > bound:
        fails.append(f"null bias z: mean z^2 {np.mean(z2):.3f} vs {63 / 61:.3f} +- {bound:.2f} over {len(z2)} replicates")
    for f in fails:
        print("FAIL region_bias self-test:", f)
    if not fails:
        print("PASS region_bias self-test: ratio 1 and 4 recovered, bias detected near the threshold, null bias z calibrated, chi2 quantiles against the CDF")
    return not fails


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--device", default="cpu", choices=("cpu", "cuda"))
    ap.add_argument("--sampler", default="both", choices=("independent", "sobol", "both"))
    ap.add_argument("--seeds", type=int, default=64)
    ap.add_argument("--spp", type=int, default=64)
    ap.add_argument("--size", type=int, default=256)
    ap.add_argument("--evidence", help="directory for half_difference.png (blue, first sampler run)")
    ap.add_argument("--history", help="append the history lines to this jsonl")
    ap.add_argument("--limit-min", type=float, default=20.0, help="stop the capture after this many minutes")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        sys.exit(0 if self_test() else 1)
    if not os.path.exists(CLI):
        raise FileNotFoundError(f"{CLI} missing: run tools/build.ps1 first")
    t0 = time.perf_counter()
    deadline = t0 + a.limit_min * 60
    samplers = ("independent", "sobol") if a.sampler == "both" else (a.sampler,)
    new_history = []
    for i, sampler in enumerate(samplers):
        t1 = time.perf_counter()
        mean, var = render_all(sampler, a.device, a.seeds, a.spp, a.size, deadline)
        lines, results = report(mean, var, sampler)
        print(f"# {SCENE} {a.device} {sampler}: {a.seeds} seeds, {a.size}x{a.size} at {a.spp} spp, {time.perf_counter() - t1:.0f} s wall")
        print("\n".join(lines))
        dev, rm, cm = map_stats(mean)
        pix_se = float(np.sqrt(var[:, 2].mean() / a.seeds))
        print(f"# {sampler} blue map: pixel SE of the seed mean {pix_se:.3e}; |row mean| max {np.abs(rm).max():.3e} "
              f"(row-mean SE {pix_se / np.sqrt(a.size):.3e}), |column mean| max {np.abs(cm).max():.3e}; "
              f"left-right {cm[:a.size // 2].mean() - cm[a.size // 2:].mean():+.3e}, top-bottom {rm[:a.size // 2].mean() - rm[a.size // 2:].mean():+.3e}")
        if a.evidence and i == 0:
            os.makedirs(a.evidence, exist_ok=True)
            top = write_signed_png(os.path.join(a.evidence, "half_difference.png"), dev)
            np.save(os.path.join(a.evidence, "half_difference.npy"), dev)
            print(f"# half_difference.png: full scale +-{top:.3e}")
        new_history += history_lines(results, sampler, a.device, a.seeds)
    if a.history:
        with open(a.history, "a", newline="\n") as f:
            f.writelines(json.dumps(h, separators=(",", ":")) + "\n" for h in new_history)
    print(f"# total {time.perf_counter() - t0:.0f} s")


if __name__ == "__main__":
    main()
