# SPDX-License-Identifier: Apache-2.0
"""Grammar checker and ABI hash for include/astroray/astroray.h.

Usage: abigen.py --check <header> | --hash <header> | --emit <header> <outdir>
       abigen.py --check-hooks <header> <capi dir>
Errors print as `file:line: message`; exit status 1 when any was found.
--emit writes astroray_layout.c, abi_hash.h, _abi.py and the recorder files (recorder_hooks.h,
recorder.cpp, replayer.h, replayer.cpp, arrec.py) into <outdir>.
--check-hooks fails naming any implemented non-query entry point without its AR_RECORD line.
Ref: docs/decisions/0001-product-shape.md D1.3 (_abi.py), D1.4 and D2.10 (recording rule), D2.7 (hash, layout), D2.10 (grammar).
"""
import hashlib
import os
import re
import sys

# The one fixed prelude block; accepted only byte-identical (task 012).
PRELUDE = """#ifdef _WIN32
#ifdef AR_BUILDING_CORE
#define AR_API __declspec(dllexport)
#else
#define AR_API __declspec(dllimport)
#endif
#else
#define AR_API
#endif
#define AR_QUERY
#define AR_IN
#define AR_OUT
#define AR_IN_ARRAY(n, T)
#define AR_OUT_ARRAY(n, T)
#define AR_STAGED(n, T)
"""

MAX_FIELDS = 128  # ar_struct_layout.field_offset capacity
INTS = {"uint32_t", "int32_t", "uint64_t"}
SCALARS = INTS | {"float", "double"}
ANNOT = re.compile(r"\b(AR_IN_ARRAY|AR_OUT_ARRAY|AR_STAGED|AR_IN|AR_OUT)\b(?:\s*\(([^)]*)\))?")
OBJ_NAME = re.compile(r"^ar_[a-z0-9]+(_[a-z0-9]+)+$")
TYPE_NAME = re.compile(r"^ar_[a-z0-9]+(_[a-z0-9]+)*$")
CONST_NAME = re.compile(r"^AR_[A-Z0-9]+(_[A-Z0-9]+)*$")
IDENT = re.compile(r"^[a-z][a-z0-9_]*$")
BANNED = {"bool": "bool", "_Bool": "bool", "enum": "enum types", "union": "unions",
          "char": "char other than const char*"}


def strip_comments(text):
    """Blank comments, keeping newlines so line numbers survive."""
    def blank(m):
        return re.sub(r"[^\n]", " ", m.group(0))
    return re.sub(r"/\*.*?\*/|//[^\n]*", blank, text, flags=re.S)


def normalise(text):
    # Comments removed, whitespace runs collapsed to one space (0001 D2.7).
    return re.sub(r"\s+", " ", strip_comments(text)).strip()


def abi_hash(text):
    digest = hashlib.sha256(normalise(text).encode("utf-8")).digest()
    return int.from_bytes(digest[:8], "big")


def ann_of(annots):
    """(annotation, count name, element type) of a pointer's one annotation."""
    if len(annots) != 1:
        return (None, None, None)
    kind, args = annots[0].group(1), annots[0].group(2)
    if args is None:
        return (kind, None, None)
    parts = [x.strip() for x in args.split(",")]
    return (kind, parts[0], parts[1])


