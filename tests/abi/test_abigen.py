# SPDX-License-Identifier: Apache-2.0
"""Tests for tools/abigen.py (0001 V1 generator part, V2)."""
import os
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
ABIGEN = os.path.join(ROOT, "tools", "abigen.py")
HEADER = os.path.join(ROOT, "include", "astroray", "astroray.h")
FIXTURES = os.path.join(HERE, "bad_headers")

sys.path.insert(0, os.path.join(ROOT, "tools"))
import abigen  # noqa: E402


def check(path):
    return subprocess.run([sys.executable, "-I", ABIGEN, "--check", path],
                          capture_output=True, text=True)


# fixture file -> substring its error line must contain
EXPECTED = {
    "unannotated_pointer": "unannotated pointer",
    "two_annotations": "exactly one annotation",
    "bad_array_count": "names no integer parameter",
    "bad_struct_array_count": "names no integer field",
    "non_status_return": "must return ar_status",
    "bare_uint8": "uint8_t only as fixed arrays",
    "bad_function_name": "must be named ar_<object>_<verb>",
    "bad_constant_name": "must be named AR_<NAME>",
    "bool_field": "bool not allowed",
    "enum_type": "enum types not allowed",
    "union_type": "union types not allowed",
    "bitfield": "bitfields not allowed",
    "function_pointer": "function pointers not allowed",
    "plain_char": "char other than const char*",
    "too_many_fields": "more than 128",
    "field_unknown_pointee": "unknown type 'int'",
    "field_in_not_const": "AR_IN needs const T*",
    "field_array_type_mismatch": "element type 'uint32_t' differs from 'float'",
    "bad_directive": "directive not allowed",
    "non_integer_define": "integer literal",
    "prelude_modified": "prelude",
}


class TestFixtures(unittest.TestCase):
    def run_fixture(self, name):
        path = os.path.join(FIXTURES, name + ".h")
        r = check(path)
        self.assertEqual(r.returncode, 1, r.stdout)
        first = r.stdout.splitlines()[0]
        self.assertIn(EXPECTED[name], first)
        # file:line: message
        rest = first[len(path) + 1:]
        self.assertTrue(rest.split(":")[0].isdigit(), first)

    def test_every_fixture_has_a_case(self):
        files = {f[:-2] for f in os.listdir(FIXTURES) if f.endswith(".h")}
        self.assertEqual(files, set(EXPECTED))

    def test_real_header_passes(self):
        r = check(HEADER)
        self.assertEqual((r.returncode, r.stdout), (0, ""))


def _add(name):
    setattr(TestFixtures, "test_" + name, lambda self, n=name: self.run_fixture(n))


for _name in EXPECTED:
    _add(_name)


class TestHash(unittest.TestCase):
    def test_hash_is_16_hex_digits(self):
        r = subprocess.run([sys.executable, "-I", ABIGEN, "--hash", HEADER],
                           capture_output=True, text=True)
        self.assertRegex(r.stdout.strip(), r"^[0-9a-f]{16}$")

    def test_comments_and_whitespace_do_not_change_hash(self):
        with open(HEADER, encoding="utf-8") as f:
            text = f.read().replace("\r\n", "\n")
        edited = text.replace("/* Status codes. */", "/* other   words */").replace(
            "ar_status ar_device_count(", "ar_status   ar_device_count(")
        self.assertNotEqual(edited, text)
        self.assertEqual(abigen.abi_hash(edited), abigen.abi_hash(text))

    def test_signature_change_changes_hash(self):
        with open(HEADER, encoding="utf-8") as f:
            text = f.read().replace("\r\n", "\n")
        old = "ar_abi_layout(uint32_t index,"
        self.assertIn(old, text)
        edited = text.replace(old, "ar_abi_layout(uint64_t index,")
        self.assertNotEqual(abigen.abi_hash(edited), abigen.abi_hash(text))


if __name__ == "__main__":
    unittest.main()
