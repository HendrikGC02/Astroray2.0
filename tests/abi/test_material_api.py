# SPDX-License-Identifier: Apache-2.0
"""Materials through the generated binding on the CPU device (task 059; 0007 d8-d9, 0005 items 40
and 43, 0001 D2.11 and V15 part): graphs and materials submitted with ar_edit_set_graph and
ar_edit_set_material compile on a scene worker, and their report entries are read through a
session report. Unsupported nodes give one NODE_UNSUPPORTED per node type, a compile error one
PROGRAM_REJECTED; entries survive their own activation and an ar_edit_reset that re-submits them,
and go when the material is replaced or removed."""
import ctypes
import importlib.util
import os
import tempfile
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DLL = os.path.join(ROOT, "out", "bin", "astroray_core.dll")
ABI = os.path.join(ROOT, "out", "build", "gen", "_abi.py")
OPS = os.path.join(ROOT, "out", "build", "gen", "_ops.py")

# RecordSlot numbers (src/kernel/vm/ops/store_slot.h, AR_RECORD_SCALARS order).
BASE_METALNESS, SPECULAR_ROUGHNESS = 2, 4
GRAPH_BAD, GRAPH_GOOD, GRAPH_TWICE, MATERIAL, MATERIAL_2 = 100, 101, 102, 200, 201


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class Graph:
    """An ar_graph_desc built the way the addon builds one."""

    def __init__(self, abi, op_id):
        self.abi, self.op_id = abi, op_id
        self.nodes, self.links, self.f32, self.i32, self.strings = [], [], [], [], b""
        self.string_count = 0

    def add(self, op, f32=(), i32=(), string=None):
        n = self.abi.ar_graph_node()
        n.op = self.op_id[op]
        n.f32_first, n.f32_count = len(self.f32), len(f32)
        n.i32_first, n.i32_count = len(self.i32), len(i32)
        if string is not None:
            n.string_first, n.string_count = self.string_count, 1
            self.strings += string.encode() + b"\0"
            self.string_count += 1
        self.f32 += list(f32)
        self.i32 += list(i32)
        self.nodes.append(n)
        return len(self.nodes) - 1

    def store(self, value, slot):
        n = self.add("STORE_SLOT", i32=(slot,))
        self.links.append(self.abi.ar_graph_link(value, 0, n, 0))
        return n

    def desc(self):
        a = self.abi
        d = a.ar_graph_desc()
        d.node_count, d.link_count = len(self.nodes), len(self.links)
        d.f32_count, d.i32_count, d.string_bytes_count = len(self.f32), len(self.i32), len(self.strings)
        # The arrays live on the object until the next desc(): the call copies them in.
        self._keep = ((a.ar_graph_node * max(1, len(self.nodes)))(*self.nodes),
                      (a.ar_graph_link * max(1, len(self.links)))(*self.links),
                      (ctypes.c_float * max(1, len(self.f32)))(*self.f32),
                      (ctypes.c_int32 * max(1, len(self.i32)))(*self.i32),
                      (ctypes.c_uint8 * max(1, len(self.strings)))(*self.strings))
        d.nodes, d.links, d.params_f32, d.params_i32, d.string_bytes = self._keep
        return d


