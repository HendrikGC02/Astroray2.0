# SPDX-License-Identifier: Apache-2.0
"""Kernel resource gate, compile timing and probe warnings of `check gate` / `check full`.

Run (Blender's python, -I first):
  kernelgate.py [--declared "name:regs=+0,stack=+16;..."] [--base origin/main]   resources
  kernelgate.py --time                  compile timing of the changed device TUs (B-05)
  kernelgate.py --probe                 uncapped register warnings and the capped probe modules'
                                        rows, never a failure
  kernelgate.py --self-test             tests/budgets/kernelgate_cases/
Ref: docs/decisions/0002-kernel-architecture.md D8, D10 (resources, growth rule, timing method);
0009 d18 (resource gate, compile timing), B-05, B-06. Registers and stack come from
`cuobjdump -res-usage`, spills from the `ptxas -v` text (the build saves it as <name>.ptxas.txt).
Gate-only probe modules (src/kernel_gpu/probe_*.cu) are built capped into kernels_probe_capped/ by the
probe target; their rows are gated "full" and a breach is a warning, never a failure (task 092).
Exit 0 pass, 1 gate failure, 2 usage or environment error.
"""
import argparse
import os
import re
import shlex
import subprocess
import sys
import tempfile
import time
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CASES = "tests/budgets/kernelgate_cases"
LAUNCH_CONFIG = "src/kernel/launch_config.h"
BUDGET = "src/kernel/budget.toml"

# 0002 D10 limits (the ids are the records', not ours).
STACK_LIMIT = {"VM": 512, "OTHER": 128}      # D10 production stack, VM kernels / all others
SPILL_CEILING = {"VM": (128, 192), "OTHER": (0, 0)}  # D10 spill allowance (stores, loads) without a re-tune
REG_GROWTH = 1.05                            # D10: +5% registers (kernels without B) fails
STACK_GROWTH = 1.10                          # D10: +10% stack fails
REGS_PER_SM = 65536                          # D10: registers <= 65536/(T*B) when B is set
PROBE_WARN, PROBE_LOUD = 150, 168            # D10 uncapped registers: warning at 150, loud above 168
COMPILE_LIMIT_S = 3.0                        # B-05; a result within 10% of it is re-timed (0009 d18)
# Explicit owner raises of B-05 for single TUs (never silent; re-measured each milestone).
COMPILE_LIMIT_OVERRIDE_S = {"shade_surface": 8.0}  # owner decision 43 (amends 41 and 40, RULINGS.md 72): the medium stack in shade_surface, M1b
COMPILE_RETIME_BAND = 0.10
COMPILE_TUS = 3                              # 0009 d18: at most three changed TUs, largest compile_s first
BUDGET_KEYS = ("regs", "stack", "spill_store", "spill_load", "compile_s", "gated")


class GateError(Exception):
    """Unusable input (missing tool, unparsable file): exit 2, never a silent pass."""


def parse_launch_rows(text):
    rows = {}
    for m in re.finditer(r"^\s*X\(\s*(\w+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(VM|OTHER)\s*\)", text, re.M):
        name = m.group(1)
        if name in rows:
            raise GateError(f"launch_config.h: duplicate row {name}")
        rows[name] = {"T": int(m.group(2)), "B": int(m.group(3)), "k": int(m.group(4)), "cls": m.group(5)}
    return rows


def parse_budget(text):
    rows = tomllib.loads(text).get("kernel", {})
    for name, r in rows.items():
        missing = [k for k in BUDGET_KEYS if k not in r]
        if missing:
            raise GateError(f"budget.toml: [kernel.{name}] lacks {', '.join(missing)}")
        if r["gated"] not in ("merge", "full"):
            raise GateError(f"budget.toml: [kernel.{name}] gated must be \"merge\" or \"full\"")
    return rows


def parse_res_usage(text):
    """cuobjdump -res-usage: {function: {regs, stack}}."""
    out, cur = {}, None
    for ln in text.splitlines():
        m = re.match(r"\s*Function (\S+):", ln)
        if m:
            cur = m.group(1)
            continue
        m = re.match(r"\s*REG:(\d+) STACK:(\d+)", ln)
        if m and cur:
            out[cur] = {"regs": int(m.group(1)), "stack": int(m.group(2))}
    return out


