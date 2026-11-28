# SPDX-License-Identifier: Apache-2.0
"""Writes b1m_m1b's .blend with the Astroray extension enabled: the GRATING card is a material kind that lives in
material.astroray, which exists only when the add-on is registered in the Blender that runs scenes/lib/blend_writer.py.
Usage: blender --background --python write_blend.py -- <desc.json> <out.blend>   (through tools/measure_viewport.ps1)
Ref: scenes/lib/blend_writer.py build (extension=True); tests/blender/smoke_a1.py install_and_enable."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tests", "blender"))
sys.path.insert(0, os.path.join(ROOT, "scenes", "lib"))
import smoke_a1  # noqa: E402
import blend_writer  # noqa: E402

args = sys.argv[sys.argv.index("--") + 1:]
if len(args) != 2:
    raise SystemExit("usage: blender --background --python write_blend.py -- <desc.json> <out.blend>")
smoke_a1.install_and_enable()
blend_writer.build(args[0], args[1])
