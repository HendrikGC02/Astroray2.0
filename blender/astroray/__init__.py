# SPDX-License-Identifier: GPL-3.0-or-later
"""Astroray extension entry point (0007 d1-d3)."""
from . import engine, native


def register():
    # Identity first: a mismatched binding must fail before any class exists.
    native.load()
    engine.register()


def unregister():
    engine.unregister()