class Checker:
    def __init__(self, path, text):
        self.path = path
        self.errors = []
        self.text = text.replace("\r\n", "\n")
        self.handles = set()
        self.structs = {}  # name -> {field name: type}
        # What --emit needs, in header order: (name, rows) with rows of
        # (base type, pointer depth, const, field name, array length or None).
        self.struct_rows = []
        self.consts = []  # (name, literal)
        self.protos = []  # (name, [(base type, pointer depth, const, parameter name)])
        self.queries = set()  # AR_QUERY function names
        self.field_ann = {}  # (struct, field) -> (annotation, count name or None, element type or None)
        self.param_ann = {}  # (function, parameter) -> the same triple
        self.cur = None

    def err(self, line, msg):
        self.errors.append(f"{self.path}:{line}: {msg}")

    def run(self):
        text = strip_comments(self.text)
        idx = text.find(PRELUDE)
        if idx < 0:
            self.err(1, "fixed prelude block missing or not byte-identical")
        else:
            blank = re.sub(r"[^\n]", " ", PRELUDE)
            text = text[:idx] + blank + text[idx + len(PRELUDE):]
        self.top_level(text)
        return self.errors

    def top_level(self, text):
        buf, start, depth = "", 0, 0
        for no, raw in enumerate(text.split("\n"), 1):
            line = raw.strip()
            if not buf and line.startswith("#"):
                self.directive(no, line)
                continue
            if not buf and line in ('extern "C" {', "}"):
                continue  # the __cplusplus guard; its #ifdef/#endif are checked above
            if not line:
                continue
            if not buf:
                start = no
            buf += " " + line
            depth += line.count("{") - line.count("}")
            if depth == 0 and buf.rstrip().endswith(";"):
                self.statement(start, " ".join(buf.split()))
                buf = ""
        if buf.strip():
            self.err(start, "unterminated declaration")

    def directive(self, no, line):
        if line in ("#ifndef ASTRORAY_H", "#define ASTRORAY_H", "#include <stdint.h>",
                    "#ifdef __cplusplus", "#endif"):
            return
        m = re.match(r"#define\s+(\S+)\s+(\S+)$", line)
        if m:
            name, val = m.groups()
            if not CONST_NAME.match(name):
                self.err(no, f"constant '{name}' must be named AR_<NAME>")
            elif not re.match(r"^(0x[0-9a-fA-F]+|[0-9]+)$", val):
                self.err(no, f"'{name}' value must be an integer literal")
            else:
                self.consts.append((name, val))
            return
        self.err(no, f"directive not allowed in the grammar: {line}")

    def statement(self, no, s):
        for w in ("enum", "union"):
            if re.search(rf"\b{w}\b", s):
                self.err(no, f"{w} types not allowed: {s}")
                return
        if s.startswith("typedef struct") and "{" in s:
            return self.struct(no, s)
        m = re.match(r"typedef struct (\w+)\* (\w+);$", s)
        if m:
            tag, name = m.groups()
            if not TYPE_NAME.match(name) or tag != name + "_s":
                self.err(no, f"handle must be 'typedef struct ar_<name>_s* ar_<name>;': {s}")
            self.handles.add(name)
            return
        if re.match(r"typedef (uint64_t ar_key|uint32_t ar_status);$", s):
            return
        if s.startswith("typedef"):
            self.err(no, f"typedef not allowed in the grammar: {s}")
            return
        if s.startswith("AR_API"):
            return self.prototype(no, s)
        self.err(no, f"top-level form not allowed in the grammar: {s}")

    def known(self, t):
        return t in SCALARS or t in self.handles or t in self.structs or t == "ar_key"

    def banned(self, no, s):
        for w, what in BANNED.items():
            if re.search(rf"\b{w}\b", s) and not (w == "char" and "const char*" in s):
                self.err(no, f"{what} not allowed: {s}")
                return True
        if re.search(r"\(\s*\*", s):
            self.err(no, f"function pointers not allowed: {s}")
            return True
        return False

    def struct(self, no, s):
        m = re.match(r"typedef struct (\w+) \{(.*)\} (\w+);$", s)
        if not m or m.group(1) != m.group(3):
            self.err(no, f"struct must be 'typedef struct ar_<name> {{...}} ar_<name>;': {s}")
            return
        name, body = m.group(1), m.group(2)
        if not TYPE_NAME.match(name):
            self.err(no, f"struct '{name}' must be named ar_<name>")
        fields = {}
        self.structs[name] = fields
        self.cur = name
        self.rows = []
        self.struct_rows.append((name, self.rows))
        decls = [d.strip() for d in body.split(";") if d.strip()]
        if len(decls) > MAX_FIELDS:
            self.err(no, f"struct '{name}' has {len(decls)} fields, more than {MAX_FIELDS}")
        for d in decls:
            self.field(no, fields, d)
        for d in decls:  # counts resolve once every field is known
            for a in ANNOT.finditer(d):
                if a.group(2) is not None:
                    n = a.group(2).split(",")[0].strip()
                    if fields.get(n) not in INTS:
                        self.err(no, f"array count '{n}' names no integer field of {name}")

    def field(self, no, fields, d):
        if ":" in d:
            self.err(no, f"bitfields not allowed: {d}")
            return
        if self.banned(no, d):
            return
        annots = list(ANNOT.finditer(d))
        rest = ANNOT.sub(" ", d).strip()
        if "*" in rest:
            toks = rest.replace("*", " * ").split()
            if len(toks) < 3 or toks[-2] != "*":
                self.err(no, f"cannot parse pointer field: {d}")
                return
            self.pointer(no, d, annots, toks, None, "struct")
            self.field_ann[(self.cur, toks[-1])] = ann_of(annots)
            const = toks[0] == "const"
            self.rows.append((toks[1] if const else toks[0], toks.count("*"), const, toks[-1], None))
            fields[toks[-1]] = "ptr"
            return
        if annots:
            self.err(no, f"annotation on a non-pointer field: {d}")
        m = re.match(r"^(\w+) (\w+)(\[(\d+)\])?$", rest)
        if not m:
            self.err(no, f"cannot parse field: {d}")
            return
        t, n, arr = m.group(1), m.group(2), m.group(3)
        if t == "uint8_t":
            if not arr:
                self.err(no, f"uint8_t only as fixed arrays: {d}")
        elif not (t in SCALARS or t == "ar_key") and t not in self.structs:
            self.err(no, f"unknown or disallowed field type '{t}': {d}")
        if not IDENT.match(n):
            self.err(no, f"field '{n}' must be lower_snake_case")
        fields[n] = "array" if arr else t
        self.rows.append((t, 0, False, n, int(m.group(4)) if arr else None))

    def check_annot(self, no, annots, d):
        if len(annots) == 0:
            self.err(no, f"unannotated pointer: {d}")
            return False
        if len(annots) > 1:
            self.err(no, f"pointer needs exactly one annotation: {d}")
            return False
        return True

    def prototype(self, no, s):
        m = re.match(r"AR_API\s+(AR_QUERY\s+)?(\S+)\s+(\w+)\s*\((.*)\);$", s)
        if not m:
            self.err(no, f"cannot parse prototype: {s}")
            return
        q, ret, name, params = m.groups()
        if q:
            self.queries.add(name)
        if ret != "ar_status":
            self.err(no, f"function '{name}' must return ar_status, not '{ret}'")
        if not OBJ_NAME.match(name):
            self.err(no, f"function '{name}' must be named ar_<object>_<verb>")
        plist, depth, cur = [], 0, ""
        for ch in params:
            depth += ch == "("
            depth -= ch == ")"
            if ch == "," and depth == 0:
                plist.append(cur.strip())
                cur = ""
            else:
                cur += ch
        if cur.strip() and cur.strip() != "void":
            plist.append(cur.strip())
        ints = {}
        for p in plist:  # integer parameters first, for count references
            q = ANNOT.sub(" ", p).split()
            if len(q) == 2 and q[0] in INTS and "*" not in p:
                ints[q[1]] = q[0]
        for p in plist:
            self.param(no, name, p, ints)
        rows = []
        for p in plist:
            toks = ANNOT.sub(" ", p).replace("*", " * ").split()
            const = toks[0] == "const"
            rows.append((toks[1] if const else toks[0], toks.count("*"), const, toks[-1]))
            if "*" in toks:
                self.param_ann[(name, toks[-1])] = ann_of(list(ANNOT.finditer(p)))
        self.protos.append((name, rows))

    def pointer(self, no, p, annots, toks, ints, where):
        """Pointer forms shared by parameters and struct fields (0001 D2.10).
        ints is None for fields: their counts are resolved once the struct is complete."""
        if not self.check_annot(no, annots, p):
            return
        kind, args = annots[0].group(1), annots[0].group(2)
        stars = toks.count("*")
        const = toks[0] == "const"
        base = toks[1] if const else toks[0]
        if kind in ("AR_IN_ARRAY", "AR_OUT_ARRAY", "AR_STAGED"):
            parts = [x.strip() for x in (args or "").split(",")]
            if len(parts) != 2:
                self.err(no, f"{kind} needs (n, T): {p}")
                return
            if ints is not None and parts[0] not in ints:
                self.err(no, f"array count '{parts[0]}' names no integer parameter of {where}")
            if parts[1] != base:
                self.err(no, f"{kind} element type '{parts[1]}' differs from '{base}': {p}")
            if kind == "AR_STAGED" and stars != 2:
                self.err(no, f"AR_STAGED needs T**: {p}")
            if kind == "AR_IN_ARRAY" and not const:
                self.err(no, f"AR_IN_ARRAY needs const T*: {p}")
            if kind == "AR_OUT_ARRAY" and const:
                self.err(no, f"AR_OUT_ARRAY needs T*: {p}")
        else:
            if args is not None:
                self.err(no, f"{kind} takes no arguments: {p}")
            if kind == "AR_IN" and not const:
                self.err(no, f"AR_IN needs const T*: {p}")
            if kind == "AR_OUT" and const:
                self.err(no, f"AR_OUT needs T*: {p}")
        if base not in ("uint8_t", "char") and not self.known(base):
            self.err(no, f"unknown type '{base}': {p}")

    def param(self, no, fn, p, ints):
        if self.banned(no, p):
            return
        annots = list(ANNOT.finditer(p))
        toks = ANNOT.sub(" ", p).replace("*", " * ").split()
        if "*" in toks:
            self.pointer(no, p, annots, toks, ints, fn)
            return
        if annots:
            self.err(no, f"annotation on a non-pointer parameter: {p}")
        if len(toks) != 2:
            self.err(no, f"cannot parse parameter: {p}")
        elif toks[0] == "uint8_t":
            self.err(no, f"uint8_t only as fixed arrays: {p}")
        elif not self.known(toks[0]):
            self.err(no, f"unknown type '{toks[0]}': {p}")


