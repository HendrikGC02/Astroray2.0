# SPDX-License-Identifier: Apache-2.0
"""Hero-lane variance (0003 Risk 11): the random hero of 0003 d6 against a hero pinned to lane 0
(pbrt-v4's choice, the gate hook ASTRORAY_TEST_HERO=lane0) at equal samples. Renders the chromatic
absorbing slab, the chromatic scattering slab (scenes/v1_slab, --scattering) and the surface-only control
a1_furnace through `astroray_cli dump`, S seeds per mode, each mode on its own seeds, and prints:
  - the means check: per scene, sampler and channel, the image means of the two modes within 3 SE
    (the lane-0 mode is unbiased only while nothing terminates secondaries); any miss voids the run.
    The Sobol column's SE comes from S seeds, so its limit is the t(S - 1) quantile with the two-sided
    probability of a normal 3 SE (lead ruling);
  - per channel V_random / V_lane0 with its 2.5-97.5 % bootstrap interval over seeds (10,000 resamples,
    seeded). Independent sampler: V = image mean of the per-pixel variance of the mean (VAR plane,
    exact in that mode, 0008 d3). Sobol (the production sampler): V = image mean of the across-seed
    variance of the per-pixel mean. The slab's medium half (left) is printed beside the whole image;
  - one CUDA render per mode of each slab (independent sampler) against the CPU's single-seed ratios;
  - pass_ms of both modes on the CPU (`astroray_cli bench`), to show the time per sample is the same.
First, a positive control: one seed of the scattering slab in both modes must differ, or the hook never
reached the kernel. The independent column's ratio does not depend on spp (V scales as 1/spp in both
modes); the Sobol column's number holds at its spp. The defaults reproduce the recorded run in 10 min.
Usage: <python> -I tools/hero_variance.py [--seeds 16] [--spp 256] [--sobol-spp 128] [--size 64]
         [--no-cuda] [--evidence out/evidence/194] [--history FILE] [--limit-min 10]
Ref: Efron and Tibshirani 1993, An Introduction to the Bootstrap, ch. 13 (percentile interval); Student's
t density (Abramowitz and Stegun 26.7.1), integrated numerically for its quantile;
docs/decisions/0003-spectral-model-and-path-state.md d2, d6, Risk 11; 0008 d3 (VAR).
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
CLI = os.path.join(ROOT, "out", "bin", "astroray_cli.exe")
FITS_READ = os.path.join(ROOT, "tests", "abi", "fits_read.py")
SCENES = (("v1_slab.absorbing", "v1_slab", {}), ("v1_slab.scattering", "v1_slab", {"scattering": True}),
          ("a1_furnace", "a1_furnace", {}))
MODES = ("random", "lane0")
CHANNELS = ("red", "green", "blue")
Z_MEANS = 3.0          # the decision table's 3 SE
BOOT, BOOT_SEED = 10000, 194
SEED_BASE = {"random": 1, "lane0": 1001}  # disjoint seeds: the modes are independent renders
CUDA_SEED = {"random": 9001, "lane0": 10001}


def t_limit(z, dof):
    """The t(dof) point with the two-sided tail probability of a normal z: the t density (Abramowitz and
    Stegun 26.7.1) integrated by the trapezoid rule on a fine grid, then interpolated."""
    p = math.erfc(z / math.sqrt(2.0))
    t = np.linspace(0.0, 40.0, 400001)
    c = math.lgamma((dof + 1) / 2) - math.lgamma(dof / 2) - 0.5 * math.log(dof * math.pi)
    pdf = np.exp(c - (dof + 1) / 2 * np.log1p(t * t / dof))
    cdf = np.concatenate([[0.0], np.cumsum((pdf[1:] + pdf[:-1]) / 2 * (t[1] - t[0]))])  # integral from 0
    return float(np.interp((1.0 - p) / 2.0, cdf, t))  # two-sided tail beyond t is 1 - 2 cdf(t)


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def build_recording(path, scene, kwargs, sampler):
    """The committed builder with the sampler mode replaced; everything else is the scene's own."""
    sys.path.insert(0, os.path.join(ROOT, "scenes", "lib"))
    import ar  # noqa: E402
    mod = load_module(f"_build_{scene}", os.path.join(ROOT, "scenes", scene, "build.py"))
    orig = ar.Scene.settings

    def settings(self, *a, **kw):
        kw["sampler"] = sampler
        return orig(self, *a, **kw)

    ar.Scene.settings = settings
    try:
        mod.build(path, **kwargs)
    finally:
        ar.Scene.settings = orig


