# SPDX-License-Identifier: Apache-2.0
"""Final OIDN check (0008 V11, final row): renders three recordings at 64 spp through `astroray_cli
render --denoise oidn --dump` (the raw Noisy Image and the denoised Combined of one session) on the
CPU and CUDA devices, one 16,384-spp CUDA render per scene as the reference, and prints the relMSE of
the raw and the denoised image against it, plus the denoiser's cold start.

Usage: <python> -I tools/denoise_check.py --self-test
       <python> -I tools/denoise_check.py [--device cpu|cuda|both] [--scene NAME ...] [--history]
                                          [--compare-only | --cold-only]
Run it under the machine lock (`check full` holds it; by hand: dot-source tools/lib/lock.ps1 and
Enter-ArMachineLock -Job bench). It is a `full`-tier or on-demand tool (0008 V13), never a gate case.

relMSE: mean((x - ref)^2 / (ref^2 + eps)) over every channel value, eps = 1e-2, the definition of
tests/common/noisy_fixture.h (kRelMseEps); restated here, there is no C++ to call.
The reference is rendered once per (recording bytes, size, REF_SPP, REF_SEED, REF_VERSION) on CUDA, with
no denoiser, and kept in the machine-wide cache of tools/scenes.py (ASTRORAY_SCENES_CACHE, else
%LOCALAPPDATA%/astroray2/scenes-cache, subfolder denoise_ref/), written through a temporary file and
renamed; a reference that disagrees with the 64-spp image mean beyond 5 SE is stale: bump REF_VERSION.
Cold start: the engine exposes no denoiser init time (DenoiserState::create returns at once and
nothing times its worker), so it is measured from outside, before any scene is rendered, as the wall
time of a fresh 16x16, 1-spp `render --denoise oidn --dump` process minus that of a `--denoise none`
process in the same cache state; the difference is the denoiser's load plus the denoise of that tiny
image plus the dump writes. "First" runs with CUDA_CACHE_PATH pointing at a new empty directory, so the
driver's JIT/kernel cache is cold; "second" repeats both processes on the directories the first run
filled. What stays warm is the operating system's file cache (the CLI, OIDN and CUDA libraries and
weights are read from disk once before): cold here means the driver cache, not a rebooted machine.
The per-scene denoise milliseconds are the same wall difference at the scene's size: neither a lower
nor an upper bound, since the oidn run also writes its --dump files (more wall) while the denoiser's
load overlaps the render (less wall).
Output: out/tmp/201/ (renders), out/evidence/201/<scene>_strip.png (noisy | denoised | reference | the
denoised image's relMSE map, one exposure per strip), --history: lines for bench/history in out/bench/.
Ref: docs/decisions/0008-film-outputs-denoise.md V11, V13; tests/common/cornell_rig.h (the reference
cache this follows); tools/scenes.py cache_dir().
"""
import argparse
import datetime
import hashlib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

np = None  # numpy, imported by use_numpy(): the self-test must stay under the fast tier's 0.2 s

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLI = os.path.join(ROOT, "out", "bin", "astroray_cli.exe")
SCENES_OUT = os.path.join(ROOT, "out", "scenes")
FITS_READ = os.path.join(ROOT, "tests", "abi", "fits_read.py")
WORK = os.path.join(ROOT, "out", "tmp", "201")
EVIDENCE = os.path.join(ROOT, "out", "evidence", "201")

EPS = 1e-2
SPP, SEED = 64, 1
REF_SPP, REF_SEED = 16384, 7777
# Bump when the engine's image changes for these recordings (kernel, film or recording changes): the
# cache key is the recording and these numbers, not the engine sources.
REF_VERSION = 2
# name -> (recording, render size, asserted). The asserted scenes are 0008 V11's set: diffuse with soft
# shadows, glossy metal (Risk 6's case), hard shadows; their Bitterli archive and HDRI come from
# `tools/fetch.ps1 assets`. b_tx (textures) is rendered and reported, not asserted.
SCENES = {"cornell_area": ("cornell_area", "512x512", True),
          "shader_balls": ("shader_balls", "512x288", True),
          "cornell_point": ("cornell_point", "512x512", True),
          "b_tx": ("s3_textured", "512x288", False)}
