# SPDX-License-Identifier: Apache-2.0
"""Integrator checks (task 076): Russian roulette furnace, shadow lifetime, closed-box statistics,
radiance scale, and the B-16 work counters and timing tripwire.

Usage: <python> -I tools/integrator_checks.py --gate --device cpu|cuda [--bless]
       <python> -I tools/integrator_checks.py --check NAME [--check NAME ...] --device D
       <python> -I tools/integrator_checks.py --check d2_rr_furnace|d4_lifetime --device D --null N
Checks (NAME): d2_rr_furnace (0004 D2), d4_lifetime (0004 D4/D5 opaque part, 0002 D5),
d28_closed_box (0004 D28, 0008 V8), d15_radiance_scale (0003 check 15), counters (B-16, both
devices) and tripwire (B-16, cuda only). Prints one line per check and a final PASS or FAIL line;
exits non-zero on failure. --bless rewrites tests/budgets/counters.toml for this device from the
counters and tripwire measured now (the reviewer sees the edit, 0009 d18).

d2 and d4 test display Y per [truth] block against truths.py uniform_enclosure, one analytic-family
member each (gate_members; tools/analytic.py counts them): |block mean - truth| <= z SE, z the
family's Sidak value over the blocks, SE from the dump's variance of the mean (independent mode).
Ref: docs/decisions/0004-integrator.md Verification rows D2, D4/D5, D28; 0002 Verification D5;
0008 V8; 0003 Verification check 15; 0009 d15, d16, d18 (work counters, tripwire), B-16.
"""
import argparse
import ctypes
import importlib.util
import math
import os
import subprocess
import sys
import tempfile
import time
import tomllib

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "scenes", "lib"))
import analytic  # noqa: E402
import ar  # noqa: E402
import meta  # noqa: E402
import truths  # noqa: E402

COUNTERS = os.path.join(ROOT, "tests", "budgets", "counters.toml")
DEVICES = ("cpu", "cuda")
# Truth-tested renders per device, each one analytic-family member: d2 with Russian roulette on and
# off, d4's default-pool render. Their block statistic departs from analytic.py's per-pixel z at
# MIN_SPP, so it was null-calibrated (`--check d2_rr_furnace --check d4_lifetime --device cpu --null
# 100`, signed block z of correct code, one-sided normal count in brackets):
#   d2 (max_depth 48) rr_on, 6400 blocks: 0 alarms at 4.99; mean -0.036 sd 0.971 skew -0.121; z>3 /
#     z<-3: 6 / 14 (8.6). The image mean is unbiased (mean / truth - 1 = -8e-5, SE 2.8e-4); the
#     negative z mean is roulette's skew: a block that drew few long paths is low with a low VAR.
#   d2 rr_off: 0 alarms; sd 0.385 (Y's SE is the Minkowski bound, conservative); none beyond 2;
#     mean / truth - 1 = +2.0e-5 (SE 1.4e-5); the truth is the series, Le / (1 - rho) less rho^49 = 1.8e-5.
#   d4 default, 1600 blocks: 0 alarms at 4.71; mean -0.009 sd 0.943 skew -0.169; z>3 / z<-3: 0 / 2 (2.2).
TRUTH_RENDERS = (("d2_rr_furnace", "rr_on"), ("d2_rr_furnace", "rr_off"), ("d4_lifetime", "default"))
# B-16 (0009 d18): the A fixtures that need no B-1M scene, and d28; the tripwire runs on the largest.
COUNTER_SCENES = ("a1_furnace", "a3_disk", "d28_closed_box")
COUNTER_KEYS = ("rays_closest", "rays_shadow", "steps", "launches", "state_bytes", "shadow_item_bytes")
COUNTER_GROWTH = 1.02  # B-16: fail above +2 %
TRIPWIRE_SCENE, TRIPWIRE_SPP, TRIPWIRE_RUNS, TRIPWIRE_LIMIT = "d28_closed_box", (1, 64), 5, 1.25  # B-16
RELATIVE_POOL = 1e-6  # 0004 D4/D5 row: equals the large-pool run within 1e-6 relative


def gate_members():
    """Analytic-family members the gate's integrator steps run (both devices)."""
    return len(TRUTH_RENDERS) * len(DEVICES)