SCALAR_CTYPES = {"uint32_t": "c_uint32", "int32_t": "c_int32", "uint64_t": "c_uint64",
                 "float": "c_float", "double": "c_double", "uint8_t": "c_uint8",
                 "ar_key": "c_uint64", "ar_status": "c_uint32"}
IDENTITY_FUNCTIONS = ("ar_abi_hash", "ar_abi_layout_count", "ar_abi_layout", "ar_build_info")


def ctype_of(chk, base, stars, const):
    """ctypes expression for one declared type (handles are opaque pointers)."""
    if base == "char" and stars == 1 and const:
        return "ctypes.c_char_p"
    if base in SCALAR_CTYPES:
        t = "ctypes." + SCALAR_CTYPES[base]
    elif base in chk.handles:
        t = "ctypes.c_void_p"
    else:
        t = base
    for _ in range(stars):
        t = f"ctypes.POINTER({t})"
    return t


LAYOUT_C = """/* SPDX-License-Identifier: Apache-2.0 */
/* Generated by tools/abigen.py from include/astroray/astroray.h; never edited.
   Compiled by MSVC so sizeof and offsetof are the compiler's own numbers
   (0001 D2.7). */
#include <stddef.h>
#include "astroray/astroray.h"

#define AR_ALIGNOF(T) _Alignof(T)
#define AR_ROW(T, N, ...) { #T, sizeof(T), AR_ALIGNOF(T), N, {__VA_ARGS__} }

const ar_struct_layout ar_layout_table[] = {
%s};
const uint32_t ar_layout_table_count = %d;
"""