COLD_SIZE, COLD_SCENE = "16x16", "cornell_point"


def use_numpy():
    global np
    if np is None:
        import numpy
        np = numpy


def rel_scale(ref, eps=EPS):
    """ref^2 + eps: the relMSE denominator. The reference's own squared value keeps a dark pixel's error
    from dominating; eps keeps a black one finite.
    Ref: tests/common/noisy_fixture.cpp rel_mse."""
    return ref * ref + eps


def rel_term(a, ref, eps=EPS):
    """(a - ref)^2 / (ref^2 + eps): the one definition of the relMSE term, for scalars and ndarrays
    alike (the error maps and the device comparison call it on arrays). The numerator is the plain
    squared error; the denominator is rel_scale.
    Ref: tests/common/noisy_fixture.cpp rel_mse."""
    return (a - ref) ** 2 / rel_scale(ref, eps)


def rel_mse(x, ref, eps=EPS):
    """mean(rel_term) over every value of two flat sequences (lists or ndarray.ravel().tolist()), in plain
    Python so the self-test needs no numpy."""
    n = len(ref)
    if len(x) != n:
        raise ValueError(f"rel_mse: {len(x)} values against {n}")
    return sum(rel_term(a, b, eps) for a, b in zip(x, ref)) / n


def rel_mse_img(x, ref):
    """rel_mse of two images (any shape, same shape)."""
    return rel_mse(np.asarray(x, dtype=np.float64).ravel().tolist(), np.asarray(ref, dtype=np.float64).ravel().tolist())


def box_filter(img, k=3):
    """k x k mean filter of an h x w list-of-lists image, edges replicated."""
    h, w, p = len(img), len(img[0]), k // 2
    at = lambda y, x: img[min(max(y, 0), h - 1)][min(max(x, 0), w - 1)]
    return [[sum(at(y + dy, x + dx) for dy in range(-p, p + 1) for dx in range(-p, p + 1)) / (k * k)
             for x in range(w)] for y in range(h)]


def self_test():
    import random
    rng = random.Random(12345)
    n = 24
    ref2 = [[0.2 + 0.6 * x / (n - 1) + 0.2 * y / (n - 1) for x in range(n)] for y in range(n)]
    noisy2 = [[v + rng.gauss(0.0, 0.15) for v in row] for row in ref2]
    flat = lambda im: [v for row in im for v in row]
    ref, noisy = flat(ref2), flat(noisy2)
    fails = []
    if rel_mse(ref, ref) != 0.0:
        fails.append("identical inputs do not give 0")
    # Constant offset d on ref = 1: d^2 / (1 + eps), exactly.
    want = 0.1 ** 2 / (1.0 + EPS)
    got = rel_mse([1.1] * 12, [1.0] * 12)
    if abs(got - want) > 1e-12:
        fails.append(f"offset case {got} != {want}")
    # ref != 1 pins the denominator: ref^2 + eps, not ref + eps or 1 + eps.
    want = 0.1 ** 2 / (0.5 ** 2 + EPS)
    got = rel_mse([0.6] * 12, [0.5] * 12)
    if abs(got - want) > 1e-12:
        fails.append(f"ref 0.5 case {got} != {want}")
    a, b = rel_mse(noisy, ref), rel_mse(flat(box_filter(noisy2)), ref)
    if not b < a:
        fails.append(f"box-filtered relMSE {b:.5f} is not below noisy {a:.5f}")
    if a <= 0.0:
        fails.append("noisy relMSE is not positive")
    for f in fails:
        print("FAIL denoise_check self-test:", f)
    if not fails:
        print(f"PASS denoise_check self-test: noisy {a:.5f}, box-filtered {b:.5f}, identical 0")
    return 1 if fails else 0


