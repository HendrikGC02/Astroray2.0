# SPDX-License-Identifier: Apache-2.0
"""B-5M with two kinds of material program (task 122): b5m/build.py unchanged, except that every
second material's base colour is its constant colour times the hit's UV (LOAD_CTX_UV, MUL_F32X3).
Those materials compile a surface program each, and the others fold to constants
(CONSTANT_SURFACE): 300 materials over 151 programs (sort.h sort_program_count). It measures the
material sort where programs differ (RULINGS 80); not a bench-set scene.
Usage: build.py --out <path.arrec>.
Ref: scenes/b5m/build.py; docs/decisions/RULINGS.md ruling 80."""
import argparse
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "lib"))
import ar  # noqa: E402

_spec = importlib.util.spec_from_file_location("b5m_build", os.path.join(os.path.dirname(HERE), "b5m", "build.py"))
b5m = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(b5m)
_constant_material = ar.Scene.material


def material(self, inputs=None, emission=None, emission_sampling=None):
    """Even calls: b5m's constant material; odd calls: base colour = colour x UV, a per-point input."""
    self._b5m_count = getattr(self, "_b5m_count", 0) + 1
    if self._b5m_count % 2 == 1 or emission is not None or "base_color" not in (inputs or {}):
        return _constant_material(self, inputs, emission, emission_sampling)
    g = self.graph()
    colour, uv = g.const(inputs["base_color"]), g.uv()
    mul = g.add("MUL_F32X3")
    g.link(colour.index, mul, 0)
    g.link(uv.index, mul, 1)
    g.store("base_color", ar.Node(mul))
    for name, value in inputs.items():
        if name != "base_color":
            g.store(name, value)
    return self.material_from_graph(g, emission_sampling)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, help="the .arrec to write")
    a = ap.parse_args()
    ar.Scene.material = material
    b5m.build(a.out)