def parse_ptxas(text):
    """ptxas -v: {entry function: {regs, stack, spill_store, spill_load}}."""
    out, cur, props = {}, None, None
    for ln in text.splitlines():
        m = re.search(r"Compiling entry function '([^']+)'", ln)
        if m:
            cur = m.group(1)
            out[cur] = {}
            continue
        m = re.search(r"Function properties for (\S+)", ln)
        if m:
            props = m.group(1)
            continue
        m = re.search(r"(\d+) bytes stack frame, (\d+) bytes spill stores, (\d+) bytes spill loads", ln)
        if m and cur and props == cur:
            out[cur].update(stack=int(m.group(1)), spill_store=int(m.group(2)), spill_load=int(m.group(3)))
        m = re.search(r"Used (\d+) registers", ln)
        if m and cur:
            out[cur]["regs"] = int(m.group(1))
    return out


def parse_declared(spec):
    """"name:regs=+4,stack=+16;other:..." -> {name: {field: int}}."""
    out = {}
    for part in filter(None, (p.strip() for p in (spec or "").split(";"))):
        name, _, kv = part.partition(":")
        out[name.strip()] = {k.strip(): int(v) for k, v in (x.split("=") for x in kv.split(",") if x)}
    return out


def evaluate(launch, budget, base, cubins, is_stage, declared, full=False):
    """Returns (failures, lines, deferred). cubins: {stem: (res-usage text, ptxas text)}; base: the
    budget rows at the merge base ({} if none); is_stage(stem): production stage TU. Rows with
    gated = "full" are deferred unless `full`; the caller must print the deferred count."""
    fails, lines = [], []
    deferred = [] if full else sorted(n for n, r in budget.items() if r["gated"] == "full")
    measured = set()
    # An allowance above the class ceiling is a failure (0009 d13); a re-tune changes 0002 D10 and
    # SPILL_CEILING, never a row.
    for n, br in sorted(budget.items()):
        lr = launch.get(n)
        cap = SPILL_CEILING[lr["cls"]] if lr else None
        if cap and (br["spill_store"] > cap[0] or br["spill_load"] > cap[1]):
            fails.append(f"{n}: spill allowance {br['spill_store']}/{br['spill_load']} B in budget.toml exceeds the "
                         f"{lr['cls']} ceiling {cap[0]}/{cap[1]} B of 0002 D10")
    for stem, (res_txt, ptx_txt) in sorted(cubins.items()):
        res, ptx = parse_res_usage(res_txt), parse_ptxas(ptx_txt)
        for name in sorted(res):
            required = name.startswith("k_") or is_stage(stem)
            lr, br = launch.get(name), budget.get(name)
            if lr is None or br is None:
                if required:
                    where = [w for w, r in (("launch_config.h", lr), ("budget.toml", br)) if r is None]
                    fails.append(f"{name} ({stem}.cubin): no row in {' and '.join(where)}")
                else:
                    lines.append(f"{name}: no rows, test module not gated")
                continue
            if br["gated"] == "full" and not full:
                lines.append(f"{name}: deferred to check full")
                continue
            measured.add(name)
            m, p = res[name], ptx.get(name)
            if not p or "spill_store" not in p or "regs" not in p:
                fails.append(f"{name}: no ptxas -v entry in {stem}.ptxas.txt")
                continue
            if m["regs"] != p["regs"] or m["stack"] != p["stack"]:
                fails.append(f"{name}: cuobjdump (regs {m['regs']}, stack {m['stack']}) and ptxas -v "
                             f"(regs {p['regs']}, stack {p['stack']}) disagree")
                continue
            bad = []
            if lr["B"] > 0:
                cap = REGS_PER_SM // (lr["T"] * lr["B"])
                if m["regs"] > cap:
                    bad.append(f"registers {m['regs']} > 65536/(T*B) = {cap} (0002 D10)")
            if p["spill_store"] > br["spill_store"]:
                bad.append(f"spill stores {p['spill_store']} B > allowance {br['spill_store']} B")
            if p["spill_load"] > br["spill_load"]:
                bad.append(f"spill loads {p['spill_load']} B > allowance {br['spill_load']} B")
            if m["stack"] > STACK_LIMIT[lr["cls"]]:
                bad.append(f"stack {m['stack']} B > {lr['cls']} limit {STACK_LIMIT[lr['cls']]} B (0002 D10)")
            old = base.get(name)
            if old is not None and old == br:  # growth is judged only when the diff leaves the row alone
                if lr["B"] == 0 and m["regs"] > br["regs"] * REG_GROWTH:
                    bad.append(f"registers {m['regs']} grew >5% over budget.toml {br['regs']}; update the row")
                if m["stack"] > br["stack"] * STACK_GROWTH:
                    bad.append(f"stack {m['stack']} B grew >10% over budget.toml {br['stack']} B; update the row")
            ref = old if old is not None else br
            cur = {"regs": m["regs"], "stack": m["stack"], "spill_store": p["spill_store"],
                   "spill_load": p["spill_load"]}
            delta = " ".join(f"{k} {cur[k]} ({cur[k] - ref[k]:+d})" for k in cur)
            dec = declared.get(name)
            dtext = ",".join(f"{k}={v:+d}" for k, v in dec.items()) if dec else "none"
            lines.append(f"{name}: {delta} | declared {dtext}" + ("" if old is not None else " | new row"))
            fails.extend(f"{name}: {b}" for b in bad)
    for n in sorted(set(launch) - set(budget)):
        fails.append(f"{n}: row in launch_config.h but none in budget.toml")
    for n in sorted(set(budget) - set(launch)):
        fails.append(f"{n}: row in budget.toml but none in launch_config.h")
    if full:
        lines += [f"{n}: gated in full but no cubin here (OptiX program or not built), not measured"
                  for n in sorted(budget) if budget[n]["gated"] == "full" and n not in measured]
    return fails, lines, deferred