def load_builder(scene):
    spec = importlib.util.spec_from_file_location(f"_build_{scene}", os.path.join(ROOT, "scenes", scene, "build.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def truth_of(scene):
    return meta.load(os.path.join(ROOT, "scenes", scene, "meta.toml"))["truth"]


def recording(scene, variant="default"):
    """out/scenes/<scene>.arrec (tools/scenes.py); d2's Russian-roulette-off variant is built here
    beside it, again whenever it is older than the scene's own recording."""
    rec = os.path.join(analytic.SCENES_OUT, scene + ".arrec")
    if not os.path.exists(rec):
        raise FileNotFoundError(f"{rec} missing: run tools/scenes.py first")
    if variant != "rr_off":
        return rec
    off = os.path.join(analytic.SCENES_OUT, scene + ".rr_off.arrec")
    if not os.path.exists(off) or os.path.getmtime(off) < os.path.getmtime(rec):
        load_builder(scene).build(off, rr=False)
    return off


def dump(rec, device, spp, size, seed, env_extra=None):
    """One `astroray_cli dump`; returns (header, {EXTNAME: array}, stdout). Raises on failure."""
    spec = importlib.util.spec_from_file_location("_fits_read_ic", analytic.FITS_READ)
    fr = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fr)
    env = {k: v for k, v in os.environ.items() if not k.startswith("ASTRORAY_TEST_")}
    env.update(env_extra or {})
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "dump.fits")
        r = subprocess.run([analytic.CLI, "dump", rec, "-o", out, "--device", device, "--spp", str(spp), "--seed",
                            str(seed), "--size", f"{size}x{size}"], capture_output=True, text=True, cwd=tmp,
                           timeout=600, env=env)
        if r.returncode != 0:
            raise RuntimeError(f"astroray_cli exited {r.returncode} ({env_extra})\n{r.stdout}{r.stderr}")
        with open(out, "rb") as f:
            hdus = fr.read_hdus(f.read())
    head = hdus[0][0]
    if head.get("DEVICE") != device or head.get("SPP") != spp or head.get("SAMPLER") != "independent":
        raise RuntimeError(f"dump header DEVICE {head.get('DEVICE')} SPP {head.get('SPP')} SAMPLER "
                           f"{head.get('SAMPLER')}: expected {device}, {spp}, independent")
    return head, {h["EXTNAME"]: np.asarray(d) for h, d in hdus[1:]}, r.stdout


def display(planes):
    return (np.stack([planes[f"display.{c}"] for c in range(3)]).astype(np.float64),
            np.stack([planes[f"display.{c}.VAR"] for c in range(3)]).astype(np.float64))


def truth_blocks(scene, planes):
    """(pass, worst z, block z threshold, measured Y mean, truth): display Y per [truth] block."""
    t = truth_of(scene)
    (name, row, truth, kind), = truths.evaluate(t, t["size"], t["size"])
    mean, var = analytic.channel(row, *display(planes))
    k = int(t["block"])
    if t["size"] % k or k * k * t["spp"] < analytic.MIN_SPP:
        raise ValueError(f"{scene}: [truth] block {k} must divide size and hold >= {analytic.MIN_SPP} samples")
    bm, bv, bt = analytic.block(mean, 1, k), analytic.block(var, 2, k), analytic.block(truth, 1, k)
    z = analytic.pixel_z(analytic.family_m(), bm.size)
    passed, reasons, worst = analytic.check_channel(bm, bv, bt, z, 0.0)
    return passed, worst[0], z, float(mean.mean()), float(truth.mean()), reasons


def seed(scene, variant):
    return analytic.seed_for(f"{scene}:{variant}")


def check_d2(device):
    t, ok, lines = truth_of("d2_rr_furnace"), True, []
    for variant in ("rr_on", "rr_off"):
        _, planes, _ = dump(recording("d2_rr_furnace", variant), device, t["spp"], t["size"], seed("d2", variant))
        p, wz, z, m, tr, why = truth_blocks("d2_rr_furnace", planes)
        ok &= p
        lines.append(f"{variant} Y {m:.6f} truth {tr:.6f} worst block z {wz:+.2f} of {z:.2f}" + (f" ({'; '.join(why)})" if why else ""))
    return ok, "; ".join(lines)


