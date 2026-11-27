# SPDX-License-Identifier: Apache-2.0
"""A GRATING material's authored block through the C ABI (0005 items 16, 23, 24; 0007 appendix row
ar_material_desc). scenes/lib/ar.py packs the 64 ext_constants bytes from the field list in
src/host/material/grating_authored.h; the engine parses them with the struct built from the same
list. Each field is checked by sending a block in which only that field is bad and reading which
field the report entry names, so a swapped or shifted field cannot pass. A valid block compiles and
the store keeps it: it raises no entry at all."""
import ctypes
import os
import struct
import sys
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scenes", "lib"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ar  # noqa: E402
from test_material_api import TestMaterialApi as _Api  # noqa: E402

FIELDS = ["pitch_nm", "depth_nm", "coating_n", "coating_k", "mode", "profile", "m_min", "m_max"]
CD = dict(pitch_nm=1600.0, depth_nm=120.0, coating_n=1.0, coating_k=6.0, mode=0, profile=0, m_min=-2, m_max=2)
KEY0 = 300


def pack(**over):
    v = dict(CD, **over)
    return struct.pack("<ffffBBbb", *[v[n] for n in FIELDS]) + bytes(44)


class GratingMaterial(unittest.TestCase):
    # The scene, session and report plumbing of the material API test, not its cases.
    setUpClass = classmethod(_Api.setUpClass.__func__)
    setUp = _Api.setUp
    destroy_session = _Api.destroy_session
    progress = _Api.progress
    entries = _Api.entries

    def submit(self, key, raw):
        a, lib = self.abi, self.lib
        e = ctypes.c_void_p()
        self.assertEqual(lib.ar_scene_edit_begin(self.scene, ctypes.byref(e)), a.AR_OK)
        m = a.ar_material_desc()
        m.ext_kind = 1
        m.ext_constants = (ctypes.c_uint8 * 64)(*raw)
        self.assertEqual(lib.ar_edit_set_material(e, key, ctypes.byref(m)), a.AR_OK)
        gen = ctypes.c_uint64()
        self.assertEqual(lib.ar_edit_commit(e, ctypes.byref(gen)), a.AR_OK)
        end = time.perf_counter() + 20.0
        while self.progress().active_generation < gen.value:
            self.assertLess(time.perf_counter(), end, f"generation {gen.value} never became active")
            time.sleep(0.005)

    def refusal(self, key, raw):
        """The one entry the material raised, as (code, detail)."""
        self.submit(key, raw)
        got = self.entries(key)
        self.assertEqual(len(got), 1, f"entries of material {key}: {got}")
        self.assertEqual(got[0][0], self.abi.AR_REPORT_PROGRAM_REJECTED)
        return got[0][1]

    def test_layout_round_trip(self):
        # One field list: the header's, as ar.py reads it, is this list and the packed 64 bytes.
        self.assertEqual(ar.GRATING_FIELDS, FIELDS)
        self.assertEqual(ar.GRATING_FORMAT, "<ffffBBbb44x")
        self.assertEqual(struct.calcsize(ar.GRATING_FORMAT), 64)
        self.assertEqual(ar.grating_block(1600.0, 120.0, (1.0, 6.0)), pack())
        self.assertEqual(ar.grating_block(740.0, 90.0, (0.0, 0.0), -1, 3, "transmit", "csv"),
                         pack(pitch_nm=740.0, depth_nm=90.0, coating_n=0.0, coating_k=0.0, mode=1, profile=2,
                              m_min=-1, m_max=3))
        # A valid block parses and compiles, and SHADE_SURFACE_EXT renders it: the material raises no entry.
        self.submit(KEY0, pack())
        self.assertEqual(self.entries(KEY0), [])
        # Only the named field is bad, and the report names it: the C parse reads each at ar.py's offset.
        bad = [("pitch_nm", pack(pitch_nm=-1.0), "pitch_nm"),
               ("depth_nm", pack(depth_nm=0.0), "depth_nm"),
               ("coating_n", pack(coating_n=-1.0), "coating_n"),
               ("coating_k", pack(coating_k=-1.0), "coating_k"),
               ("index-matched coating", pack(coating_n=1.0, coating_k=0.0), "index-matched coating reflects nothing"),
               ("mode", pack(mode=2), "mode 2"),
               ("profile", pack(profile=3), "profile 3"),
               ("m_min", pack(m_min=-9), "m_min"),
               ("m_max", pack(m_max=9), "m_max"),
               ("reserved", pack()[:63] + b"\x01", "reserved byte 63"),
               ("reserved first", pack()[:20] + b"\x01" + pack()[21:], "reserved byte 20"),
               ("transmit", pack(mode=1), "transmission grating not built"),
               ("blazed", pack(profile=1), "blazed not built")]
        for i, (what, raw, text) in enumerate(bad):
            with self.subTest(what):
                self.assertIn(text, self.refusal(KEY0 + 1 + i, raw))
        # The pitch and the depth are not interchangeable: swapped, the groove is deeper than the
        # scalar range allows. The builder's entry is the report's GRATING_TABLE_LIMIT, once.
        key = KEY0 + 50
        self.submit(key, pack(pitch_nm=120.0, depth_nm=1600.0))
        got = self.entries(key)
        self.assertEqual(len(got), 1, got)
        self.assertEqual(got[0][0], self.abi.AR_REPORT_GRATING_TABLE_LIMIT)
        self.assertIn("scalar theory out of range", got[0][1])



if __name__ == "__main__":
    unittest.main()
