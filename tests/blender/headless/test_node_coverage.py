# SPDX-License-Identifier: Apache-2.0
"""0007 V10 first clause (0005 item 40): the coverage matrix has a row for every ShaderNode type.

The committed node list and the generated matrix are compared with the ShaderNode types of the
Blender this runs in: the pinned bpy wheel on hosted ci, Blender 5.3 in the full tier. A node
that a Blender update adds or removes fails here until the list is regenerated
(tests/blender/list_shader_nodes.py) and the new node has a disposition.
"""
import csv
import os
import sys
import unittest

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import support  # noqa: E402

LIST = os.path.join(support.ROOT, "tests", "data", "shader_node_types_5_3.txt")
CSV = os.path.join(support.ROOT, "out", "build", "gen", "coverage.csv")


class TestNodeCoverage(unittest.TestCase):
    def test_row_set_equals_bpy(self):
        live = sorted(n for n in dir(bpy.types) if n.startswith("ShaderNode"))
        with open(LIST, encoding="utf-8") as f:
            listed = [ln.strip() for ln in f if ln.strip()]
        self.assertEqual(listed, live, "tests/data/shader_node_types_5_3.txt differs from this Blender's ShaderNode types")
        if not os.path.exists(CSV):
            raise RuntimeError(f"{CSV} is missing: run tools/build.ps1 first")
        with open(CSV, encoding="utf-8", newline="") as f:
            rows = list(csv.DictReader(f))
        self.assertEqual([r["node_type"] for r in rows], live)


if __name__ == "__main__":
    support.main()