def check_d4(device):
    """Default pools (truth test, reference), then the starved pools of [truth] pool: on the CPU
    twice at one thread (bit-identical), on CUDA once. Every starved render equals the reference
    within 1e-6 relative, per pixel and channel."""
    t = truth_of("d4_lifetime")
    rec, s = recording("d4_lifetime"), seed("d4", "all")
    _, ref, _ = dump(rec, device, t["spp"], t["size"], s)
    p, wz, z, m, tr, why = truth_blocks("d4_lifetime", ref)
    msgs = [f"default pools Y {m:.6f} truth {tr:.6f} worst block z {wz:+.2f} of {z:.2f}" + (f" ({'; '.join(why)})" if why else "")]
    ok = p
    env = {"ASTRORAY_TEST_POOL": t["pool"]}
    if device == "cpu":
        env["ASTRORAY_TEST_THREADS"] = "1"
    runs = []
    for _ in range(2 if device == "cpu" else 1):
        _, planes, out = dump(rec, device, t["spp"], t["size"], s, env)
        if f"POOL={t['pool']}" not in out:
            raise RuntimeError("the CLI's report lists no TEST_HOOK_ACTIVE entry for ASTRORAY_TEST_POOL")
        runs.append(planes)
    a, b = display(ref)[0], display(runs[0])[0]
    rel = float(np.max(np.abs(a - b) / np.maximum(np.abs(a), 1e-30)))
    ok &= rel <= RELATIVE_POOL
    msgs.append(f"pool {t['pool']} vs default: max relative difference {rel:.2e} (limit {RELATIVE_POOL:g})")
    if device == "cpu":
        same = all(np.array_equal(runs[0][k], runs[1][k]) for k in runs[0])
        ok &= same
        msgs.append(f"threads=1 twice: {'bit-identical' if same else 'DIFFER'}")
    return ok, "; ".join(msgs)


def check_d28(device):
    """0004 D28 and 0008 V8, exact: samples resolved = camera samples launched, one path end per
    sample (film_path_end_stats counts each end once in bounce_paths), bounce mean and maximum =
    max_depth in every pixel (Russian roulette off in a closed box: every path ends at MAX_BOUNCE)."""
    t = truth_of("d28_closed_box")
    _, pl, _ = dump(recording("d28_closed_box"), device, t["spp"], t["size"], seed("d28", "all"))
    launched = t["size"] ** 2 * t["spp"]
    nsamp = int(pl["display.0.NSAMP"].astype(np.int64).sum())
    ends = int(pl["bounce_paths"].astype(np.int64).sum())
    d = t["max_depth"]
    mean_ok = bool((pl["bounces"] == d).all())
    max_ok = bool((pl["bounce_max"] == d).all())
    ok = nsamp == launched and ends == nsamp and mean_ok and max_ok
    return ok, (f"samples {nsamp} of {launched} launched, path ends {ends}, bounce mean "
                f"{float(pl['bounces'].min()):g}..{float(pl['bounces'].max()):g} and max "
                f"{int(pl['bounce_max'].min())}..{int(pl['bounce_max'].max())} (max_depth {d})")


def logged_scale_exponents(log):
    """The k of every "radiance scale s = 2^k" line session.cpp logs when an epoch's s changes."""
    return [int(ln.rsplit("2^", 1)[1]) for ln in log.splitlines() if "session: radiance scale s = 2^" in ln]