PY_HEAD = """# SPDX-License-Identifier: Apache-2.0
# Generated by tools/abigen.py from include/astroray/astroray.h; never edited
# and never committed (0001 D1.3).
import ctypes

ABI_HASH = 0x%016x


class AbiMismatch(Exception):
    pass

"""

PY_TAIL = '''

IDENTITY = %s


def bind(lib, names=None):
    """Set argtypes and restype; names limits it to those functions."""
    for name in (PROTOTYPES if names is None else names):
        fn = getattr(lib, name)
        fn.restype = ctypes.c_uint32
        fn.argtypes = PROTOTYPES[name]


def _call(fn, *args):
    st = fn(*args)
    if st != 0:
        raise AbiMismatch(f"{fn.__name__} returned status {st}")


def check(lib):
    """Refuse a library whose header hash or struct layout differs from this binding.
    The library's table comes from MSVC's sizeof/offsetof, the binding's own
    numbers from ctypes, so two independent computations are compared."""
    bind(lib, IDENTITY)
    h = ctypes.c_uint64()
    _call(lib.ar_abi_hash, ctypes.byref(h))
    if h.value != ABI_HASH:
        raise AbiMismatch(f"ABI hash mismatch: binding 0x{ABI_HASH:016x}, library 0x{h.value:016x}")
    n = ctypes.c_uint32()
    _call(lib.ar_abi_layout_count, ctypes.byref(n))
    table = {}
    for i in range(n.value):
        row = ar_struct_layout()
        _call(lib.ar_abi_layout, i, ctypes.byref(row))
        table[bytes(row.name).split(b"\\0")[0].decode()] = row
    for cls in STRUCTS:
        name = cls.__name__
        row = table.get(name)
        if row is None:
            raise AbiMismatch(f"{name}: missing from the library's layout table")
        names = [f[0] for f in cls._fields_]
        if row.field_count != len(names):
            raise AbiMismatch(f"{name}: library has {row.field_count} fields, binding {len(names)}")
        # Fields before size, so a swapped pair is named by field.
        for j, field in enumerate(names):
            ours = getattr(cls, field).offset
            if row.field_offset[j] != ours:
                raise AbiMismatch(f"{name}.{field}: offset {ours} in the binding, "
                                  f"{row.field_offset[j]} in the library")
        if row.size != ctypes.sizeof(cls):
            raise AbiMismatch(f"{name}: size {ctypes.sizeof(cls)} in the binding, {row.size} in the library")
        if row.align != ctypes.alignment(cls):
            raise AbiMismatch(f"{name}: alignment {ctypes.alignment(cls)} in the binding, "
                              f"{row.align} in the library")
    if len(table) != len(STRUCTS):
        raise AbiMismatch(f"library has {len(table)} structs, binding {len(STRUCTS)}")
'''


