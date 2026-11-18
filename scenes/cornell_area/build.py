# SPDX-License-Identifier: Apache-2.0
"""Bitterli's Cornell box lit by the original rectangular lamp as an area lamp. Thin entry over scenes/cornell/cornell_box.py.
Usage: build.py --out <path.arrec> [--blend <path.blend>]"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "cornell"))
import cornell_box  # noqa: E402

if __name__ == "__main__":
    cornell_box.main("area")