def check_d15(device):
    """The automatic s must not be 1 (else the comparison is trivial): read from the context log of
    an in-process render, and s = 1 under the hook; then the two CLI films must be bit-identical."""
    auto_k = logged_scale_exponents(in_process("d15_radiance_scale", device)[3])
    # The hook must be in the environment the engine DLL starts with (its C runtime does not see
    # os.environ edits), so the hooked render runs in a child process.
    env = {k: v for k, v in os.environ.items() if not k.startswith("ASTRORAY_TEST_")}
    env["ASTRORAY_TEST_RADIANCE_SCALE"] = "1"
    r = subprocess.run([sys.executable, "-I", os.path.abspath(__file__), "--log-scale", "d15_radiance_scale", "--device",
                        device], capture_output=True, text=True, env=env, timeout=600)
    if r.returncode != 0:
        raise RuntimeError(f"hooked in-process render failed: {r.stdout}{r.stderr}")
    unit_k = [int(v) for v in r.stdout.split()]
    if len(auto_k) != 1 or auto_k[0] == 0 or any(unit_k):
        return False, f"automatic s = 2^{auto_k} must be one value other than 2^0, hooked s = 2^{unit_k or [0]} must be 2^0"
    t = truth_of("d15_radiance_scale")
    rec, s = recording("d15_radiance_scale"), seed("d15", "all")
    _, auto, _ = dump(rec, device, t["spp"], t["size"], s)
    _, one, out = dump(rec, device, t["spp"], t["size"], s, {"ASTRORAY_TEST_RADIANCE_SCALE": "1"})
    if "RADIANCE_SCALE=1" not in out:
        raise RuntimeError("the CLI's report lists no TEST_HOOK_ACTIVE entry for ASTRORAY_TEST_RADIANCE_SCALE")
    differ = sorted(k for k in auto if not np.array_equal(auto[k], one[k]))
    return not differ, (f"automatic s = 2^{auto_k[0]}; film bit-identical with it and with s = 1 over {len(auto)} planes"
                        if not differ else f"planes differ: {differ}")


class _InProcess(ar.Scene):
    """ar.Scene whose finish() renders in this process instead of writing a recording: commit,
    one FINAL session per run, then the counters of its last pass (ar_session_read_counters, in
    src/host/session/readback.cpp counter_words order) and the wall time of each run."""
    device, runs, spp = "cpu", 1, None
    result = None

    def __init__(self, path, **kw):
        kw["device"] = type(self).device
        super().__init__(path, **kw)

    def settings(self, spp, *args, **kw):
        super().settings(type(self).spp or spp, *args, **kw)
        self.samples = type(self).spp or spp

    def finish(self):
        a, lib = self.abi, self.lib
        gen = ctypes.c_uint64()
        self._call(lib.ar_edit_commit(self.edit_h, ctypes.byref(gen)), "ar_edit_commit")
        w, h = self.size
        times = []
        for _ in range(type(self).runs):
            sh = ctypes.c_void_p()
            sd = a.ar_session_desc(a.AR_SESSION_MODE_FINAL, w, h, a.AR_DENOISER_NONE)
            self._call(lib.ar_session_create(self.scene_h, ctypes.byref(sd), ctypes.byref(sh)), "ar_session_create")
            self.session_h = sh
            v = a.ar_view_desc()
            ctypes.memmove(ctypes.byref(v.camera), ctypes.byref(self._camera), ctypes.sizeof(a.ar_camera_desc))
            v.region_width, v.region_height, v.render_scale = w, h, 1.0
            self._call(lib.ar_session_set_view(sh, ctypes.byref(v)), "ar_session_set_view")
            t0 = time.perf_counter()
            self._call(lib.ar_session_start(sh), "ar_session_start")
            self._call(lib.ar_session_wait(sh, 120000), "ar_session_wait")
            times.append(time.perf_counter() - t0)
            p = a.ar_progress()
            self._call(lib.ar_session_get_progress(sh, ctypes.byref(p)), "ar_session_get_progress")
            if p.state != a.AR_STATE_CONVERGED:
                raise RuntimeError(f"in-process render did not converge (state {p.state})")
            probe = (ctypes.c_uint64 * 1)()
            lib.ar_session_read_counters(sh, 0, probe)
            err = (ctypes.c_uint8 * 256)()
            lib.ar_session_last_error(sh, 256, err)
            n = int(bytes(err).split(b"\0")[0].decode().rsplit("need ", 1)[1])
            words = (ctypes.c_uint64 * n)()
            self._call(lib.ar_session_read_counters(sh, n, words), "ar_session_read_counters")
            lib.ar_session_destroy(sh)
            self.session_h = ctypes.c_void_p()
        # Words 0-3, then kStageCount stage items, then 11 words of which state_bytes_moved and
        # shadow_item_bytes_moved are the seventh and eighth (src/host/counters.h).
        counters = dict(zip(COUNTER_KEYS, (words[0], words[1], words[2], words[3], words[n - 5], words[n - 4])))
        self._teardown()
        with open(os.path.join(self._tmp.name, "context.log"), encoding="utf-8", errors="replace") as f:
            log = f.read()
        type(self).result = (counters, times, w * h * self.samples, log)


