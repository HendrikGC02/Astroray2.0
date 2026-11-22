# SPDX-License-Identifier: Apache-2.0
"""Generate the addon's op schema and the node coverage matrix from ops.def.

  opsgen.py [--root DIR] [--out DIR]   write _ops.py and coverage.csv
  opsgen.py --self-test                prove a volume-reachable row without an
                                       interval rule, and an ineligible foldable
                                       row, are refused

Ref: docs/decisions/0005-material-model.md items 30, 40; 0006 D7.
"""
import argparse
import ast
import os
import re
import sys

NONE_RULE = "NONE_SURFACE_ONLY"
DISPOSITIONS = ("implemented", "approximated", "ignored", "unsupported")
COSTS = ("CHEAP", "MEM", "TEX")
TYPES = ("", "f32", "f32x3", "spec")
# Operand tokens: the bytes the interpreter reads (ops.def header).
OPERANDS = ("reg", "imm", "uvset", "lift", "regrgbs", "slotimm", "jmpoff")
OPS_DEF = "src/kernel/vm/ops.def"
LOWERING = "blender/astroray/materials/lowering_table.py"
NODE_LIST = "tests/data/shader_node_types_5_3.txt"


class OpsError(Exception):
    pass


def split_args(s):
    out, cur, q = [], "", False
    for ch in s:
        if ch == '"':
            q = not q
        if ch == "," and not q:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    out.append(cur.strip())
    return out


def parse_table(text):
    """Rows of ops.def as dicts, in order. Raises OpsError on any bad row."""
    rows = []
    for no, line in enumerate(text.splitlines(), 1):
        line = line.split("//")[0].strip()
        if not line.startswith("AR_OP("):
            continue
        if not line.endswith(")"):
            raise OpsError(f"ops.def:{no}: row must be one line ending in ')'")
        a = split_args(line[len("AR_OP("):-1])
        if len(a) != 9:
            raise OpsError(f"ops.def:{no}: expected 9 fields, got {len(a)}")
        name, arity, operands, result, cost, rule, vol, test, fold = a
        if any(x not in ("true", "false") for x in (vol, test, fold)):
            raise OpsError(f"ops.def:{no}: {name}: flags must be true or false")
        r = dict(name=name, arity=int(arity), operands=operands.strip('"'),
                 result=result.strip('"'), cost=cost, rule=rule,
                 volume=vol == "true", test_only=test == "true", foldable=fold == "true", line=no)
        if r["result"] not in TYPES:
            raise OpsError(f"ops.def:{no}: {name}: unknown result type {r['result']!r}")
        for tok in r["operands"].split("+") if r["operands"] else ():
            if tok not in OPERANDS:
                raise OpsError(f"ops.def:{no}: {name}: unknown operand token {tok!r}")
        if cost not in COSTS:
            raise OpsError(f"ops.def:{no}: {name}: unknown cost class {cost}")
        if r["foldable"]:
            # ops.def header: foldable needs a value result, no test hook, and operands the fold test can rebuild.
            toks = r["operands"].split("+") if r["operands"] else []
            why = ("an empty or spec result type" if r["result"] not in ("f32", "f32x3")
                   else "test_only" if r["test_only"]
                   else f"operand token(s) other than reg and imm: {r['operands']!r}"
                   if any(t not in ("reg", "imm") for t in toks) else "")
            if why:
                raise OpsError(f"ops.def:{no}: {name}: foldable is not allowed with {why}")
        rows.append(r)
    names = [r["name"] for r in rows]
    if len(set(names)) != len(names):
        raise OpsError("ops.def: duplicate opcode name")
    if len(rows) > 256:
        raise OpsError("ops.def: more than 256 opcodes do not fit the u8 opcode")
    for r in rows:
        if r["rule"] == NONE_RULE:
            if r["volume"]:
                raise OpsError(f"ops.def:{r['line']}: {r['name']} is volume_reachable but has no "
                               f"interval rule (0006 D7); name interval_<name> or clear the flag")
        elif not re.fullmatch(r"interval_[a-z0-9_]+", r["rule"]):
            raise OpsError(f"ops.def:{r['line']}: {r['name']}: interval rule {r['rule']!r} is neither "
                           f"interval_<name> nor {NONE_RULE}")
    return rows


