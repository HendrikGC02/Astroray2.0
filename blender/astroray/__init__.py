# SPDX-License-Identifier: GPL-3.0-or-later
"""Astroray extension entry point (0007 d1-d3)."""
from . import _abi, diagnostics, engine, native, properties, ui


def register():
    # Identity first: a mismatched binding must fail before any class exists.
    lib = native.load()
    # Read once here: the Diagnostics panel's draw makes no ABI call (0007 d26).
    diagnostics.set_identity(native._build_line(_abi, lib), _abi.ABI_HASH)
    engine.register()
    properties.register()
    ui.register()


def unregister():
    ui.unregister()
    properties.unregister()
    engine.unregister()