def emit(path, outdir):
    with open(path, encoding="utf-8", newline="") as f:
        text = f.read().replace("\r\n", "\n")
    chk = Checker(path, text)
    errors = chk.run()
    if errors:
        for e in errors:
            print(e)
        return 1
    os.makedirs(outdir, exist_ok=True)
    h = abi_hash(text)

    rows = ""
    for name, fields in chk.struct_rows:
        offs = ", ".join(f"offsetof({name}, {f[3]})" for f in fields)
        rows += f"    AR_ROW({name}, {len(fields)}, {offs}),\n"
    files = {
        "astroray_layout.c": LAYOUT_C % (rows, len(chk.struct_rows)),
        "abi_hash.h": "/* Generated by tools/abigen.py; never edited. */\n"
                      f"#define AR_ABI_HASH_VALUE 0x{h:016x}ull\n",
    }

    py = PY_HEAD % h
    for name, value in chk.consts:
        py += f"{name} = {value}\n"
    for name, fields in chk.struct_rows:
        py += f"\n\nclass {name}(ctypes.Structure):\n    _fields_ = [\n"
        for base, stars, const, fname, arr in fields:
            t = ctype_of(chk, base, stars, const)
            if arr is not None:
                t = f"{t} * {arr}"
            py += f'        ("{fname}", {t}),\n'
        py += "    ]\n"
    py += "\n\nSTRUCTS = (" + ", ".join(n for n, _ in chk.struct_rows) + ",)\n\nPROTOTYPES = {\n"
    for name, params in chk.protos:
        args = ", ".join(ctype_of(chk, b, st, c) for b, st, c, _ in params)
        py += f'    "{name}": [{args}],\n'
    py += "}\n"
    py += PY_TAIL % repr(IDENTITY_FUNCTIONS)
    files["_abi.py"] = py

    for name, content in files.items():
        with open(os.path.join(outdir, name), "w", encoding="utf-8", newline="\n") as f:
            f.write(content)
    return 0


def main(argv):
    if len(argv) == 4 and argv[1] == "--emit":
        return emit(argv[2], argv[3])
    if len(argv) != 3 or argv[1] not in ("--check", "--hash"):
        print("usage: abigen.py --check|--hash <header> | --emit <header> <outdir>", file=sys.stderr)
        return 2
    path = argv[2]
    with open(path, encoding="utf-8", newline="") as f:
        text = f.read().replace("\r\n", "\n")
    if argv[1] == "--hash":
        print(f"{abi_hash(text):016x}")
        return 0
    errors = Checker(path, text).run()
    for e in errors:
        print(e)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