def run_cli(args, mode, tmp):
    env = {k: v for k, v in os.environ.items() if k != "ASTRORAY_TEST_HERO"}
    if mode == "lane0":
        env["ASTRORAY_TEST_HERO"] = "lane0"
    r = subprocess.run([CLI, *args], capture_output=True, text=True, cwd=tmp, timeout=1200, env=env)
    if r.returncode != 0:
        raise RuntimeError(f"astroray_cli {args[0]} exited {r.returncode} ({mode})\n{r.stdout}{r.stderr}")
    # The hook is visible in the session's report (TEST_HOOK_ACTIVE) exactly when it was asked for.
    if ("HERO=lane0" in r.stdout) != (mode == "lane0"):
        raise RuntimeError(f"mode {mode}: the CLI's report {'lacks' if mode == 'lane0' else 'has'} HERO=lane0\n{r.stdout}")
    return r.stdout


def render(fr, rec, device, spp, seed, size, mode, sampler, tmp):
    out = os.path.join(tmp, "dump.fits")
    run_cli(["dump", rec, "-o", out, "--device", device, "--spp", str(spp), "--seed", str(seed), "--size", f"{size}x{size}"],
            mode, tmp)
    with open(out, "rb") as f:
        hdus = fr.read_hdus(f.read())
    head, planes = hdus[0][0], {h["EXTNAME"]: d for h, d in hdus[1:]}
    for key, want in (("SAMPLER", sampler), ("DEVICE", device), ("SPP", spp), ("SEED", seed)):
        if head.get(key) != want:
            raise RuntimeError(f"the dump's {key} is {head.get(key)!r}, asked for {want!r}")
    mean = np.stack([planes[f"display.{c}"] for c in range(3)]).astype(np.float64)
    var = np.stack([planes[f"display.{c}.VAR"] for c in range(3)]).astype(np.float64)
    if mean.shape != (3, size, size):
        raise RuntimeError(f"the dump is {mean.shape[1:]}, expected {size}x{size}")
    return mean, var


def boot_counts(rng, S):
    """BOOT x S multiplicities of a resample of S seeds with replacement."""
    idx = rng.integers(0, S, (BOOT, S))
    return np.stack([np.bincount(row, minlength=S) for row in idx]).astype(np.float64)


def v_independent(var, w):
    """var (S, 3, h, w) -> V per resample (B, 3): weighted seed mean of the image-mean VAR."""
    per_seed = var.mean(axis=(-2, -1))  # (S, 3)
    return w @ per_seed / w.sum(axis=1, keepdims=True)


def v_sobol(mean, w):
    """mean (S, 3, h, w) -> V per resample (B, 3): image mean of the across-seed variance (ddof 1)
    of the per-pixel mean, the resampled seeds counted with their multiplicity."""
    S = mean.shape[0]
    x = mean.reshape(S, 3, -1)
    out = np.empty((w.shape[0], 3))
    for c in range(3):
        for b0 in range(0, w.shape[0], 1000):
            wb = w[b0:b0 + 1000]
            m1, m2 = wb @ x[:, c] / S, wb @ (x[:, c] ** 2) / S
            out[b0:b0 + 1000, c] = ((m2 - m1 ** 2) * S / (S - 1)).mean(axis=1)
    return out


def ratio_with_interval(data, sampler, rng, cols=None):
    """data[mode] = (mean, var), each (S, 3, h, w); cols restricts to a column range. Returns per
    channel (ratio, lo, hi) of V_random / V_lane0."""
    vs = {}
    for mode in MODES:
        m, v = data[mode]
        if cols is not None:
            m, v = m[..., cols], v[..., cols]
        S = m.shape[0]
        full = np.ones((1, S))
        f = v_independent if sampler == "independent" else v_sobol
        vs[mode] = (f(v if sampler == "independent" else m, full)[0], f(v if sampler == "independent" else m, boot_counts(rng, S)))
    point = vs["random"][0] / vs["lane0"][0]
    boot = vs["random"][1] / vs["lane0"][1]
    lo, hi = np.percentile(boot, [2.5, 97.5], axis=0)
    return [(float(point[c]), float(lo[c]), float(hi[c])) for c in range(3)]


def means_check(data, sampler):
    """Per channel (z, mean_random, mean_lane0) of the image means; SE from VAR (independent) or from
    the seeds' spread (Sobol)."""
    stats = {}
    for mode in MODES:
        m, v = data[mode]
        S, n = m.shape[0], m.shape[-1] * m.shape[-2]
        img = m.mean(axis=(-2, -1))  # (S, 3)
        se2 = (v.sum(axis=(-2, -1)) / n ** 2).mean(axis=0) / S if sampler == "independent" else img.var(axis=0, ddof=1) / S
        stats[mode] = (img.mean(axis=0), se2)
    (ma, sa), (mb, sb) = stats["random"], stats["lane0"]
    return [(float((ma[c] - mb[c]) / np.sqrt(sa[c] + sb[c])), float(ma[c]), float(mb[c])) for c in range(3)]