def read_pfm(path):
    """(h, w, 3) float32, rows bottom-up (the film's order, as the CLI writes them)."""
    with open(path, "rb") as f:
        data = f.read()
    m = re.match(rb"PF\n(\d+) (\d+)\n(-?[0-9.]+)\n", data)
    if not m or float(m.group(3)) >= 0:
        raise RuntimeError(f"{path}: not a little-endian colour PFM")
    w, h = int(m.group(1)), int(m.group(2))
    px = np.frombuffer(data, dtype="<f4", offset=m.end())
    if px.size != w * h * 3:
        raise RuntimeError(f"{path}: {px.size} floats for {w}x{h}")
    return px.reshape(h, w, 3).copy()


def read_fits_display(path):
    """(mean (h, w, 3), variance of the mean (h, w, 3)) of a film dump's display planes."""
    spec = importlib.util.spec_from_file_location("_fits_read_dc", FITS_READ)
    fr = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fr)
    with open(path, "rb") as f:
        hdus = fr.read_hdus(f.read())
    planes = {h["EXTNAME"]: np.asarray(d) for h, d in hdus[1:]}
    mean = np.stack([planes[f"display.{c}"] for c in range(3)], axis=-1).astype(np.float64)
    var = np.stack([planes[f"display.{c}.VAR"] for c in range(3)], axis=-1).astype(np.float64)
    return mean, var


def run_cli(rec, device, spp, seed, size, denoise, stem, dump=False, timeout=7200, cuda_cache=None):
    """One `astroray_cli render`; returns (wall seconds, stdout). Raises on a failed render.
    cuda_cache: a directory for CUDA_CACHE_PATH (the driver's kernel cache), else the user's own."""
    os.makedirs(WORK, exist_ok=True)
    out = os.path.join(WORK, stem + ".fits")
    args = [CLI, "render", rec, "-o", out, "--device", device, "--spp", str(spp), "--seed", str(seed),
            "--size", size, "--denoise", denoise] + (["--dump"] if dump else [])
    env = dict(os.environ, CUDA_CACHE_PATH=cuda_cache) if cuda_cache else None
    t0 = time.perf_counter()
    r = subprocess.run(args, capture_output=True, text=True, timeout=timeout, cwd=WORK, env=env)
    wall = time.perf_counter() - t0
    if r.returncode != 0:
        raise RuntimeError(f"astroray_cli exited {r.returncode}: {' '.join(args)}\n{r.stdout}{r.stderr}")
    if denoise == "oidn" and ("DENOISER_UNAVAILABLE" in r.stdout or "(Combined, denoised)" not in r.stdout):
        raise RuntimeError(f"the render was not denoised ({device}):\n{r.stdout}")
    return wall, r.stdout


def cache_dir():
    """Ref: tools/scenes.py cache_dir() (restated: importing it costs the self-test its budget)."""
    d = os.environ.get("ASTRORAY_SCENES_CACHE")
    if not d and os.environ.get("LOCALAPPDATA"):
        d = os.path.join(os.environ["LOCALAPPDATA"], "astroray2", "scenes-cache")
    return os.path.join(d or os.path.join(ROOT, "out", "refs"), "denoise_ref")


def reference(scene, rec, size):
    """(image (h, w, 3), seconds, 'cached'|'rendered'): the 16k-spp CUDA render without a denoiser."""
    with open(rec, "rb") as f:
        key = hashlib.sha256(f.read() + f"|{size}|{REF_SPP}|{REF_SEED}|v{REF_VERSION}".encode()).hexdigest()[:16]
    d = cache_dir()
    path = os.path.join(d, f"{scene}-{key}.npy")
    if os.path.isfile(path):
        return np.load(path), 0.0, "cached"
    wall, _ = run_cli(rec, "cuda", REF_SPP, REF_SEED, size, "none", f"{scene}_ref")
    img, _ = read_fits_display(os.path.join(WORK, f"{scene}_ref.fits"))
    os.makedirs(d, exist_ok=True)
    tmp = f"{path}.tmp{os.getpid()}.npy"
    np.save(tmp, img.astype(np.float32))
    os.replace(tmp, path)
    return img.astype(np.float32), wall, "rendered"


