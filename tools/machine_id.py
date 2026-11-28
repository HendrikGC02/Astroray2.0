# SPDX-License-Identifier: Apache-2.0
"""The machine id and the machine rule for timing rows (owner decision 47, 0009 d13, d18).

One source for the id: `astroray_cli machine-id`, which prints what cli/bench.cpp computes for the history lines.
Nothing here recomputes it. The CLI needs the engine DLL (nvcc-built kernels) and a CUDA device, so it runs only in
the writers, which hold a full dev build under the machine lock: tools/build.ps1 and `check gate` call `refresh`,
which caches the answer in machine-id.json of the lock directory. Every reader (check fast, tools/test.ps1, the
history writers) reads that cache; a missing one is an error naming tools/build.ps1, never a guess.

    machine_id.py id                       print the cached id
    machine_id.py refresh [--sha S]        ask the CLI, rewrite the cache when it differs, print one line if it did
    machine_id.py reference                print the reference id of bench/machines.toml
    machine_id.py judge --row R --measured M --limit L [--unit s] [--id X] [--machines FILE]
                                           print one JSON line {verdict, limit, line}; verdict is pass, fail or off
    machine_id.py record --id X --row R --value V [--machines FILE]
                                           write the measured median of a row into [machine."X"] (check.ps1 profile)
    machine_id.py record-reference --row R --value V --note TEXT [--machines FILE]
                                           the same for [reference_median] (check.ps1 profile -Reference)

Importable: machine_id(), refresh(), judge(), timing_ok() (the timing CHECKs inside tests), record().
Ref: docs/decisions/0000-owner-decisions.md decision 47 (a-f); bench/machines.toml.
"""
import argparse
import datetime
import json
import os
import re
import subprocess
import sys
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MACHINES = os.path.join(ROOT, "bench", "machines.toml")
# ASTRORAY_CLI names another CLI binary (a fixture whose copy of the tools has no build of its own); the id is still the CLI's.
CLI = os.environ.get("ASTRORAY_CLI") or os.path.join(ROOT, "out", "bin", "astroray_cli.exe")
# Set by the tools (never by hand, except to prove an off-reference path) to the id of a machine that is not the
# reference one: a timing check inside a test then prints its measurement against the limit instead of failing.
OFF_REFERENCE_ENV = "ASTRORAY_OFF_REFERENCE"


class MachineError(RuntimeError):
    pass


def cache_path():
    """machine-id.json in the lock directory (tools/lib/lock.ps1 Get-ArLockDir): one file per machine."""
    d = os.environ.get("ASTRORAY_LOCK_DIR") or os.path.join(os.environ.get("ProgramData", ""), "astroray2")
    return os.path.join(d, "machine-id.json")


def ask_cli(cli=CLI):
    """The id `astroray_cli machine-id` prints. Only the writers call this (refresh): they hold a full dev build
    under the machine lock; a reader never runs the CLI, which needs the engine DLL and a CUDA device."""
    if not os.path.exists(cli):
        raise MachineError(f"{cli} is missing: run tools/build.ps1")
    p = subprocess.run([cli, "machine-id"], capture_output=True, text=True, timeout=120)
    m = re.search(r"^machine-id: (\S+)$", p.stdout, re.M)
    if p.returncode != 0 or not m:
        raise MachineError(f"`{cli} machine-id` exited {p.returncode} without an id: {(p.stdout + p.stderr).strip()[-300:]}")
    return m.group(1)


def read_cache(path=None):
    path = path or cache_path()
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as f:
        d = json.load(f)
    if not isinstance(d.get("id"), str) or not d["id"]:
        raise MachineError(f"{path} holds no id: delete it and run tools/build.ps1")
    return d


def machine_id(cache=None):
    """The id of this machine, read from the cache a full dev build wrote (build.ps1, check gate). A missing cache is
    an error naming tools/build.ps1: one full build writes it; nothing here builds or guesses."""
    d = read_cache(cache)
    if d is None:
        raise MachineError(f"no machine id cached at {cache or cache_path()}: run tools/build.ps1 (a full dev build writes it)")
    return d["id"]


def refresh(cli=CLI, cache=None, sha=""):
    """Writer: asks the CLI and (re)writes the cache when it differs. Returns the one line to print ('' when the cache
    is current). A CLI that refuses (no CUDA device after a driver reset, a stale binary) leaves the cache as it is,
    so the reference machine keeps its identity, and says so."""
    path = cache or cache_path()
    old = read_cache(path)
    try:
        new = ask_cli(cli)
    except MachineError as e:
        return f"machine-id cache kept ({old['id'] if old else 'none'}), not refreshed: {e}"
    if old and old["id"] == new:
        return ""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump({"id": new, "date": datetime.date.today().isoformat(), "sha": sha}, f)
    return f"machine-id cache {'written' if not old else 'changed from ' + old['id'] + ' to'} {new} ({path})"


def load_machines(path=MACHINES):
    if not os.path.exists(path):
        raise MachineError(f"{path} is missing: it names the reference machine")
    with open(path, "rb") as f:
        t = tomllib.load(f)
    if not isinstance(t.get("reference"), str) or not t["reference"]:
        raise MachineError(f"{path} has no `reference = \"<machine id>\"`")
    return t


def reference(path=MACHINES):
    return load_machines(path)["reference"]


def _fmt(v, unit):
    return f"{v:.4g} {unit}".rstrip()


