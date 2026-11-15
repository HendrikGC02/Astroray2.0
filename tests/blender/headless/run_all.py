# SPDX-License-Identifier: Apache-2.0
"""Runs the headless sync suite: inside Blender (Invoke-ArBlender) or with the bpy wheel (CI)."""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

suite = unittest.defaultTestLoader.discover(HERE, pattern="test_*.py", top_level_dir=HERE)
result = unittest.TextTestRunner(stream=sys.stdout, verbosity=2).run(suite)
sys.exit(0 if result.wasSuccessful() else 1)