def run(cmd, cwd=ROOT, env=None):
    r = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True)
    return r.returncode, (r.stdout or "") + (r.stderr or "")


def git(*args):
    code, out = run(["git", *args])
    if code != 0:
        raise GateError(f"git {' '.join(args)}: {out.strip()}")
    return out


def cuobjdump_path(build):
    cache = os.path.join(build, "CMakeCache.txt")
    if not os.path.exists(cache):
        raise GateError(f"{cache} missing: run tools/build.ps1 first")
    m = re.search(r"^CMAKE_CUDA_COMPILER:FILEPATH=(.+)$", open(cache, encoding="utf-8").read(), re.M)
    exe = os.path.join(os.path.dirname(m.group(1)), "cuobjdump.exe") if m else ""
    if not os.path.exists(exe):
        raise GateError("cuobjdump.exe not found next to the configured nvcc (is this a GPU build?)")
    return exe


def read_cubins(build, kernel_dir):
    cuobj = cuobjdump_path(build)
    d = os.path.join(build, kernel_dir)
    cubins = {}
    if not os.path.isdir(d):
        raise GateError(f"{d} missing: build astroray_kernels_gpu first")
    for f in sorted(os.listdir(d)):
        if not f.endswith(".cubin"):
            continue
        stem = f[:-6]
        code, res = run([cuobj, "-res-usage", os.path.join(d, f)])
        if code != 0:
            raise GateError(f"cuobjdump failed on {f}: {res.strip()}")
        log = os.path.join(d, stem + ".ptxas.txt")
        if not os.path.exists(log):
            raise GateError(f"{log} missing (the build saves the ptxas -v output)")
        cubins[stem] = (res, open(log, encoding="utf-8").read())
    return cubins


def base_budget(ref):
    """budget.toml rows at the merge base with `ref`; none if the file is new in this diff."""
    merge_base = git("merge-base", ref, "HEAD").strip()
    code, out = run(["git", "show", f"{merge_base}:{BUDGET}"])
    return parse_budget(out) if code == 0 else {}


def check_ptx(ptx_dir, launch, budget):
    """OptiX programs: every .entry of a PTX file needs rows (0002 D8 Verification)."""
    fails = []
    if os.path.isdir(ptx_dir):
        for f in sorted(os.listdir(ptx_dir)):
            if f.endswith(".ptx"):
                for name in re.findall(r"^(?:\.visible\s+)?\.entry\s+(\w+)", open(os.path.join(ptx_dir, f)).read(), re.M):
                    if name not in launch or name not in budget:
                        fails.append(f"{name} ({f}): OptiX program without a row in launch_config.h and budget.toml")
    return fails


PROBE_CAPPED_DIR = "kernels_probe_capped"


def split_probe(fails, lines, probe_cubins):
    """Failures of the capped probe kernels become warnings: a probe measures, it never gates."""
    names = {n for res, _ in probe_cubins.values() for n in parse_res_usage(res)}
    ours = lambda t: t.split(":", 1)[0].split(" ", 1)[0] in names
    return [f for f in fails if not ours(f)], [f for f in fails if ours(f)], [ln for ln in lines if ours(ln)]


def probe_warnings(warns):
    return ["WARNING (probe, capped) " + w for w in warns]