class TestMaterialApi(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for p in (DLL, ABI, OPS):
            if not os.path.exists(p):
                raise AssertionError(f"{p} missing: run tools/build.ps1 first")
        cls.abi = load_module("_abi_under_test", ABI)
        cls.op_id = load_module("_ops_under_test", OPS).OP_ID
        cls.lib = ctypes.CDLL(DLL)
        cls.abi.check(cls.lib)
        cls.abi.bind(cls.lib)

    def setUp(self):
        a, lib = self.abi, self.lib
        d = tempfile.TemporaryDirectory()
        self.addCleanup(d.cleanup)
        desc = a.ar_context_desc(a.AR_DEVICE_CPU, 0, os.path.join(d.name, "context.log").encode(), None)
        self.ctx = ctypes.c_void_p()
        self.assertEqual(lib.ar_context_create(ctypes.byref(desc), ctypes.byref(self.ctx)), a.AR_OK)
        self.addCleanup(lib.ar_context_destroy, self.ctx)
        sdesc = a.ar_scene_desc(None, 2, 0)
        self.scene = ctypes.c_void_p()
        self.assertEqual(lib.ar_scene_create(self.ctx, ctypes.byref(sdesc), ctypes.byref(self.scene)), a.AR_OK)
        self.addCleanup(lib.ar_scene_destroy, self.scene)
        vdesc = a.ar_session_desc(a.AR_SESSION_MODE_VIEWPORT, 8, 8, a.AR_DENOISER_NONE)
        self.session = ctypes.c_void_p()
        self.assertEqual(lib.ar_session_create(self.scene, ctypes.byref(vdesc), ctypes.byref(self.session)), a.AR_OK)
        self.addCleanup(self.destroy_session)
        # A running session latches each activated generation (its progress shows it).
        self.assertEqual(lib.ar_session_start(self.session), a.AR_OK)
        self.assertEqual(lib.ar_session_wait(self.session, 20000), a.AR_OK)

    def destroy_session(self):
        self.lib.ar_session_close(self.session)
        end = time.perf_counter() + 20.0
        while self.progress().state != self.abi.AR_STATE_CLOSED and time.perf_counter() < end:
            self.lib.ar_session_wait(self.session, 10)
        self.lib.ar_session_destroy(self.session)

    def progress(self):
        p = self.abi.ar_progress()
        self.assertEqual(self.lib.ar_session_get_progress(self.session, ctypes.byref(p)), self.abi.AR_OK)
        return p

    def graph(self):
        return Graph(self.abi, self.op_id)

    def bad_graph(self):
        g = self.graph()
        g.add("UNSUPPORTED", string="ShaderNodeBevel")
        g.add("UNSUPPORTED", string="ShaderNodeBevel")
        g.add("UNSUPPORTED", string="ShaderNodeAmbientOcclusion")
        g.store(g.add("CONST_F32", f32=(0.5,)), SPECULAR_ROUGHNESS)
        return g

    def good_graph(self):
        g = self.graph()
        g.store(g.add("CONST_F32", f32=(0.5,)), SPECULAR_ROUGHNESS)
        return g

    def edit(self, graphs=(), materials=(), reset=False, removes=()):
        """One edit: (key, Graph) and (key, surface graph key) pairs; returns its generation."""
        a, lib = self.abi, self.lib
        e = ctypes.c_void_p()
        self.assertEqual(lib.ar_scene_edit_begin(self.scene, ctypes.byref(e)), a.AR_OK)
        if reset:
            self.assertEqual(lib.ar_edit_reset(e), a.AR_OK)
        for key, g in graphs:
            self.assertEqual(lib.ar_edit_set_graph(e, key, a.AR_TARGET_SURFACE, ctypes.byref(g.desc())), a.AR_OK)
        for key, graph_key in materials:
            m = a.ar_material_desc()
            m.surface_graph = graph_key
            self.assertEqual(lib.ar_edit_set_material(e, key, ctypes.byref(m)), a.AR_OK)
        for kind, key in removes:
            self.assertEqual(lib.ar_edit_remove(e, kind, key), a.AR_OK)
        gen = ctypes.c_uint64()
        self.assertEqual(lib.ar_edit_commit(e, ctypes.byref(gen)), a.AR_OK)
        end = time.perf_counter() + 20.0
        while self.progress().active_generation < gen.value:
            self.assertLess(time.perf_counter(), end, f"generation {gen.value} never became active")
            time.sleep(0.005)
        return gen.value

    def entries(self, subject):
        """(code, detail) of the session report's entries about `subject`, sorted."""
        a, lib = self.abi, self.lib
        info = a.ar_report_info()
        self.assertEqual(lib.ar_session_report_info(self.session, ctypes.byref(info)), a.AR_OK)
        out = []
        for i in range(info.entry_count):
            e = a.ar_report_entry()
            self.assertEqual(lib.ar_session_report_entry(self.session, i, ctypes.byref(e)), a.AR_OK)
            if e.subject == subject:
                out.append((e.code, bytes(e.detail).split(b"\0")[0].decode()))
        return sorted(out)

    def test_two_node_types_two_entries(self):
        a = self.abi
        want = sorted([(a.AR_REPORT_NODE_UNSUPPORTED, "ShaderNodeAmbientOcclusion"),
                       (a.AR_REPORT_NODE_UNSUPPORTED, "ShaderNodeBevel")])
        self.edit(graphs=[(GRAPH_BAD, self.bad_graph())], materials=[(MATERIAL, GRAPH_BAD)])
        self.assertEqual(self.entries(MATERIAL), want, "after its own generation activated")
        # A reset clears every scene entry; the re-submitted material raises its own again.
        self.edit(graphs=[(GRAPH_BAD, self.bad_graph())], materials=[(MATERIAL, GRAPH_BAD)], reset=True)
        self.assertEqual(self.entries(MATERIAL), want, "after ar_edit_reset re-submitted it")
        print(f"entries of material {MATERIAL}: {self.entries(MATERIAL)}")
        # The edit that replaces the material removes them (0001 V15).
        self.edit(graphs=[(GRAPH_GOOD, self.good_graph())], materials=[(MATERIAL, GRAPH_GOOD)])
        self.assertEqual(self.entries(MATERIAL), [])

    def test_graph_change_recompiles_its_materials(self):
        a = self.abi
        self.edit(graphs=[(GRAPH_GOOD, self.good_graph())], materials=[(MATERIAL_2, GRAPH_GOOD)])
        self.assertEqual(self.entries(MATERIAL_2), [])
        self.edit(graphs=[(GRAPH_GOOD, self.bad_graph())])  # the material itself is not re-sent
        self.assertEqual(len(self.entries(MATERIAL_2)), 2)
        self.edit(removes=[(a.AR_KIND_MATERIAL, MATERIAL_2)])
        self.assertEqual(self.entries(MATERIAL_2), [])

    def test_compile_error_and_missing_graph_are_rejected(self):
        a = self.abi
        g = self.graph()
        g.store(g.add("CONST_F32", f32=(0.1,)), BASE_METALNESS)
        g.store(g.add("CONST_F32", f32=(0.2,)), BASE_METALNESS)
        self.edit(graphs=[(GRAPH_TWICE, g)], materials=[(MATERIAL, GRAPH_TWICE), (MATERIAL_2, 999)])
        got = self.entries(MATERIAL)
        self.assertEqual(len(got), 1)
        self.assertEqual(got[0][0], a.AR_REPORT_PROGRAM_REJECTED)
        self.assertIn("stored twice", got[0][1])
        self.assertEqual(self.entries(MATERIAL_2), [(a.AR_REPORT_PROGRAM_REJECTED, "surface graph 999 is not set")])

    def test_other_graph_targets_refuse(self):
        a, lib = self.abi, self.lib
        e = ctypes.c_void_p()
        self.assertEqual(lib.ar_scene_edit_begin(self.scene, ctypes.byref(e)), a.AR_OK)
        g = self.good_graph()
        self.assertEqual(lib.ar_edit_set_graph(e, GRAPH_GOOD, a.AR_TARGET_VOLUME, ctypes.byref(g.desc())),
                         a.AR_ERR_UNSUPPORTED)
        buf = (ctypes.c_uint8 * 256)()
        self.assertEqual(lib.ar_edit_last_error(e, 256, buf), a.AR_OK)
        self.assertIn("not implemented in this build", bytes(buf).split(b"\0")[0].decode())
        # Past the declared targets it is an enum range error (0007 d9).
        self.assertEqual(lib.ar_edit_set_graph(e, GRAPH_GOOD, a.AR_TARGET_WORLD + 1, ctypes.byref(g.desc())),
                         a.AR_ERR_INVALID_ARGUMENT)
        self.assertEqual(lib.ar_edit_last_error(e, 256, buf), a.AR_OK)
        self.assertIn("out of range", bytes(buf).split(b"\0")[0].decode())
        self.assertEqual(lib.ar_edit_abort(e), a.AR_OK)


if __name__ == "__main__":
    unittest.main()