def in_process(scene, device, runs=1, spp=None):
    """Builds `scene` with its own builder into this process and renders it `runs` times, under the
    environment this process started with. Returns (counters, run seconds, samples, context log)."""
    cls = type("_Run", (_InProcess,), {"device": device, "runs": runs, "spp": spp})
    mod = load_builder(scene)
    real, mod.ar.Scene = mod.ar.Scene, cls
    try:
        with tempfile.TemporaryDirectory() as tmp:
            mod.build(os.path.join(tmp, "unused.arrec"))
    finally:
        mod.ar.Scene = real
    return cls.result


def load_baselines():
    with open(COUNTERS, "rb") as f:
        return tomllib.load(f)


def check_counters(device, measured):
    base = load_baselines().get(device, {})
    ok, parts = True, []
    for scene in COUNTER_SCENES:
        counters = in_process(scene, device)[0]
        measured.setdefault(device, {})[scene] = counters
        b = base.get(scene)
        if b is None:
            ok = False
            parts.append(f"{scene}: no baseline in tests/budgets/counters.toml (run --bless)")
            continue
        over = [f"{k} {counters[k]} > {b[k]} x {COUNTER_GROWTH}" for k in COUNTER_KEYS if counters[k] > b[k] * COUNTER_GROWTH]
        ok &= not over
        parts.append(f"{scene}: " + (", ".join(over) if over else
                     f"{counters['rays_closest']} closest, {counters['rays_shadow']} shadow, {counters['steps']} steps"))
    return ok, "; ".join(parts)


def check_tripwire(device, measured):
    """B-16 tripwire: wall ms per sample of TRIPWIRE_SCENE at each TRIPWIRE_SPP, after one burn-in
    render, minimum of TRIPWIRE_RUNS; fails above TRIPWIRE_LIMIT x the baseline. CUDA only (lead)."""
    if device != "cuda":
        return True, "cuda only"
    base = load_baselines().get("tripwire", {}).get(device, {})
    ok, parts = True, []
    for spp in TRIPWIRE_SPP:
        _, times, samples, _ = in_process(TRIPWIRE_SCENE, device, runs=TRIPWIRE_RUNS + 1, spp=spp)
        ms = min(times[1:]) * 1e3 / samples
        key = f"ms_per_sample_{spp}spp"
        measured.setdefault("tripwire", {}).setdefault(device, {})[key] = ms
        if key not in base:
            ok = False
            parts.append(f"{key} {ms:.3g}: no baseline (run --bless)")
            continue
        ok &= ms <= base[key] * TRIPWIRE_LIMIT
        parts.append(f"{spp} spp {ms:.3g} ms per sample (baseline {base[key]:.3g}, limit x{TRIPWIRE_LIMIT})")
    return ok, "; ".join(parts)