def write_maps(path, data, write_png):
    """Scattering slab, independent sampler: seed-mean VAR (channel mean) of both modes on one scale,
    then their ratio (mid grey 1, black 0, white 2); two black columns between panels."""
    vr, v0 = (data[m][1].mean(axis=(0, 1)) for m in MODES)
    top = max(vr.max(), v0.max()) or 1.0
    ratio = np.clip(vr / np.where(v0 > 0, v0, np.inf) / 2.0, 0.0, 1.0)
    gap = np.zeros((vr.shape[0], 2))
    g = np.concatenate([vr / top, gap, v0 / top, gap, ratio], axis=1)
    lin = np.where(g <= 0.04045, g / 12.92, ((g + 0.055) / 1.055) ** 2.4)  # write_png re-encodes to sRGB
    write_png(path, np.stack([lin, lin, lin]), 1.0, enlarge=4)
    r = vr / v0
    h, w = r.shape
    cm = r.mean(axis=0)
    print(f"# maps: VAR full scale {top:.3e}; ratio map mean {r.mean():.4f} sd {r.std():.4f}; medium half {r[:, :w // 2].mean():.4f}, "
          f"direct half {r[:, w // 2:].mean():.4f}, edge columns {cm[w // 2 - 1]:.4f} {cm[w // 2]:.4f}; column-mean sd "
          f"{cm.std():.4f} against {r.std() / np.sqrt(h):.4f} for structureless pixels")


