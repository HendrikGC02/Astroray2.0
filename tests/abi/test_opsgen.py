# SPDX-License-Identifier: Apache-2.0
"""tools/opsgen.py: engine ids parsed from the kernel headers, malformed input refused, the matrix.

The addon holds no copy of these ids (0005 item 40), so the pins here check the parser against
values read from the headers by hand.
"""
import csv
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import opsgen  # noqa: E402


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


GOOD_SEM = "enum class SpectrumSemantic : uint8_t { REFLECTANCE = 0, UNBOUNDED, IOR };"
GOOD_SLOTS = ("#define AR_RECORD_SCALARS(X) X(a) X(b)\n#define AR_RECORD_SPECTRAL(X) X(c, C, IOR)\n"
              "#define AR_RECORD_FRAMES(X) X(d)\n")


class TestOpsgen(unittest.TestCase):
    def test_header_ids_match_pins(self):
        sem = opsgen.parse_semantics(read(opsgen.SPECTRUM_REF_H))
        slots = opsgen.parse_record_slots(read(opsgen.STORE_SLOT_H), sem)
        tex = opsgen.parse_tex_modes(read(opsgen.TEX_IMAGE_H))
        self.assertEqual((slots["specular_weight"], slots["specular_roughness"], slots["base_color"]), (3, 4, 24))
        self.assertEqual((tex["LINEAR"], tex["EXTEND"]), (1, 2))
        self.assertEqual(sem["REFLECTANCE"], 0)
        # ops.def's slotimm comment: 0-23 scalar, 24-32 spectral, 33-36 frames.
        self.assertEqual(sorted(slots.values()), list(range(37)))
        self.assertEqual(slots["geometry_normal"], 33)

    def test_good_fixtures_parse(self):
        sem = opsgen.parse_semantics(GOOD_SEM)
        self.assertEqual(opsgen.parse_record_slots(GOOD_SLOTS, sem), {"a": 0, "b": 1, "c": 2, "d": 3})

    def test_malformed_header_raises(self):
        sem = opsgen.parse_semantics(GOOD_SEM)
        bad = {
            "no semantic enum": lambda: opsgen.parse_semantics("struct S {};"),
            "bad enumerator": lambda: opsgen.parse_semantics("enum class SpectrumSemantic { A B };"),
            "no spectral macro": lambda: opsgen.parse_record_slots("#define AR_RECORD_SCALARS(X) X(a)\n", sem),
            "empty frames": lambda: opsgen.parse_record_slots(GOOD_SLOTS.replace("X(d)", ""), sem),
            "unknown semantic": lambda: opsgen.parse_record_slots(GOOD_SLOTS.replace("IOR", "NOPE"), sem),
            "duplicate field": lambda: opsgen.parse_record_slots(GOOD_SLOTS.replace("X(d)", "X(a)"), sem),
            "no kTexExtend": lambda: opsgen.parse_tex_modes("kTexLinear = 1u;"),
            "no kTexLinear": lambda: opsgen.parse_tex_modes("kTexExtend = 2u;"),
        }
        for what, fn in bad.items():
            with self.subTest(what), self.assertRaises(opsgen.OpsError):
                fn()

    def test_lowering_partition_is_checked(self):
        rows = opsgen.parse_table(read(opsgen.OPS_DEF))
        node = {"disposition": "approximated", "ops": [], "note": "x"}
        impl = {"disposition": "implemented", "ops": [], "note": "x"}
        nodes = ["A", "B", "C"]
        text = opsgen.coverage(rows, {"A": impl}, nodes, {"B": node}).splitlines()
        self.assertEqual(text[1:], ["A,implemented,,x", "B,approximated,,x", "C,unsupported,,no lowering"])
        for what, low, planned in (("in both", {"A": impl}, {"A": node}),
                                   ("absent from the list", {}, {"Z": node}),
                                   ("planned but implemented", {}, {"A": impl})):
            with self.subTest(what), self.assertRaises(opsgen.OpsError):
                opsgen.coverage(rows, low, nodes, planned)

    def test_matrix_and_generated_module(self):
        with tempfile.TemporaryDirectory() as out:
            r = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "opsgen.py"),
                                "--root", ROOT, "--out", out], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            with open(os.path.join(out, "coverage.csv"), encoding="utf-8", newline="") as f:
                rows = list(csv.DictReader(f))
            ns = {}
            with open(os.path.join(out, "_ops.py"), encoding="utf-8") as f:
                exec(compile(f.read(), "_ops.py", "exec"), ns)
        # The row count is the node list's; ops.def rows do not change it.
        names = [ln.strip() for ln in read(opsgen.NODE_LIST).splitlines() if ln.strip()]
        self.assertEqual(len(rows), 108)
        self.assertEqual([x["node_type"] for x in rows], names)
        for x in rows:
            self.assertTrue(x["node_type"] and x["disposition"] and x["note"], f"blank field in {x}")
            self.assertIn(x["disposition"], opsgen.DISPOSITIONS)
            if x["disposition"] == "implemented":
                self.assertTrue(set(x["ops"].split("+")) - {""} <= set(ns["OP_ID"]), x)
        self.assertEqual((ns["RECORD_SLOT"]["base_color"], ns["TEX_MODE"]["EXTEND"], ns["SEMANTIC"]["REFLECTANCE"]),
                         (24, 2, 0))

    def test_self_test_passes(self):
        r = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "opsgen.py"), "--self-test"],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main()