def bless(device, measured):
    """Rewrites this device's tables of tests/budgets/counters.toml, keeping the other device's."""
    data = load_baselines()
    data[device] = measured.get(device, data.get(device, {}))
    if "tripwire" in measured:
        data.setdefault("tripwire", {})[device] = measured["tripwire"][device]
    lines = ["# Work-counter baselines of the gate's B-16 check (0009 d18), written by",
             "# `tools/integrator_checks.py --gate --device D --bless`: per device and fixed scene, the counters of",
             "# the last pass at the scene's seed (+2 % fails), and the CUDA tripwire's wall ms per sample",
             "# (1.25x fails). An edit here is a re-bless, which the reviewer sees."]
    for dev in DEVICES:
        for scene, c in sorted(data.get(dev, {}).items()):
            lines += ["", f"[{dev}.{scene}]"] + [f"{k} = {int(c[k])}" for k in COUNTER_KEYS]
    for dev, row in sorted(data.get("tripwire", {}).items()):
        lines += ["", f"[tripwire.{dev}]"] + [f"{k} = {v:.6g}" for k, v in sorted(row.items())]
    with open(COUNTERS, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


CHECKS = {"d2_rr_furnace": check_d2, "d4_lifetime": check_d4, "d28_closed_box": check_d28,
          "d15_radiance_scale": check_d15, "counters": check_counters, "tripwire": check_tripwire}


def null_calibration(scene, device, n):
    """Null run of the block-vs-truth statistic the gate applies to `scene`: n independent seeds per
    truth-tested variant of correct code; counts alarms at the gate's threshold and prints the tails
    of the signed block z against the normal."""
    import statistics
    nd = statistics.NormalDist()
    t = truth_of(scene)
    for sc, variant in [r for r in TRUTH_RENDERS if r[0] == scene]:
        zs, alarms, crit, rel = [], 0, None, []
        for i in range(n):
            _, pl, _ = dump(recording(sc, variant), device, t["spp"], t["size"],
                            analytic.mix_bits(analytic.fnv1a(f"null:{sc}:{variant}:{i}")) & 0xFFFFFFFF)
            row = truths.evaluate(t, t["size"], t["size"])[0]
            mean, var = analytic.channel(row[1], *display(pl))
            k = int(t["block"])
            bm, bv, bt = analytic.block(mean, 1, k), analytic.block(var, 2, k), analytic.block(row[2], 1, k)
            z = (bm - bt) / np.sqrt(bv)
            crit = analytic.pixel_z(analytic.family_m(), bm.size)
            alarms += int((np.abs(z) > crit).any())
            rel.append(float(mean.mean() / row[2].mean() - 1.0))
            zs.extend(z.ravel())
        v = np.asarray(zs)
        tails = ", ".join(f"z>{c}: {int((v > c).sum())} z<-{c}: {int((v < -c).sum())} ({v.size * nd.cdf(-c):.2f})"
                          for c in (2.0, 3.0, 4.0))
        skew = float(((v - v.mean()) ** 3).mean() / v.std() ** 3)
        rel = np.asarray(rel)
        print(f"null {sc} {variant} {device}: image mean / truth - 1 over the seeds {rel.mean():+.2e} "
              f"(SE {rel.std() / math.sqrt(rel.size):.1e})")
        print(f"null {sc} {variant} {device}: {n} seeds, {v.size} blocks; alarms {alarms} at z {crit:.2f} (expected "
              f"{n * analytic.sidak(analytic.FAMILY_ALPHA, analytic.family_m()):.4f}); mean {v.mean():+.3f} sd "
              f"{v.std():.3f} skew {skew:+.3f} min {v.min():+.2f} max {v.max():+.2f}; {tails}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="append", choices=sorted(CHECKS), help="one check (repeatable)")
    ap.add_argument("--gate", action="store_true", help="every check")
    ap.add_argument("--device", required=True, choices=DEVICES)
    ap.add_argument("--bless", action="store_true", help="rewrite this device's counters.toml baselines")
    ap.add_argument("--log-scale", metavar="SCENE", help=argparse.SUPPRESS)  # check_d15's child
    ap.add_argument("--null", type=int, metavar="N", help="null calibration of d2 or d4's block test")
    a = ap.parse_args()
    if a.log_scale:
        print(" ".join(str(k) for k in logged_scale_exponents(in_process(a.log_scale, a.device)[3])))
        return 0
    if bool(a.check) == a.gate:
        ap.error("give --check or --gate")
    if a.null:
        for c in a.check:
            null_calibration(c, a.device, a.null)
        return 0
    if a.gate:
        analytic.check_family()  # the declared analytic family counts this tool's members too
    names = list(CHECKS) if a.gate else a.check
    t0, ok, measured = time.perf_counter(), True, {}
    for name in names:
        t1 = time.perf_counter()
        fn = CHECKS[name]
        try:
            passed, msg = fn(a.device, measured) if name in ("counters", "tripwire") else fn(a.device)
        except (RuntimeError, ValueError, OSError) as e:
            passed, msg = False, str(e).splitlines()[0]
        ok &= passed or a.bless and name in ("counters", "tripwire")
        print(f"{'PASS' if passed else 'FAIL'} integrator {name} {a.device}: {msg} ({time.perf_counter() - t1:.1f} s)")
    if a.bless:
        bless(a.device, measured)
        print(f"blessed {os.path.relpath(COUNTERS, ROOT)} for {a.device}")
    print(f"{'PASS' if ok else 'FAIL'} integrator {a.device}: {len(names)} checks in {time.perf_counter() - t0:.1f} s")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