def gate_cubins(launch, budget, base, cubins, probe_cubins, is_stage, declared, full, extra_fails=()):
    """The one evaluation sequence of resources(), probe() and the self-test: evaluate() over the
    production cubins plus the capped probe cubins, then the probe kernels' breaches split off as
    warnings. Returns (failures, probe warnings, lines, probe lines, deferred)."""
    every = dict(cubins)
    every.update(probe_cubins)
    fails, lines, deferred = evaluate(launch, budget, base, every, is_stage, declared, full)
    fails, warns, plines = split_probe(fails + list(extra_fails), lines, probe_cubins)
    return fails, warns, lines, plines, deferred


def resources(args):
    build = os.path.join(ROOT, args.build_dir)
    launch = parse_launch_rows(open(os.path.join(ROOT, LAUNCH_CONFIG), encoding="utf-8").read())
    budget = parse_budget(open(os.path.join(ROOT, BUDGET), encoding="utf-8").read())
    cubins = read_cubins(build, "kernels")
    probe_cubins = {}
    if args.full:
        if os.path.isdir(os.path.join(build, PROBE_CAPPED_DIR)):
            probe_cubins = read_cubins(build, PROBE_CAPPED_DIR)
        else:
            print(f"kernelgate: {PROBE_CAPPED_DIR} not built yet; the probe rows are measured by --probe")

    def is_stage(stem):
        return not stem.startswith(("test_", "probe_")) and os.path.exists(os.path.join(ROOT, "src/kernel_gpu", stem + ".cu"))

    fails, warns, lines, _, deferred = gate_cubins(launch, budget, base_budget(args.base), cubins, probe_cubins, is_stage,
                                                   parse_declared(args.declared), args.full,
                                                   check_ptx(os.path.join(build, "optix"), launch, budget))
    for ln in lines:
        print(ln)
    for w in probe_warnings(warns):
        print(w)
    for f in fails:
        print("FAIL " + f)
    # Never a silent skip: the gate always says how many rows it left to `check full`.
    print(f"kernelgate: {len(deferred)} row(s) gated full deferred to check full" + (f": {', '.join(deferred)}" if deferred else ""))
    print(f"kernelgate: {len(cubins) + len(probe_cubins)} cubins, {len(fails)} failures")
    return 1 if fails else 0


def changed_files(ref):
    merge_base = git("merge-base", ref, "HEAD").strip()
    names = git("diff", "--name-only", merge_base).split() + git("ls-files", "--others", "--exclude-standard").split()
    return {n.replace("\\", "/") for n in names}


def timed_env(dump, base):
    """`base` with vcvarsall's variables over it. A dump without INCLUDE and LIB is no MSVC environment: raise,
    never time nvcc without it (0009 d18; nvcc would run vcvarsall itself inside the clock, owner decision 44)."""
    got = {}
    for line in dump:
        m = re.match(r"^([^=\s][^=]*)=(.*)$", line.rstrip("\r"))
        if m:
            got[m[1].upper()] = m[2]
    missing = [k for k in ("INCLUDE", "LIB") if not got.get(k)]
    if missing:
        raise GateError(f"the MSVC environment has no {' or '.join(missing)}: is vcvarsall's toolset installed?")
    return {**base, **got}