def stale_guard(ref, noisy_mean, noisy_var, scene):
    """The 64-spp image mean agrees with the reference's per channel within 5 SE of the mean (the
    reference's own noise is negligible), else the cached reference belongs to an older engine. 5 SE:
    a correct reference fails a channel with probability ~6e-7, while an engine change that moves a
    scene's mean is many SE at 512 x 512 pixels, so the guard neither cries wolf nor sleeps."""
    n = ref.shape[0] * ref.shape[1]
    for k in range(3):
        se = np.sqrt(noisy_var[..., k].sum()) / n
        if not (np.isfinite(se) and se > 0.0):
            raise RuntimeError(f"{scene}: stale guard has no usable variance (channel {k}, SE {se})")
        z = (noisy_mean[..., k].mean() - float(ref[..., k].astype(np.float64).mean())) / se
        if not np.isfinite(z) or abs(z) > 5.0:
            raise RuntimeError(f"{scene}: stale denoise reference: bump REF_VERSION (channel {k}, z {z:.1f})")


def recording(name):
    rec = os.path.join(SCENES_OUT, SCENES[name][0] + ".arrec")
    if not os.path.isfile(rec):
        raise FileNotFoundError(f"{rec} missing: run `<python> -I tools/scenes.py {SCENES[name][0]}` (assets: `pwsh tools/fetch.ps1 assets`)")
    return rec


def worst_pixels(x, ref, frac=0.05):
    """The share of the total squared error held by the worst `frac` of pixels, and their mean
    reference luminance against the image's (numpy, as the decision table asks)."""
    e = ((x.astype(np.float64) - ref) ** 2).sum(-1).ravel()
    lum = ref.astype(np.float64).mean(-1).ravel()
    top = np.argsort(e)[-max(1, int(frac * e.size)):]
    return e[top].sum() / e.sum(), lum[top].mean(), lum.mean()


def strip(scene, noisy, den, ref):
    """out/evidence/201/<scene>_strip.png: noisy | denoised | reference | relMSE map of the denoised."""
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import analytic  # noqa: E402  (write_png; not imported by the self-test)
    err = rel_term(den.astype(np.float64), ref.astype(np.float64)).mean(-1)
    scale = 1.0 / max(float(np.percentile(ref.mean(-1), 99)), 1e-6)
    emap = np.repeat((err / max(float(np.percentile(err, 99)), 1e-12))[..., None], 3, axis=-1)
    panels = [np.clip(p.astype(np.float64) * scale, 0, 1) for p in (noisy, den, ref)] + [np.clip(emap, 0, 1)]
    row = np.concatenate(panels, axis=1)  # (h, 4w, 3), rows bottom-up
    os.makedirs(EVIDENCE, exist_ok=True)
    path = os.path.join(EVIDENCE, f"{scene}_strip.png")
    analytic.write_png(path, row.transpose(2, 0, 1), 1.0, enlarge=1)
    return path


def cold_start(device):
    """(first, second) cold-start milliseconds, see the module docstring: each oidn process minus the none
    process of the same cache state; the first pair runs on new empty CUDA_CACHE_PATH directories, the
    second pair on the directories the first pair filled."""
    rec = recording(COLD_SCENE)
    os.makedirs(WORK, exist_ok=True)
    dirs = {m: tempfile.mkdtemp(prefix=f"cuda_cache_{m}_", dir=WORK) for m in ("oidn", "none")}
    out = []
    for _ in range(2):
        w = {m: run_cli(rec, device, 1, SEED, COLD_SIZE, m, f"cold_{device}_{m}", dump=(m == "oidn"),
                        cuda_cache=dirs[m])[0] for m in ("oidn", "none")}
        out.append((w["oidn"] - w["none"]) * 1e3)
    for d in dirs.values():
        shutil.rmtree(d, ignore_errors=True)
    return out