def judge(row, measured, limit, mid=None, unit="", machines=MACHINES):
    """(verdict, effective limit, line) of one timing row (decision 47 c). pass/fail against the limit on the
    reference machine; on another machine with a profile (a measured median for the row beside the reference's) the
    limit is the reference limit x (median / reference median), never below the reference limit (no limit is
    edited to fit a machine, 47 f; a faster machine keeps the reference limit); without a profile an overrun is
    `off` with its line and does not fail. A row within its reference limit passes without reading the id."""
    if measured <= limit:
        return "pass", limit, ""
    t = load_machines(machines)
    mid = mid or machine_id()
    if mid == t["reference"]:
        return "fail", limit, ""
    prof = t.get("machine", {}).get(mid, {})
    ref = t.get("reference_median", {})
    if row in prof and row in ref:
        if not (prof[row] > 0 and ref[row] > 0):
            raise MachineError(f"{machines}: the medians of {row} must be positive")
        derived = limit * max(prof[row] / ref[row], 1.0)
        line = (f"profile {mid} {row}: {_fmt(measured, unit)} against {_fmt(derived, unit)} "
                f"(reference limit {_fmt(limit, unit)} x {prof[row]:g}/{ref[row]:g})")
        return ("pass" if measured <= derived else "fail"), derived, line
    return "off", limit, f"off-reference {mid} {row}: {_fmt(measured, unit)} against {_fmt(limit, unit)}"


def timing_ok(row, measured, limit, unit=""):
    """For a timing CHECK inside a test: True when within the limit; with ASTRORAY_OFF_REFERENCE set (the tools set it
    on a machine that is not the reference one) an overrun prints one off-reference line and is also True. Unset
    (the reference machine, hosted ci, a hand run), an overrun is False: nothing changes."""
    if measured <= limit:
        return True
    mid = os.environ.get(OFF_REFERENCE_ENV)
    if not mid:
        return False
    print(f"off-reference {mid} {row}: {_fmt(measured, unit)} against {_fmt(limit, unit)}", flush=True)
    return True


def record(mid, row, value, machines=MACHINES):
    """Rewrites [machine."mid"] of the file with row = value (other rows of that machine and every other line
    stay). Refuses the reference id: its medians are the records' numbers, edited by hand."""
    t = load_machines(machines)
    if mid == t["reference"]:
        raise MachineError(f"{mid} is the reference machine: its medians are the recorded ones in [reference_median]")
    if not value > 0:
        raise MachineError(f"median of {row} is {value}: not a measurement")
    rows = dict(t.get("machine", {}).get(mid, {}))
    rows[row] = value
    with open(machines, encoding="utf-8", newline="") as f:
        text = f.read().replace("\r\n", "\n")
    head = f'[machine."{mid}"]'
    # Drop the old section: from its header to the next header at line start (or the end).
    text = re.sub(r"(?ms)^" + re.escape(head) + r"\n.*?(?=^\[|\Z)", "", text).rstrip("\n") + "\n"
    text += "\n" + head + "\n" + "".join(f'"{k}" = {v:.6g}\n' for k, v in sorted(rows.items()))
    with open(machines, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def record_reference(row, value, note, machines=MACHINES):
    """Writes the reference machine's median of `row` into [reference_median] with `note` (a comment line: how, when,
    which sha) above it, replacing that row's old comment and value; the other rows and lines stay. Lead only
    (`check.ps1 profile -Reference`)."""
    load_machines(machines)
    if not value > 0:
        raise MachineError(f"median of {row} is {value}: not a measurement")
    with open(machines, encoding="utf-8", newline="") as f:
        text = f.read().replace("\r\n", "\n")
    entry = f'# {note}\n"{row}" = {value:.6g}\n'
    old = re.compile(r'(?m)^(?:#[^\n]*\n)*^"' + re.escape(row) + r'" = [^\n]*\n')
    if old.search(text):
        text = old.sub(lambda _: entry, text, count=1)
    elif "[reference_median]\n" in text:
        text = text.replace("[reference_median]\n", "[reference_median]\n" + entry, 1)
    else:
        text = text.rstrip("\n") + "\n\n[reference_median]\n" + entry
    with open(machines, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("id")
    rf = sub.add_parser("refresh")
    rf.add_argument("--sha", default="")
    r = sub.add_parser("reference")
    r.add_argument("--machines", default=MACHINES)
    j = sub.add_parser("judge")
    j.add_argument("--row", required=True)
    j.add_argument("--measured", type=float, required=True)
    j.add_argument("--limit", type=float, required=True)
    j.add_argument("--unit", default="")
    j.add_argument("--id", default="")
    j.add_argument("--machines", default=MACHINES)
    w = sub.add_parser("record")
    w.add_argument("--id", required=True)
    w.add_argument("--row", required=True)
    w.add_argument("--value", type=float, required=True)
    w.add_argument("--machines", default=MACHINES)
    q = sub.add_parser("record-reference")
    q.add_argument("--row", required=True)
    q.add_argument("--value", type=float, required=True)
    q.add_argument("--note", required=True)
    q.add_argument("--machines", default=MACHINES)
    a = ap.parse_args(argv)
    try:
        if a.cmd == "id":
            print(machine_id())
        elif a.cmd == "refresh":
            line = refresh(sha=a.sha)
            if line:
                print(line)
        elif a.cmd == "reference":
            print(reference(a.machines))
        elif a.cmd == "judge":
            v, lim, line = judge(a.row, a.measured, a.limit, a.id or None, a.unit, a.machines)
            print(json.dumps({"verdict": v, "limit": lim, "line": line}))
        elif a.cmd == "record-reference":
            record_reference(a.row, a.value, a.note, a.machines)
        else:
            record(a.id, a.row, a.value, a.machines)
    except MachineError as e:
        print(f"FAIL machine_id: {e}")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