def pass_ms(rec, mode, tmp):
    out = run_cli(["bench", rec, "--device", "cpu", "--passes", "72", "--warmup", "8", "--size", "64x64"], mode, tmp)
    line = json.loads(next(l for l in out.splitlines() if l.startswith("{") and '"pass_ms"' in l))
    return line["value"], line["spread"]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0], formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    ap.add_argument("--seeds", type=int, default=16, help="seeds per mode")
    ap.add_argument("--spp", type=int, default=256, help="independent column spp")
    ap.add_argument("--sobol-spp", type=int, default=128, help="Sobol column spp")
    ap.add_argument("--size", type=int, default=64, help="image width and height")
    ap.add_argument("--no-cuda", action="store_true")
    ap.add_argument("--evidence", help="directory for hero_variance.png")
    ap.add_argument("--history", help="append the hero_variance_ratio lines to this jsonl")
    ap.add_argument("--limit-min", type=float, default=10.0, help="capture time limit in minutes")
    a = ap.parse_args()
    if not os.path.exists(CLI):
        raise FileNotFoundError(f"{CLI} missing: run tools/build.ps1 first")
    t0 = time.perf_counter()
    deadline = t0 + a.limit_min * 60
    fr = load_module("_fits_read_hero", FITS_READ)
    tmp = tempfile.mkdtemp(prefix="hero_variance_")
    recs, results = {}, {}
    # Positive control: the same seed in both modes renders different images, or the hook is dead.
    control = os.path.join(tmp, "control.arrec")
    build_recording(control, "v1_slab", {"scattering": True}, "independent")
    pair = [render(fr, control, "cpu", 16, 1, a.size, mode, "independent", tmp)[0] for mode in MODES]
    differ = int(np.count_nonzero(pair[0] != pair[1]))
    print(f"# control: v1_slab.scattering seed 1 at 16 spp, random against lane0: {differ} of {pair[0].size} values differ")
    if differ == 0:
        print("FAIL hero_variance: the lane-0 hook changed nothing; it does not reach the kernel")
        sys.exit(1)
    for sampler, spp in (("independent", a.spp), ("sobol", a.sobol_spp)):
        t1 = time.perf_counter()
        for name, scene, kwargs in SCENES:
            rec = recs[(name, sampler)] = os.path.join(tmp, f"{name}.{sampler}.arrec")
            build_recording(rec, scene, kwargs, sampler)
            data = {}
            for mode in MODES:
                ms, vs = [], []
                for s in range(a.seeds):
                    if time.perf_counter() > deadline:
                        raise RuntimeError(f"capture passed its {a.limit_min} min limit at {name} {sampler} {mode} seed {s}")
                    m, v = render(fr, rec, "cpu", spp, SEED_BASE[mode] + s, a.size, mode, sampler, tmp)
                    ms.append(m)
                    vs.append(v)
                data[mode] = (np.stack(ms), np.stack(vs))
            results[(name, sampler)] = data
        print(f"# cpu {sampler}: {len(SCENES)} scenes x 2 modes x {a.seeds} seeds, {a.size}x{a.size} at {spp} spp, "
              f"{time.perf_counter() - t1:.0f} s wall")
    void = False
    limits = {"independent": Z_MEANS, "sobol": t_limit(Z_MEANS, a.seeds - 1)}
    print(f"# means limits: independent {Z_MEANS:g} SE (exact VAR); sobol {limits['sobol']:.3f} SE, the t({a.seeds - 1}) "
          f"point with the two-sided probability {math.erfc(Z_MEANS / math.sqrt(2.0)):.4g} of a normal {Z_MEANS:g} SE")
    for (name, sampler), data in results.items():
        for c, (z, mr, m0) in enumerate(means_check(data, sampler)):
            ok = abs(z) <= limits[sampler]
            void |= not ok
            print(f"means {name:18s} {sampler:11s} {CHANNELS[c]:5s} random {mr:.6f} lane0 {m0:.6f} z {z:+5.2f} {'ok' if ok else 'FAIL'}")
    if void:
        print(f"FAIL hero_variance: the modes' means differ beyond their limits; no ratio is reported "
              f"({time.perf_counter() - t0:.0f} s)")
        sys.exit(1)
    rng = np.random.default_rng(BOOT_SEED)
    hist = []
    for (name, sampler), data in results.items():
        half = slice(0, a.size // 2) if name.startswith("v1_slab") else None
        whole = ratio_with_interval(data, sampler, rng)
        part = ratio_with_interval(data, sampler, rng, half) if half else None
        for c, (r, lo, hi) in enumerate(whole):
            verdict = "above 1: stop and report" if lo > 1.0 else ("below 1" if hi < 1.0 else "holds 1")
            extra = f" | medium half {part[c][0]:.4f} [{part[c][1]:.4f}, {part[c][2]:.4f}]" if part else ""
            print(f"ratio {name:18s} {sampler:11s} {CHANNELS[c]:5s} V_random/V_lane0 {r:.4f} [{lo:.4f}, {hi:.4f}] {verdict}{extra}")
            hist.append({"scene": name, "metric": f"hero_variance_ratio.{CHANNELS[c]}.{sampler}", "value": r,
                         "unit": "ratio", "n": a.seeds, "spread": (hi - lo) / 2})
    for mode in MODES:
        ms, sd = pass_ms(recs[("v1_slab.scattering", "independent")], mode, tmp)
        print(f"pass_ms v1_slab.scattering cpu {mode}: {ms:.3f} ms (spread {sd:.3f}), 64x64 independent")
    if not a.no_cuda:
        t1 = time.perf_counter()
        for name, _, _ in SCENES[:2]:
            rec = recs[(name, "independent")]
            vr, v0 = (render(fr, rec, "cuda", a.spp, CUDA_SEED[m], a.size, m, "independent", tmp)[1].mean(axis=(-2, -1))
                      for m in MODES)
            cpu = {m: results[(name, "independent")][m][1].mean(axis=(-2, -1)) for m in MODES}  # (S, 3)
            for c in range(3):
                pairs = (cpu["random"][:, None, c] / cpu["lane0"][None, :, c]).ravel()
                lo, hi = np.percentile(pairs, [2.5, 97.5])
                r = vr[c] / v0[c]
                print(f"cuda  {name:18s} independent {CHANNELS[c]:5s} one-seed ratio {r:.4f}; CPU one-seed pairs "
                      f"[{lo:.4f}, {hi:.4f}] {'inside' if lo <= r <= hi else 'OUTSIDE'}")
        print(f"# cuda cross-check {time.perf_counter() - t1:.0f} s wall")
    if a.evidence:
        an = load_module("_analytic_hero", os.path.join(ROOT, "tools", "analytic.py"))
        write_maps(os.path.join(a.evidence, "hero_variance.png"), results[("v1_slab.scattering", "independent")], an.write_png)
    if a.history:
        sha = subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True, text=True, cwd=ROOT).stdout.strip()
        date = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        with open(a.history, "a", newline="\n") as f:
            for h in hist:
                line = {"sha": sha, "machine": "rtx5070ti-8c16t", "driver": "unknown", "cuda": "none", **h, "date": date}
                f.write(json.dumps(line, separators=(",", ":")) + "\n")
    print(f"PASS hero_variance: means agree within their limits; {time.perf_counter() - t0:.0f} s wall")


if __name__ == "__main__":
    main()