def measure(device, scenes):
    rows = []
    for name in scenes:
        rec, size = recording(name), SCENES[name][1]
        ref, ref_s, how = reference(name, rec, size)
        if how == "rendered":
            print(f"reference {name}: rendered {REF_SPP} spp on cuda in {ref_s:.1f} s")
        stem = f"{name}_{device}"
        w_none, _ = run_cli(rec, device, SPP, SEED, size, "none", stem + "_none")
        w_oidn, _ = run_cli(rec, device, SPP, SEED, size, "oidn", stem, dump=True)
        noisy = read_pfm(os.path.join(WORK, stem + "_noisy_image.pfm"))
        den = read_pfm(os.path.join(WORK, stem + "_combined.pfm"))
        mean, var = read_fits_display(os.path.join(WORK, stem + "_none.fits"))
        # One film: the session with the denoiser reports the film of the one without it, bit for bit.
        if mean.shape != noisy.shape or not np.allclose(mean, noisy, rtol=1e-6, atol=1e-9):
            raise RuntimeError(f"{name} {device}: Noisy Image differs from the --denoise none film")
        stale_guard(ref, mean, var, name)
        raw, dn = rel_mse_img(noisy, ref), rel_mse_img(den, ref)
        np.savez(os.path.join(WORK, stem + ".npz"), noisy=noisy, den=den)
        png = strip(name, noisy, den, ref) if device == "cuda" or not os.path.isfile(
            os.path.join(EVIDENCE, f"{name}_strip.png")) else None
        rows.append({"scene": name, "device": device, "raw": raw, "den": dn, "render_s": w_none,
                     "denoise_ms": (w_oidn - w_none) * 1e3, "worst": worst_pixels(den, ref), "png": png})
    return rows


# Relative agreement of the denoised relMSE against the reference between the devices.
# Ref: docs/decisions/RULINGS.md ruling 110 (the CPU-versus-CUDA outcome of this check's decision table).
DEVICE_AGREE = 0.05