def msvc_environment():
    """The environment build.ps1 builds in, from the one implementation both use (tools/lib/msvc.ps1)."""
    script = os.path.join(ROOT, "tools", "lib", "msvc.ps1")
    r = subprocess.run(["pwsh", "-NoProfile", "-Command", f". '{script}'; Get-ArMsvcEnv"], cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        raise GateError(f"MSVC environment: {(r.stderr or r.stdout).strip()[-400:]}")
    return timed_env(r.stdout.splitlines(), os.environ)


def nvcc_command(build, stem, out_cubin):
    """The production nvcc call of ninja's rule for this cubin, minus depfile output."""
    code, text = run(["ninja", "-C", build, "-t", "commands", f"kernels/{stem}.cubin"])
    if code != 0:
        raise GateError(f"ninja -t commands kernels/{stem}.cubin: {text.strip()}")
    seg = next((s for ln in text.strip().splitlines() for s in ln.split(" && ") if " -- " in s), None)
    if not seg:
        raise GateError(f"no nvcc call in the build rule of {stem}")
    argv = shlex.split(seg.split(" -- ", 1)[1])
    out, i = [], 0
    while i < len(argv):
        if argv[i] in ("-MD",):
            i += 1
        elif argv[i] in ("-MF", "-o"):
            i += 2
        else:
            out.append(argv[i])
            i += 1
    return out[:-1] + ["-o", out_cubin, out[-1]]


def time_runs(stem, cmd, env, runner, clock):
    """One TU's serial nvcc runs, no cache, OS file cache warm from the build (0002 D10); a first time within the
    retime band of the limit is re-timed, three runs at most."""
    runs = []
    while True:
        t0 = clock()
        code, out = runner(cmd, env=env)
        runs.append(clock() - t0)
        if code != 0:
            raise GateError(f"nvcc failed timing {stem}: {out.strip()[-400:]}")
        limit = COMPILE_LIMIT_OVERRIDE_S.get(stem, COMPILE_LIMIT_S)
        near = abs(runs[0] - limit) <= COMPILE_RETIME_BAND * limit
        if not near or len(runs) == 3:
            return runs


def time_tus(cmds, get_env, runner=run, clock=time.perf_counter):
    """{stem: [seconds]} for [(stem, cmd)]. The MSVC environment comes from get_env once, before the first run, and
    every run gets it (owner decision 44: nvcc never runs vcvarsall inside the clock)."""
    env = get_env()
    return {stem: time_runs(stem, cmd, env, runner, clock) for stem, cmd in cmds}


def compile_timing(args):
    build = os.path.join(ROOT, args.build_dir)
    budget = parse_budget(open(os.path.join(ROOT, BUDGET), encoding="utf-8").read())
    changed = changed_files(args.base)
    cubins = read_cubins(build, "kernels")
    tus = []
    for stem, (res, _) in cubins.items():
        code, deps = run(["ninja", "-C", build, "-t", "deps", f"kernels/{stem}.cubin"])
        if code != 0:
            raise GateError(f"ninja -t deps kernels/{stem}.cubin: {deps.strip()}")
        paths = [p.strip() for p in deps.splitlines()[1:] if p.strip()]
        if not paths:
            raise GateError(f"ninja -t deps has no entry for kernels/{stem}.cubin (not built?)")
        used = {os.path.relpath(p, ROOT).replace("\\", "/") for p in paths if os.path.splitdrive(p)[0].lower() == os.path.splitdrive(ROOT)[0].lower()}
        if used & changed:
            cs = max([budget[n]["compile_s"] for n in parse_res_usage(res) if n in budget] or [0.0])
            tus.append((cs, stem))
    tus = sorted(tus, reverse=True)[:COMPILE_TUS]
    if not tus:
        print("compile timing: no changed device TU")
        return 0
    fails = []
    with tempfile.TemporaryDirectory() as tmp:
        cmds = [(stem, nvcc_command(build, stem, os.path.join(tmp, stem + ".cubin"))) for _, stem in tus]
        times = time_tus(cmds, msvc_environment)
    for cs, stem in tus:
        runs = times[stem]
        t = min(runs)
        limit = COMPILE_LIMIT_OVERRIDE_S.get(stem, COMPILE_LIMIT_S)
        print(f"compile {stem}: {t:.2f} s (limit {limit} s, {len(runs)} run(s), baseline {cs} s)")
        if t > limit:
            fails.append(f"B-05: {stem} compiles in {t:.2f} s, limit {limit} s")
    for f in fails:
        print("FAIL " + f)
    return 1 if fails else 0


def probe(args):
    build = os.path.join(ROOT, args.build_dir)
    # kernels_probe is built with AR_LAUNCH_BOUNDS compiled out: every count there is uncapped.
    cubins = read_cubins(build, "kernels_probe")
    for stem, (res, _) in sorted(cubins.items()):
        for name, m in sorted(parse_res_usage(res).items()):
            if m["regs"] > PROBE_LOUD:
                print(f"WARNING LOUD: {name} uses {m['regs']} registers uncapped (> {PROBE_LOUD}, 0002 D10)")
            elif m["regs"] >= PROBE_WARN:
                print(f"WARNING: {name} uses {m['regs']} registers uncapped (>= {PROBE_WARN}, 0002 D10)")
            else:
                print(f"{name}: {m['regs']} registers uncapped")
    # The probe modules' capped build: measured against their rows and 0002 D10, warnings only.
    probe_cubins = read_cubins(build, PROBE_CAPPED_DIR)
    launch = parse_launch_rows(open(os.path.join(ROOT, LAUNCH_CONFIG), encoding="utf-8").read())
    budget = parse_budget(open(os.path.join(ROOT, BUDGET), encoding="utf-8").read())
    _, warns, _, lines, _ = gate_cubins(launch, budget, {}, {}, probe_cubins, lambda s: False, {}, True)
    for ln in lines:
        print(f"capped {ln.split(' | ')[0]}")
    for w in probe_warnings(warns):
        print(w)
    return 0


def self_test():
    cases = os.path.join(ROOT, CASES)
    bad = 0
    for case in sorted(os.listdir(cases)):
        d = os.path.join(cases, case)
        read = lambda f: open(os.path.join(d, f), encoding="utf-8").read()
        want = read("expect.txt").split("\n")
        launch, budget = parse_launch_rows(read("launch_config.h")), parse_budget(read("budget.toml"))
        def case_cubins(sub):
            p = os.path.join(d, sub)
            if not os.path.isdir(p):
                return {}
            return {f[:-len(".resusage.txt")]: (read(os.path.join(sub, f)), read(os.path.join(sub, f.replace(".resusage.", ".ptxas."))))
                    for f in sorted(os.listdir(p)) if f.endswith(".resusage.txt")}
        # The case's own directory stands for kernels/, its kernels_probe_capped/ for the probe build.
        cubins, probe_cubins = case_cubins("."), case_cubins(PROBE_CAPPED_DIR)
        bp = os.path.join(d, "base_budget.toml")  # the rows at the merge base, when the case needs growth
        base = parse_budget(read("base_budget.toml")) if os.path.exists(bp) else {}
        fails, warns, _, _, deferred = gate_cubins(launch, budget, base, cubins, probe_cubins, lambda s: False, {},
                                                  want[2:3] == ["full"])
        got = "fail" if fails else "pass"
        if want[1].startswith("deferred="):
            ok = got == want[0] and len(deferred) == int(want[1][9:])
        else:
            ok = got == want[0] and (not want[1] or any(want[1] in f for f in fails))
        # Optional fourth line "warn=<text>": a probe warning line must contain it.
        if len(want) > 3 and want[3].startswith("warn="):
            ok = ok and any(want[3][5:] in w for w in probe_warnings(warns))
        bad += not ok
        print(f"case {case}: expect {want[0]}, got {got}" + (f" ({fails[0]})" if fails else "") + (": ok" if ok else ": MISMATCH"))
    # The timed command's environment: vcvarsall's INCLUDE and LIB reach it, and a dump without them raises.
    base = {"PATH": "p", "INCLUDE": "stale"}
    env = timed_env(["INCLUDE=inc", "LIB=lib", "Path=newp", "noise without equals", "=C:=C:\\x"], base)
    ok = env["INCLUDE"] == "inc" and env["LIB"] == "lib" and env["PATH"] == "newp"
    try:
        timed_env(["Path=newp"], base)
        ok = False
    except GateError:
        pass
    bad += not ok
    print(f"case timed_env: {'ok' if ok else 'MISMATCH'}")
    # The timed runs: the environment is fetched once and reaches every run, the near-limit re-times included.
    sentinel, calls, got_env = {"INCLUDE": "inc", "LIB": "lib"}, [], []
    def stub_run(cmd, env=None):
        got_env.append(env)
        return 0, ""
    ticks = iter(range(0, 1000))
    clock = lambda: next(ticks) * COMPILE_LIMIT_S  # every run reads the limit exactly: near the limit, so 3 runs each
    times = time_tus([("a", ["x"]), ("b", ["y"])], lambda: calls.append(1) or sentinel, stub_run, clock)
    ok = len(calls) == 1 and len(got_env) == 6 and all(e is sentinel for e in got_env) and times == {"a": [COMPILE_LIMIT_S] * 3, "b": [COMPILE_LIMIT_S] * 3}
    bad += not ok
    print(f"case time_tus: {'ok' if ok else 'MISMATCH'}")
    print(f"self-test: {'FAILED' if bad else 'ok'}")
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--build-dir", default="out/build")
    ap.add_argument("--base", default="origin/main", help="ref the diff is measured against")
    ap.add_argument("--declared", default="", help='the task\'s declared delta: "name:regs=+0,stack=+16;..."')
    ap.add_argument("--full", action="store_true", help="also check the rows gated full (check full)")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--time", action="store_true")
    mode.add_argument("--probe", action="store_true")
    mode.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    try:
        return self_test() if a.self_test else compile_timing(a) if a.time else probe(a) if a.probe else resources(a)
    except GateError as e:
        print(f"kernelgate: {e}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
