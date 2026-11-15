# SPDX-License-Identifier: Apache-2.0
"""Closed-form truths of the analytic scenes (0009 d16, d20), named by meta.toml's [truth] formula.

A truth function takes the [truth] table and the image size and returns a list of checks, each
(name, weights, truth, kind): the measured channel is weights . (display R, G, B) per pixel and
truth is an (height, width) array in display units (working-space RGB, film exposure 1). kind
"pixels" tests every pixel, "image" tests the image mean. Every check is one member of the
analytic family per device (tools/analytic.py).
Ref: docs/decisions/0009-build-test-process.md d16, d20; 0003 d11 (display Y, the unit anchor);
src/host/data_resource.h (observer.bin layout).
"""
import os
import struct

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OBSERVER_BIN = os.path.join(ROOT, "data", "spectral", "observer.bin")

# observer.bin layout from src/host/data_resource.h: magic[8], version, kind, hash[8], then (after
# the sampling words) the CMFs at byte 36, Y_n, K, the D65 SPD and M_ws (9 doubles, row-major).
_SAMPLES = 471
_CMF_AT = 36
_MWS_AT = _CMF_AT + 3 * 4 * _SAMPLES + 8 + 8 + 4 * _SAMPLES
_BYTES = _MWS_AT + 72


def working_luminance_row():
    """The row of M_ws^-1 that gives Y from working-space RGB: display Y = row . RGB, because
    working RGB = M_ws . XYZ (0003 d11). M_ws is read from the shipped observer.bin."""
    with open(OBSERVER_BIN, "rb") as f:
        raw = f.read()
    if len(raw) != _BYTES or raw[:6] != b"ARDATA":
        raise ValueError(f"{OBSERVER_BIN}: not the observer.bin layout of src/host/data_resource.h")
    m_ws = np.array(struct.unpack_from("<9d", raw, _MWS_AT)).reshape(3, 3)
    return np.linalg.inv(m_ws)[1]


def _emitted_rgb(t):
    colour = np.asarray(t["colour"], dtype=np.float64)
    if colour.shape != (3,):
        raise ValueError("truth colour must be an RGB triple")
    return colour * float(t["radiance"])


def emitter_radiance(t, width, height):
    """A5: a view-filling emitter through a pinhole; every pixel's display RGB is the emitted
    colour times radiance (0009 d20: pixel = L)."""
    rgb = _emitted_rgb(t)
    eye = np.eye(3)
    return [(n, eye[i], np.full((height, width), rgb[i]), "pixels") for i, n in enumerate("RGB")]


def display_luminance(t, width, height):
    """y_anchor: an RGB white emitter of strength L has display Y = L, the unit anchor (0003 d11,
    check 11: white of strength 1 gives Y = 1). The truth is that literal value; the measured Y is
    formed with the working space's luminance row, so an error in M_ws or the white point shows.
    Checked per pixel and, at the record's 1e-3 precision, as the image mean."""
    if list(t["colour"]) != [1.0, 1.0, 1.0]:
        raise ValueError("display_luminance is the unit anchor: its emitter colour must be white (1, 1, 1)")
    row = working_luminance_row()
    truth = np.full((height, width), 1.0 * float(t["radiance"]))
    return [("Y", row, truth, "pixels"), ("Y image mean", row, truth, "image")]

FORMULAS = {"emitter_radiance": emitter_radiance, "display_luminance": display_luminance}


def evaluate(truth_table, width, height):
    name = truth_table["formula"]
    if name not in FORMULAS:
        raise ValueError(f"unknown truth formula '{name}'; known: {sorted(FORMULAS)}")
    return FORMULAS[name](truth_table, width, height)