def compare_devices(scenes):
    """Backend-defect check on the saved arrays of both devices: the denoised relMSE against the reference
    agrees between CPU and CUDA within DEVICE_AGREE relative, per scene. Both renders share the RNG, so the
    raw images agree to ~1e-8 and "the denoised images differ less than the raw ones" can never hold: the
    CUDA device runs its network at reduced precision, the CPU device does not, and that alone exceeds it.
    Also printed, not asserted: the cpu~cuda denoised relMSE and the share of it held by the worst 1 % of
    pixels (a difference concentrated in few pixels is shown, not failed). Returns the failing rows."""
    bad = []
    for name in scenes:
        c, g = (np.load(os.path.join(WORK, f"{name}_{d}.npz")) for d in ("cpu", "cuda"))
        ref, _, _ = reference(name, recording(name), SCENES[name][1])
        ref = ref.astype(np.float64)
        e_c, e_g = rel_mse_img(c["den"], ref), rel_mse_img(g["den"], ref)
        rel = abs(e_c - e_g) / max(e_c, e_g)
        diff = rel_term(ref + (c["den"].astype(np.float64) - g["den"]), ref).mean(-1).ravel()  # (cpu - cuda)^2 over the reference term
        worst = np.sort(diff)[::-1][:max(1, diff.size // 100)].sum() / diff.sum()
        print(f"cpu vs cuda {name}: denoised vs reference cpu {e_c:.4g} cuda {e_g:.4g} (differ {rel:.1%}); "
              f"cpu~cuda denoised {diff.mean():.3e} (raw {rel_mse_img(c['noisy'], g['noisy']):.3e}), "
              f"worst 1 % of pixels hold {worst:.0%}")
        if rel > DEVICE_AGREE:
            word = "FAIL" if SCENES[name][2] else "note"
            print(f"{word} denoise_check: {name} denoised relMSE differs {rel:.1%} between cpu and cuda "
                  f"(limit {DEVICE_AGREE:.0%})")
            if SCENES[name][2]:
                bad.append({"scene": name})
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--compare-only", action="store_true", help="only the cpu/cuda comparison, from the saved arrays")
    ap.add_argument("--cold-only", action="store_true", help="only the cold starts (no scene is rendered)")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--device", choices=("cpu", "cuda", "both"), default="both")
    ap.add_argument("--scene", action="append", choices=sorted(SCENES))
    ap.add_argument("--history", action="store_true", help="write bench/history lines to out/bench/")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    use_numpy()
    scenes = a.scene or list(SCENES)
    if a.compare_only:
        bad = compare_devices(scenes)
        print(f"{'FAIL' if bad else 'PASS'} denoise_check: cpu~cuda comparison of {len(scenes)} scenes")
        return 1 if bad else 0
    devices = ["cpu", "cuda"] if a.device == "both" else [a.device]
    t0 = time.perf_counter()
    rows, cold = [], {}
    for dev in devices:  # first, so no scene render has warmed anything the cold start is to pay
        cold[dev] = cold_start(dev)
    for dev in devices if not a.cold_only else []:
        rows += measure(dev, scenes)
    print("scene           dev   raw relMSE  denoised  ratio   render s  denoise ms")
    bad = []
    for r in rows:
        print(f"{r['scene']:<16} {r['device']:<5} {r['raw']:10.4g} {r['den']:9.4g} {r['den'] / r['raw']:6.3f} "
              f"{r['render_s']:9.2f} {r['denoise_ms']:10.0f}")
        if not r["den"] < r["raw"]:
            share, wl, al = r["worst"]
            word = "FAIL" if SCENES[r["scene"]][2] else "note"  # reported-only scenes never fail the run
            print(f"{word} denoise_check: {r['scene']} {r['device']} denoised {r['den']:.5f} is not below raw "
                  f"{r['raw']:.5f}; the worst 5 % of pixels hold {share:.0%} of the squared error, their mean "
                  f"reference luminance {wl:.4f} against {al:.4f}")
            if SCENES[r["scene"]][2]:
                bad.append(r)
    for dev, (c1, c2) in cold.items():
        print(f"cold start {dev}: first {c1:.0f} ms, second {c2:.0f} ms (wall of oidn minus none, {COLD_SIZE} 1 spp, cold driver cache then warm)")
    if len(devices) == 2 and not a.cold_only:
        bad += compare_devices(scenes)
    if a.history:
        now = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        sha = subprocess.run(["git", "-C", ROOT, "rev-parse", "HEAD"], capture_output=True, text=True, check=True).stdout.strip()
        with open(os.path.join(ROOT, "bench", "history", "2026.jsonl")) as f:
            last = json.loads(f.read().splitlines()[-1])
        out = os.path.join(ROOT, "out", "bench", f"history-denoise-{now[:10].replace('-', '')}.jsonl")
        os.makedirs(os.path.dirname(out), exist_ok=True)
        base = {"sha": sha, "machine": last["machine"], "driver": last["driver"], "cuda": last["cuda"],
                "n": 1, "spread": 0.0, "date": now}
        lines = []
        for r in rows:
            lines += [{**base, "scene": f"{r['scene']}_{r['device']}", "metric": "oidn_relmse_raw", "value": float(f"{r['raw']:.4g}"), "unit": "relMSE"},
                      {**base, "scene": f"{r['scene']}_{r['device']}", "metric": "oidn_relmse_denoised", "value": float(f"{r['den']:.4g}"), "unit": "relMSE"}]
        for dev, (c1, c2) in cold.items():
            lines.append({**base, "scene": f"cold_{dev}", "metric": "oidn_cold_init_ms", "value": round(c1, 1), "unit": "ms", "n": 2,
                          "spread": round(abs(c1 - c2), 1)})
        with open(out, "a", newline="\n") as f:
            for ln in lines:
                f.write(json.dumps(ln) + "\n")
        print(f"history lines appended to {out}")
    print(f"{'FAIL' if bad else 'PASS'} denoise_check: {len(rows)} rows in {time.perf_counter() - t0:.0f} s")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
