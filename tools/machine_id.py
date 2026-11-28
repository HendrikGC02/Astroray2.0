# SPDX-License-Identifier: Apache-2.0
"""The machine id and the machine rule for timing rows (owner decision 47, 0009 d13, d18).

One source for the id: `astroray_cli machine-id`, which prints what cli/bench.cpp computes for the history lines.
Nothing here recomputes it; a CLI that is not built is an error naming tools/build.ps1, never a guess.

    machine_id.py id                       print the id
    machine_id.py reference                print the reference id of bench/machines.toml
    machine_id.py judge --row R --measured M --limit L [--unit s] [--id X] [--machines FILE]
                                           print one JSON line {verdict, limit, line}; verdict is pass, fail or off
    machine_id.py record --id X --row R --value V [--machines FILE]
                                           write the measured median of a row into [machine."X"] (check.ps1 profile)

Importable: machine_id(), judge(), timing_ok() (the timing CHECKs inside tests), record().
Ref: docs/decisions/0000-owner-decisions.md decision 47 (a-f); bench/machines.toml.
"""
import argparse
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
_ids = {}


class MachineError(RuntimeError):
    pass


def machine_id(cli=CLI):
    """The id astroray_cli computes, once per process and CLI path."""
    if cli in _ids:
        return _ids[cli]
    if not os.path.exists(cli):
        raise MachineError(f"{cli} is missing: the machine id comes from `astroray_cli machine-id`; run tools/build.ps1")
    p = subprocess.run([cli, "machine-id"], capture_output=True, text=True, timeout=120)
    m = re.search(r"^machine-id: (\S+)$", p.stdout, re.M)
    if p.returncode != 0 or not m:
        raise MachineError(f"`{cli} machine-id` exited {p.returncode} without an id: {(p.stdout + p.stderr).strip()[-300:]}")
    _ids[cli] = m.group(1)
    return _ids[cli]


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


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("id")
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
    a = ap.parse_args(argv)
    try:
        if a.cmd == "id":
            print(machine_id())
        elif a.cmd == "reference":
            print(reference(a.machines))
        elif a.cmd == "judge":
            v, lim, line = judge(a.row, a.measured, a.limit, a.id or None, a.unit, a.machines)
            print(json.dumps({"verdict": v, "limit": lim, "line": line}))
        else:
            record(a.id, a.row, a.value, a.machines)
    except MachineError as e:
        print(f"FAIL machine_id: {e}")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