def read_lowering(text):
    """The LOWERING dict literal of lowering_table.py, without executing it."""
    for n in ast.parse(text).body:
        if isinstance(n, ast.Assign) and any(getattr(t, "id", "") == "LOWERING" for t in n.targets):
            return ast.literal_eval(n.value)
    raise OpsError(f"{LOWERING}: no LOWERING assignment")


def coverage(rows, lowering, nodes):
    op_names = {r["name"] for r in rows}
    for node, ent in lowering.items():
        if node not in nodes:
            raise OpsError(f"{LOWERING}: {node} is not a ShaderNode type of Blender 5.3")
        if ent.get("disposition") not in DISPOSITIONS:
            raise OpsError(f"{LOWERING}: {node}: disposition must be one of {DISPOSITIONS}")
        for o in ent.get("ops", ()):
            if o not in op_names:
                raise OpsError(f"{LOWERING}: {node}: unknown op {o}")
    out = ["node_type,disposition,ops,note"]
    for n in nodes:
        ent = lowering.get(n)
        if ent is None:
            out.append(f"{n},unsupported,,no lowering")
        else:
            out.append(f"{n},{ent['disposition']},{'+'.join(ent.get('ops', ()))},{ent.get('note', '')}")
    return "\n".join(out) + "\n"


def emit_ops_py(rows):
    L = ["# SPDX-License-Identifier: Apache-2.0",
         "# Generated by tools/opsgen.py from src/kernel/vm/ops.def. Do not edit.",
         "OPS = ["]
    for i, r in enumerate(rows):
        L.append(f"    dict(name={r['name']!r}, id={i}, arity={r['arity']}, operands={r['operands']!r}, "
                 f"result={r['result']!r}, volume_reachable={r['volume']}, test_only={r['test_only']}, "
                 f"foldable={r['foldable']}),")
    L += ["]", "OP_ID = {o['name']: o['id'] for o in OPS}", ""]
    return "\n".join(L)


def write_if_changed(path, text):
    if os.path.exists(path):
        with open(path, encoding="utf-8", newline="") as f:
            if f.read() == text:
                return
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def self_test():
    """Each seeded bad row must be refused, with the message naming the row."""
    cases = [
        ("a volume-reachable row without an interval rule",
         'AR_OP(BAD, 0, "", "f32", CHEAP, NONE_SURFACE_ONLY, true, false, false)', "interval rule"),
        ("a foldable row with a lift operand",
         'AR_OP(BAD, 1, "reg+lift", "f32", CHEAP, NONE_SURFACE_ONLY, false, false, true)', "BAD: foldable"),
        ("a foldable row with an empty result",
         'AR_OP(BAD, 1, "reg", "", CHEAP, NONE_SURFACE_ONLY, false, false, true)', "BAD: foldable"),
        ("a foldable test_only row",
         'AR_OP(BAD, 0, "reg", "f32", CHEAP, NONE_SURFACE_ONLY, false, true, true)', "BAD: foldable"),
        ("an eight-field row",
         'AR_OP(BAD, 0, "reg", "f32", CHEAP, NONE_SURFACE_ONLY, false, false)', "expected 9 fields"),
    ]
    bad = 0
    for what, row, needle in cases:
        try:
            parse_table(row)
        except OpsError as e:
            if needle in str(e):
                print(f"opsgen self-test: refused {what}: {e}")
                continue
            print(f"opsgen self-test FAILED: {what} was refused with the wrong message: {e}")
        else:
            print(f"opsgen self-test FAILED: {what} was accepted")
        bad += 1
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap.add_argument("--out")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    out = a.out or os.path.join(a.root, "out", "build", "gen")
    rd = lambda p: open(os.path.join(a.root, p), encoding="utf-8").read()
    try:
        rows = parse_table(rd(OPS_DEF))
        nodes = [ln.strip() for ln in rd(NODE_LIST).splitlines() if ln.strip()]
        csv = coverage(rows, read_lowering(rd(LOWERING)), nodes)
    except OpsError as e:
        print(f"opsgen: {e}", file=sys.stderr)
        return 1
    os.makedirs(out, exist_ok=True)
    write_if_changed(os.path.join(out, "_ops.py"), emit_ops_py(rows))
    write_if_changed(os.path.join(out, "coverage.csv"), csv)
    print(f"opsgen: {len(rows)} ops, {len(nodes)} node types -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
