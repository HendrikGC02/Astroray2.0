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
    emit_recorder_files(chk, h, outdir)
    return 0


# ---------------------------------------------------------------------------
# Recorder, replayer and .arrec decoder (0001 D1.4, D2.10 recording rule).
# Wire format (little endian, native scalar widths): header = "ARREC001", u64 ABI hash,
# the ar_build_record bytes; then records {u32 function index (declaration order over all
# prototypes), u32 flags (0), u64 payload bytes, payload}. Payload: the parameters in order.
# A handle is a u64 id (0 null) assigned in creation order, counted per recording by record
# position; AR_OUT handles and AR_OUT values are not stored. An AR_IN pointer is u8 present then
# the struct; an AR_IN string is u32 length (0xFFFFFFFF null) and bytes; an AR_IN_ARRAY is u8
# present then count elements. In a struct, plain fields come first (declaration order), pointer
# fields after, so a count is always read before its array. A staged array is captured at
# ar_edit_item_commit: u32 kind (index of the begin function, 0xFFFFFFFF none), then per staged
# field u64 count and the bytes.
# ---------------------------------------------------------------------------
LIKE = SCALARS | {"uint8_t", "ar_key"}
DESTROYS = ("ar_context_destroy", "ar_scene_destroy", "ar_session_destroy")
SIZES = {"uint8_t": 1, "uint32_t": 4, "int32_t": 4, "uint64_t": 8, "ar_key": 8, "float": 4, "double": 8}


class Model:
    """The parsed header seen as the recorder needs it."""

    def __init__(self, chk):
        self.chk = chk
        self.structs = dict(chk.struct_rows)
        self.funcs = [(i, n, rows) for i, (n, rows) in enumerate(chk.protos)]
        self.rec = [f for f in self.funcs if f[1] not in chk.queries]
        self.used = []
        for _, n, rows in self.rec:
            for r in rows:
                k = self.classify(n, r)
                if k[0] == "struct" or (k[0] == "array" and k[1] not in LIKE):
                    self.need(k[1])
        # Count parameters must precede their arrays (the decoder reads in order).
        for _, n, rows in self.rec:
            seen = set()
            for r in rows:
                k = self.classify(n, r)
                if k[0] == "array" and k[2] not in seen:
                    raise SystemExit(f"{n}: count '{k[2]}' must be declared before {r[3]}")
                seen.add(r[3])

    def need(self, name):
        if name in self.used:
            return
        for f in self.ordered(name):
            if f[1] == "struct" or (f[1] == "array" and f[2] not in LIKE):
                self.need(f[2])
        self.used.append(name)  # dependencies first

    def classify(self, fn, row):
        base, stars, _, pname = row
        if stars == 0:
            return ("handle", base) if base in self.chk.handles else ("scalar", base)
        kind, count, _ = self.chk.param_ann[(fn, pname)]
        if kind == "AR_IN":
            return ("string",) if base == "char" else ("struct", base)
        if kind == "AR_IN_ARRAY":
            return ("array", base, count)
        if kind == "AR_OUT" and base in self.chk.handles:
            return ("hout", base)
        return ("out", base) if kind == "AR_OUT" else ("skip", base)

    def ordered(self, sname):
        """Wire order of a struct: (field, kind, type, extra); staged fields excluded."""
        plain, ptrs = [], []
        for base, stars, _, f, arr in self.structs[sname]:
            if stars == 0:
                if arr is not None:
                    if base not in LIKE:
                        raise SystemExit(f"{sname}.{f}: fixed arrays of structs are not recordable")
                    plain.append((f, "fixed", base, arr))
                elif base in LIKE:
                    plain.append((f, "scalar", base, None))
                else:
                    plain.append((f, "struct", base, None))
                continue
            kind, count, _ = self.chk.field_ann[(sname, f)]
            if kind == "AR_IN" and base == "char":
                ptrs.append((f, "string", None, None))
            elif kind == "AR_IN_ARRAY":
                ptrs.append((f, "array", base, count))
            elif kind != "AR_STAGED":
                raise SystemExit(f"{sname}.{f}: {kind} pointer fields are not recordable")
        return plain + ptrs

    def staged(self, sname):
        return [(f, self.chk.field_ann[(sname, f)][1], base) for base, stars, _, f, _ in self.structs[sname]
                if stars == 2]

    def begin_info(self, fn, rows):
        """(out parameter, struct) when fn stages arrays."""
        for r in rows:
            k = self.classify(fn, r)
            if k[0] == "out" and k[1] in self.structs and self.staged(k[1]):
                return r[3], k[1]
        return None

    def key_expr(self, fn, rows):
        for r in rows:
            if self.classify(fn, r) == ("scalar", "ar_key"):
                return r[3]
        for r in rows:
            k = self.classify(fn, r)
            if k[0] == "struct" and any(f[0] == "key" for f in self.ordered(k[1])):
                return f"{r[3]}->key"
        raise SystemExit(f"{fn}: stages arrays but has no key to match its item commit")


def cparams(rows):
    return ", ".join(f"{'const ' if c else ''}{b}{'*' * s} {n}" for b, s, c, n in rows)


def cargs(rows):
    return ", ".join(r[3] for r in rows)


HOOKS_HEAD = """/* Generated by tools/abigen.py from include/astroray/astroray.h; never edited. */
#pragma once
#include <stdint.h>

#include <string>

#include "astroray/astroray.h"

struct ar_recorder;
namespace astroray {
struct ContextState;
}

/* The recorder of a context (null when recording is off). The overload for a handle type is
   defined by the translation unit that owns the handle struct: context.cpp (context and the
   state), scene_api.cpp (scene, edit); the session one comes with the session task. */
ar_recorder* ar_rec_of_state(astroray::ContextState& state);
%s
ar_recorder* ar_rec_open(const char* path, const ar_build_record& build, std::string* err);
void ar_rec_close(ar_recorder* r, std::string* err);
void ar_rec_bind(ar_recorder* r, uint64_t id, const void* handle);

/* Pre-call hooks: one per non-query function, returning the handle id reserved for an AR_OUT
   handle (0 otherwise). Post hooks run after a successful call that fills staged buffers. */
%s
/* One branch per call when recording is off. AR_RECORD goes first in the entry point;
   AR_RECORD_BIND after a successful AR_OUT handle write; AR_RECORD_POST after a successful
   staging begin. `rec` is the recorder expression, the other arguments the call's own. */
#define AR_RECORD(fn, rec, ...)                                \\
  ar_recorder* const ar_rec_ = (rec);                          \\
  uint64_t ar_rec_id_ = 0;                                     \\
  if (ar_rec_) ar_rec_id_ = ar_rec_##fn(ar_rec_, __VA_ARGS__); \\
  (void)ar_rec_id_
#define AR_RECORD_BIND(handle)                                             \\
  do {                                                                     \\
    if (ar_rec_ && ar_rec_id_) ar_rec_bind(ar_rec_, ar_rec_id_, (handle)); \\
  } while (0)
#define AR_RECORD_POST(fn, ...)                          \\
  do {                                                   \\
    if (ar_rec_) ar_rec_post_##fn(ar_rec_, __VA_ARGS__); \\
  } while (0)
"""


def emit_hooks_h(m):
    ofs = "".join(f"ar_recorder* ar_rec_of({h} h);\n" for h in sorted(m.chk.handles))
    decl = ""
    for _, n, rows in m.rec:
        decl += f"uint64_t ar_rec_{n}(ar_recorder* rec_, {cparams(rows)});\n"
        if m.begin_info(n, rows):
            decl += f"void ar_rec_post_{n}(ar_recorder* rec_, {cparams(rows)});\n"
    return HOOKS_HEAD % (ofs, decl)


def enc_stmt(v, kind, t, extra, ind):
    if kind == "scalar":
        return f"{ind}w.scalar({v});\n"
    if kind == "fixed":
        return f"{ind}w.bytes({v}, sizeof {v});\n"
    if kind == "struct":
        return f"{ind}enc_{t}(w, {v});\n"
    if kind == "string":
        return f"{ind}w.cstr({v});\n"
    if t in LIKE:  # array of scalars
        return f"{ind}w.array({v}, (size_t){extra});\n"
    return f"{ind}w.array_of({v}, (size_t){extra}, [&](const {t}& e) {{ enc_{t}(w, e); }});\n"


def dec_stmt(v, kind, t, extra, ind):
    if kind == "scalar":
        return f"{ind}{v} = r.scalar<{t}>();\n"
    if kind == "fixed":
        return f"{ind}r.bytes({v}, sizeof {v});\n"
    if kind == "struct":
        return f"{ind}dec_{t}(r, a, {v});\n"
    if kind == "string":
        return f"{ind}{v} = r.cstr(a);\n"
    if t in LIKE:
        return f"{ind}{v} = r.array<{t}>(a, (size_t){extra});\n"
    return f"{ind}{v} = r.array_of<{t}>(a, (size_t){extra}, [&]({t}& e) {{ dec_{t}(r, a, e); }});\n"


REC_HEAD = """// Generated by tools/abigen.py from include/astroray/astroray.h; never edited.
// The recorder of one context: appends every non-query call under its own lock, in arrival order.
#include <stdio.h>
#include <string.h>

#include <filesystem>
#include <map>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "abi_hash.h"
#include "recorder_hooks.h"

namespace {

constexpr char kMagic[8] = {'A', 'R', 'R', 'E', 'C', '0', '0', '1'};
constexpr size_t kFlushBytes = size_t(64) << 20;
constexpr uint64_t kUnknown = ~uint64_t(0);

struct Writer {
  std::vector<uint8_t>& b;
  void bytes(const void* p, size_t n) {
    if (!n) return;
    size_t o = b.size();
    b.resize(o + n);
    memcpy(b.data() + o, p, n);
  }
  template <class T>
  void scalar(T v) { bytes(&v, sizeof v); }
  void u8(uint8_t v) { bytes(&v, 1); }
  void cstr(const char* s) {
    if (!s) return scalar<uint32_t>(0xFFFFFFFFu);
    size_t n = strlen(s);
    scalar<uint32_t>(static_cast<uint32_t>(n));
    bytes(s, n);
  }
  template <class T>
  void array(const T* p, size_t n) {
    u8(p != nullptr);
    if (p) bytes(p, n * sizeof(T));
  }
  template <class T, class F>
  void array_of(const T* p, size_t n, F&& f) {
    u8(p != nullptr);
    if (p)
      for (size_t i = 0; i < n; ++i) f(p[i]);
  }
};

struct StagedField {
  uint64_t count;
  void** slot;
  uint32_t esize;
};
struct StagedItem {
  uint32_t kind;
  std::vector<StagedField> fields;
};

}  // namespace

struct ar_recorder {
  std::mutex mu;
  FILE* f = nullptr;
  std::vector<uint8_t> buf;      // not yet written
  std::vector<uint8_t> payload;  // the record being built
  uint64_t creates = 0;
  std::unordered_map<const void*, uint64_t> ids;
  std::map<std::pair<uint64_t, uint64_t>, StagedItem> staged;  // (edit id, key)
  std::string error;

  uint64_t id_of(const void* h) const {
    if (!h) return 0;
    auto it = ids.find(h);
    return it == ids.end() ? kUnknown : it->second;
  }
  void flush() {
    if (!buf.empty() && f && error.empty() && fwrite(buf.data(), 1, buf.size(), f) != buf.size()) error = "write failed";
    buf.clear();
  }
  uint64_t append(uint32_t fn, bool creating) {
    uint32_t flags = 0;
    uint64_t n = payload.size();
    Writer w{buf};
    w.scalar(fn);
    w.scalar(flags);
    w.scalar(n);
    w.bytes(payload.data(), payload.size());
    if (buf.size() >= kFlushBytes) flush();
    return creating ? ++creates : 0;
  }
};

ar_recorder* ar_rec_open(const char* path, const ar_build_record& build, std::string* err) {
  FILE* f = nullptr;
  if (_wfopen_s(&f, std::filesystem::u8path(path).c_str(), L"wb") != 0 || !f) {
    if (err) *err = std::string("cannot open recording '") + path + "' for writing";
    return nullptr;
  }
  ar_recorder* r = new ar_recorder;
  r->f = f;
  Writer w{r->buf};
  w.bytes(kMagic, sizeof kMagic);
  w.scalar<uint64_t>(AR_ABI_HASH_VALUE);
  w.bytes(&build, sizeof build);
  return r;
}

void ar_rec_close(ar_recorder* r, std::string* err) {
  if (!r) return;
  r->flush();
  if (r->f && fclose(r->f) != 0 && r->error.empty()) r->error = "close failed";
  if (err) *err = r->error;
  delete r;
}

void ar_rec_bind(ar_recorder* r, uint64_t id, const void* handle) {
  std::lock_guard<std::mutex> lk(r->mu);
  r->ids[handle] = id;
}

"""


def emit_recorder_cpp(m):
    s = REC_HEAD
    s += "namespace {\n"
    for name in m.used:
        s += f"void enc_{name}(Writer& w, const {name}& v) {{\n"
        for f, kind, t, extra in m.ordered(name):
            s += enc_stmt(f"v.{f}", kind, t, f"v.{extra}" if extra else None, "  ")
        s += "}\n"
    s += "}  // namespace\n\n"
    for idx, n, rows in m.rec:
        creating = any(m.classify(n, r)[0] == "hout" for r in rows)
        s += f"uint64_t ar_rec_{n}(ar_recorder* rec_, {cparams(rows)}) {{\n"
        s += "  std::lock_guard<std::mutex> lk_(rec_->mu);\n  rec_->payload.clear();\n  Writer w{rec_->payload};\n"
        for r in rows:
            k = m.classify(n, r)
            p = r[3]
            if k[0] == "handle":
                s += f"  w.scalar(rec_->id_of({p}));\n"
            elif k[0] == "scalar":
                s += f"  w.scalar({p});\n"
            elif k[0] == "string":
                s += f"  w.cstr({p});\n"
            elif k[0] == "struct":
                s += f"  w.u8({p} != nullptr);\n  if ({p}) enc_{k[1]}(w, *{p});\n"
            elif k[0] == "array":
                cnt = f"(size_t){k[2]}"
                if k[1] in LIKE:
                    s += f"  w.array({p}, {cnt});\n"
                else:
                    s += f"  w.array_of({p}, {cnt}, [&](const {k[1]}& e) {{ enc_{k[1]}(w, e); }});\n"
            else:
                s += f"  (void){p};\n"
        edit = next((r[3] for r in rows if m.classify(n, r)[0] == "handle"), None)
        if n == "ar_edit_item_commit":
            s += f"""  {{
    auto it = rec_->staged.find({{rec_->id_of({edit}), key}});
    if (it == rec_->staged.end()) {{
      w.scalar<uint32_t>(0xFFFFFFFFu);
    }} else {{
      w.scalar(it->second.kind);
      for (const StagedField& sf : it->second.fields) {{
        size_t bytes = size_t(sf.count) * sf.esize;
        w.scalar<uint64_t>(sf.count);
        if (sf.count && *sf.slot) w.bytes(*sf.slot, bytes);
        else w.bytes(std::vector<uint8_t>(bytes).data(), bytes);
      }}
      rec_->staged.erase(it);
    }}
  }}
"""
        if n in ("ar_edit_commit", "ar_edit_abort"):
            s += f"""  {{
    uint64_t id = rec_->id_of({edit});
    rec_->staged.erase(rec_->staged.lower_bound({{id, 0}}), rec_->staged.upper_bound({{id, ~uint64_t(0)}}));
  }}
"""
        s += f"  return rec_->append({idx}, {'true' if creating else 'false'});\n}}\n\n"
        bi = m.begin_info(n, rows)
        if bi:
            out, st = bi
            edit = next(r[3] for r in rows if m.classify(n, r)[0] == "handle")
            s += f"void ar_rec_post_{n}(ar_recorder* rec_, {cparams(rows)}) {{\n"
            s += "  std::lock_guard<std::mutex> lk_(rec_->mu);\n  StagedItem item;\n"
            s += f"  item.kind = {idx};\n"
            for f, cnt, t in m.staged(st):
                s += (f"  item.fields.push_back({{{out}->{cnt}, reinterpret_cast<void**>({out}->{f}), "
                      f"static_cast<uint32_t>(sizeof({t}))}});\n")
            for r in rows:
                s += f"  (void){r[3]};\n"
            s += f"  rec_->staged[{{rec_->id_of({edit}), static_cast<uint64_t>({m.key_expr(n, rows)})}}] = std::move(item);\n}}\n\n"
    return s


REPLAY_H = """// Generated by tools/abigen.py from include/astroray/astroray.h; never edited.
// Replays an .arrec through the public ABI only (0001 D1.4).
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

#include "astroray/astroray.h"

struct ArReplayOptions {
  const char* log_path = nullptr;     // overrides the recorded context log path when set
  const char* record_path = nullptr;  // records the replay when set (else none)
  bool keep_alive = false;            // skip the recorded *_destroy calls; the caller owns the handles
};

struct ArReplayResult {
  ar_status status = AR_OK;  // the first failing call, AR_ERR_ABI_MISMATCH, or AR_ERR_INVALID_ARGUMENT for a bad file
  std::string message;
  uint64_t calls = 0;
  ar_context context = nullptr;
  std::vector<ar_scene> scenes;      // live handles only with keep_alive
  std::vector<ar_session> sessions;  // the sessions to render
};

ar_status ar_replay_file(const char* path, const ArReplayOptions& opt, ArReplayResult* out);
"""

REPLAY_HEAD = """// Generated by tools/abigen.py from include/astroray/astroray.h; never edited.
#include "replayer.h"

#include <stdio.h>
#include <string.h>

#include <deque>
#include <map>
#include <utility>

namespace {

constexpr char kMagic[8] = {'A', 'R', 'R', 'E', 'C', '0', '0', '1'};

using Arena = std::deque<std::vector<unsigned char>>;

struct Reader {
  const uint8_t* p;
  const uint8_t* end;
  bool fail = false;
  size_t left() const { return static_cast<size_t>(end - p); }
  const uint8_t* take(size_t n) {
    if (n > left()) {
      fail = true;
      p = end;
      return nullptr;
    }
    const uint8_t* q = p;
    p += n;
    return q;
  }
  void bytes(void* dst, size_t n) {
    const uint8_t* q = take(n);
    if (q && n) memcpy(dst, q, n);
  }
  template <class T>
  T scalar() {
    T v{};
    bytes(&v, sizeof v);
    return v;
  }
  const char* cstr(Arena& a) {
    uint32_t n = scalar<uint32_t>();
    if (n == 0xFFFFFFFFu || fail) return nullptr;
    const uint8_t* q = take(n);
    if (!q) return nullptr;
    a.emplace_back(size_t(n) + 1, 0);
    memcpy(a.back().data(), q, n);
    return reinterpret_cast<const char*>(a.back().data());
  }
  template <class T>
  T* alloc(Arena& a, size_t n) {
    a.emplace_back(n * sizeof(T) + 1, 0);
    return reinterpret_cast<T*>(a.back().data());
  }
  template <class T>
  const T* array(Arena& a, size_t n) {
    if (scalar<uint8_t>() == 0 || fail) return nullptr;
    if (n > left() / sizeof(T)) {
      fail = true;
      return nullptr;
    }
    T* d = alloc<T>(a, n);
    bytes(d, n * sizeof(T));
    return d;
  }
  template <class T, class F>
  const T* array_of(Arena& a, size_t n, F&& f) {
    if (scalar<uint8_t>() == 0 || fail) return nullptr;
    if (n > left()) {
      fail = true;
      return nullptr;
    }
    T* d = alloc<T>(a, n);
    for (size_t i = 0; i < n && !fail; ++i) f(d[i]);
    return d;
  }
};

struct Slot {
  uint64_t count;
  void** slot;
  uint32_t esize;
};
struct Open {
  uint32_t kind;
  std::vector<Slot> slots;
};

struct State {
  const ArReplayOptions* opt;
  ArReplayResult* res;
  std::map<uint64_t, void*> live;
  uint64_t creates = 0;
  std::map<std::pair<uint64_t, uint64_t>, Open> open;
  std::string msg;  // why the last record could not be replayed
  void* find(uint64_t id) const {
    auto it = live.find(id);
    return it == live.end() ? nullptr : it->second;
  }
};

"""


def emit_replayer_cpp(m):
    s = REPLAY_HEAD
    for name in m.used:
        s += f"void dec_{name}(Reader& r, Arena& a, {name}& v) {{\n  (void)a;\n  v = {name}{{}};\n"
        for f, kind, t, extra in m.ordered(name):
            s += dec_stmt(f"v.{f}", kind, t, f"v.{extra}" if extra else None, "  ")
        s += "}\n\n"
    s += """// Returns the call's status; a record that cannot be decoded sets st.msg and stop.
ar_status replay_record(State& st, uint32_t fn, Reader& r, Arena& a, bool& stop) {
  (void)a;
  switch (fn) {
"""
    for idx, n, rows in m.rec:
        s += f"    case {idx}: {{  // {n}\n"
        for p in rows:
            k = m.classify(n, p)
            nm = p[3]
            if k[0] == "handle":
                s += f"      uint64_t id_{nm} = r.scalar<uint64_t>();\n      {k[1]} {nm} = static_cast<{k[1]}>(st.find(id_{nm}));\n"
            elif k[0] == "scalar":
                s += f"      {p[0]} {nm} = r.scalar<{p[0]}>();\n"
            elif k[0] == "string":
                s += f"      const char* {nm} = r.cstr(a);\n"
            elif k[0] == "struct":
                s += (f"      {k[1]} {nm}_v{{}};\n      bool {nm}_p = r.scalar<uint8_t>() != 0;\n"
                      f"      if ({nm}_p) dec_{k[1]}(r, a, {nm}_v);\n")
                if n == "ar_context_create":
                    s += (f"      if (st.opt->log_path) {nm}_v.log_path = st.opt->log_path;\n"
                          f"      {nm}_v.record_path = st.opt->record_path;\n")
                s += f"      const {k[1]}* {nm} = {nm}_p ? &{nm}_v : nullptr;\n"
            elif k[0] == "array":
                if k[1] in LIKE:
                    s += f"      const {k[1]}* {nm} = r.array<{k[1]}>(a, (size_t){k[2]});\n"
                else:
                    s += (f"      const {k[1]}* {nm} = r.array_of<{k[1]}>(a, (size_t){k[2]}, "
                          f"[&]({k[1]}& e) {{ dec_{k[1]}(r, a, e); }});\n")
            elif k[0] == "hout":
                s += f"      {k[1]} {nm}_v = nullptr;\n      {k[1]}* {nm} = &{nm}_v;\n"
            elif k[0] == "out":
                s += f"      {k[1]} {nm}_v{{}};\n      {k[1]}* {nm} = &{nm}_v;\n"
            else:
                s += f"      {k[1]}* {nm} = nullptr;\n"
        if n == "ar_edit_item_commit":
            s += """      uint32_t staged_kind = r.scalar<uint32_t>();
      if (staged_kind != 0xFFFFFFFFu) {
        auto it = st.open.find({id_edit, key});
        if (it == st.open.end() || it->second.kind != staged_kind) {
          st.msg = "item commit carries staged arrays but no matching begin was replayed";
          stop = true;
          return AR_ERR_INVALID_ARGUMENT;
        }
        for (const Slot& sl : it->second.slots) {
          uint64_t count = r.scalar<uint64_t>();
          const uint8_t* src = r.take(size_t(count) * sl.esize);
          if (r.fail || count != sl.count) {
            st.msg = "staged array count differs from the replayed begin";
            stop = true;
            return AR_ERR_INVALID_ARGUMENT;
          }
          if (count) memcpy(*sl.slot, src, size_t(count) * sl.esize);
        }
        st.open.erase(it);
      }
"""
        s += """      if (r.fail || r.left() != 0) {
        st.msg = "record payload does not match its function's parameters";
        stop = true;
        return AR_ERR_INVALID_ARGUMENT;
      }
"""
        creating = [p for p in rows if m.classify(n, p)[0] == "hout"]
        if creating:
            s += "      uint64_t new_id = ++st.creates;\n"
        if n in DESTROYS:
            s += "      if (st.opt->keep_alive) return AR_OK;\n"
        s += f"      ar_status code = {n}({cargs(rows)});\n"
        for p in creating:
            b = m.classify(n, p)[1]
            s += f"      if (code == AR_OK && {p[3]}_v) {{\n        st.live[new_id] = {p[3]}_v;\n"
            if b == "ar_context":
                s += f"        st.res->context = {p[3]}_v;\n"
            if b == "ar_scene":
                s += f"        st.res->scenes.push_back({p[3]}_v);\n"
            if b == "ar_session":
                s += f"        st.res->sessions.push_back({p[3]}_v);\n"
            s += "      }\n"
        if n in ("ar_edit_commit", "ar_edit_abort"):
            s += "      st.open.erase(st.open.lower_bound({id_edit, 0}), st.open.upper_bound({id_edit, ~uint64_t(0)}));\n"
        bi = m.begin_info(n, rows)
        if bi:
            out, sname = bi
            edit = next(p[3] for p in rows if m.classify(n, p)[0] == "handle")
            s += f"      if (code == AR_OK) {{\n        Open o;\n        o.kind = {idx};\n"
            for f, cnt, t in m.staged(sname):
                s += (f"        o.slots.push_back({{{out}_v.{cnt}, reinterpret_cast<void**>({out}_v.{f}), "
                      f"static_cast<uint32_t>(sizeof({t}))}});\n")
            key = m.key_expr(n, rows).replace("->", "_v.")
            s += f"        st.open[{{id_{edit}, static_cast<uint64_t>({key})}}] = std::move(o);\n      }}\n"
        s += "      return code;\n    }\n"
    s += """    default:
      st.msg = "record names a function that is not a recordable call";
      stop = true;
      return AR_ERR_INVALID_ARGUMENT;
  }
}

const char* const kNames[] = {
"""
    s += "".join(f'    "{n}",\n' for _, n, _ in m.funcs)
    s += """};

}  // namespace

ar_status ar_replay_file(const char* path, const ArReplayOptions& opt, ArReplayResult* out) {
  *out = ArReplayResult{};
  auto bad = [&](ar_status code, const std::string& why) {
    out->status = code;
    out->message = why;
    return code;
  };
  FILE* f = nullptr;
  if (fopen_s(&f, path, "rb") != 0 || !f) return bad(AR_ERR_INVALID_ARGUMENT, std::string("cannot open ") + path);
  std::vector<uint8_t> data;
  std::vector<uint8_t> chunk(size_t(1) << 16);
  size_t got;
  while ((got = fread(chunk.data(), 1, chunk.size(), f)) > 0) data.insert(data.end(), chunk.begin(), chunk.begin() + got);
  fclose(f);
  const size_t header = sizeof kMagic + 8 + sizeof(ar_build_record);
  if (data.size() < header || memcmp(data.data(), kMagic, sizeof kMagic) != 0)
    return bad(AR_ERR_INVALID_ARGUMENT, std::string(path) + " is not an ARREC001 recording");
  uint64_t file_hash = 0, lib_hash = 0;
  memcpy(&file_hash, data.data() + sizeof kMagic, 8);
  if (ar_abi_hash(&lib_hash) != AR_OK) return bad(AR_ERR_INTERNAL, "ar_abi_hash failed");
  if (file_hash != lib_hash) {
    char b[160];
    snprintf(b, sizeof b, "recording ABI hash 0x%016llx does not match this build's ABI hash 0x%016llx",
             static_cast<unsigned long long>(file_hash), static_cast<unsigned long long>(lib_hash));
    return bad(AR_ERR_ABI_MISMATCH, b);
  }
  State st;
  st.opt = &opt;
  st.res = out;
  size_t pos = header;
  while (pos < data.size()) {
    uint32_t fn = 0;
    uint64_t n = 0;
    if (data.size() - pos < 16) return bad(AR_ERR_INVALID_ARGUMENT, "truncated record header");
    memcpy(&fn, &data[pos], 4);
    memcpy(&n, &data[pos + 8], 8);
    pos += 16;
    if (n > data.size() - pos) return bad(AR_ERR_INVALID_ARGUMENT, "truncated record payload");
    Reader r{data.data() + pos, data.data() + pos + n};
    pos += static_cast<size_t>(n);
    Arena a;
    bool stop = false;
    st.msg.clear();
    ar_status code = replay_record(st, fn, r, a, stop);
    ++out->calls;
    if (stop) return bad(code, st.msg + " (record " + std::to_string(out->calls) + ")");
    if (code != AR_OK && out->status == AR_OK)
      bad(code, std::string(kNames[fn]) + " returned status " + std::to_string(code) + " (record " +
                    std::to_string(out->calls) + ")");
  }
  return out->status;
}
"""
    return s


ARREC_PY = '''# SPDX-License-Identifier: Apache-2.0
# Generated by tools/abigen.py from include/astroray/astroray.h; never edited.
"""Decodes an .arrec into (function name, args) tuples (0001 D1.4)."""
import struct

ABI_HASH = 0x%(hash)016x
BUILD_RECORD_SIZE = %(brs)d
NONE_KIND = 0xFFFFFFFF
FMT = {"uint8_t": "B", "uint32_t": "I", "int32_t": "i", "uint64_t": "Q", "ar_key": "Q", "float": "f", "double": "d"}
DTYPE = {"uint8_t": "uint8", "uint32_t": "uint32", "int32_t": "int32", "uint64_t": "uint64", "ar_key": "uint64",
         "float": "float32", "double": "float64"}
# name -> [(field, kind, type, extra)] in wire order (kind: scalar fixed struct string array)
STRUCTS = %(structs)s
# index -> (name, [(parameter, kind, type, count parameter)]) for the recordable functions
FUNCS = %(funcs)s
# index of a staging begin function -> [(field, element type)]
STAGED = %(staged)s


class Reader:
    def __init__(self, data, pos=0):
        self.data, self.pos = data, pos

    def take(self, n):
        if self.pos + n > len(self.data):
            raise ValueError("truncated recording")
        b = self.data[self.pos:self.pos + n]
        self.pos += n
        return b

    def scalar(self, t):
        return struct.unpack("<" + FMT[t], self.take(struct.calcsize(FMT[t])))[0]

    def elems(self, t, n):
        if t == "uint8_t":
            return bytes(self.take(n))
        return list(struct.unpack("<%%d%%s" %% (n, FMT[t]), self.take(n * struct.calcsize(FMT[t]))))


def read_struct(r, name):
    v = {}
    for f, kind, t, extra in STRUCTS[name]:
        if kind == "scalar":
            v[f] = r.scalar(t)
        elif kind == "fixed":
            v[f] = r.elems(t, extra)
        elif kind == "struct":
            v[f] = read_struct(r, t)
        elif kind == "string":
            n = r.scalar("uint32_t")
            v[f] = None if n == 0xFFFFFFFF else bytes(r.take(n)).decode("utf-8")
        else:
            n = v[extra]
            if not r.scalar("uint8_t"):
                v[f] = None
            elif t in FMT:
                v[f] = r.elems(t, n)
            else:
                v[f] = [read_struct(r, t) for _ in range(n)]
    return v


def decode(path):
    """Returns (header, calls): header has abi_hash and build (bytes); calls is a list of
    (function name, args dict); an item commit's args carry "staged": [(field, dtype, count, bytes)]."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"ARREC001":
        raise ValueError(path + " is not an ARREC001 recording")
    r = Reader(data, 8)
    header = {"abi_hash": r.scalar("uint64_t"), "build": bytes(r.take(BUILD_RECORD_SIZE))}
    calls = []
    while r.pos < len(data):
        fn, flags, n = struct.unpack("<IIQ", r.take(16))
        end = r.pos + n
        name, params = FUNCS[fn]
        args = {}
        for p, kind, t, extra in params:
            if kind == "handle":
                args[p] = r.scalar("uint64_t")
            elif kind == "scalar":
                args[p] = r.scalar(t)
            elif kind == "string":
                m = r.scalar("uint32_t")
                args[p] = None if m == 0xFFFFFFFF else bytes(r.take(m)).decode("utf-8")
            elif kind == "struct":
                args[p] = read_struct(r, t) if r.scalar("uint8_t") else None
            else:
                m = args[extra]
                if not r.scalar("uint8_t"):
                    args[p] = None
                elif t in FMT:
                    args[p] = r.elems(t, m)
                else:
                    args[p] = [read_struct(r, t) for _ in range(m)]
        if name == "ar_edit_item_commit":
            kind = r.scalar("uint32_t")
            staged = []
            if kind != NONE_KIND:
                for field, t in STAGED[kind]:
                    cnt = r.scalar("uint64_t")
                    staged.append((field, DTYPE[t], cnt, bytes(r.take(cnt * struct.calcsize(FMT[t])))))
            args["staged"] = staged
        if r.pos != end:
            raise ValueError(f"{name}: payload is {n} bytes but {n - (end - r.pos)} were decoded")
        calls.append((name, args))
    return header, calls
'''


def struct_size(m, name):
    off, al = 0, 1
    for base, stars, _, f, arr in m.structs[name]:
        if stars:
            raise SystemExit(f"{name}: pointer fields in a fixed-size struct")
        sz, a = (SIZES[base],) * 2 if base in SIZES else struct_size(m, base)
        off = (off + a - 1) // a * a + sz * (arr or 1)
        al = max(al, a)
    return (off + al - 1) // al * al, al


def emit_arrec_py(m, h):
    from pprint import pformat
    structs = {n: m.ordered(n) for n in m.used}
    funcs, staged = {}, {}
    for idx, n, rows in m.rec:
        pr = []
        for r in rows:
            k = m.classify(n, r)
            if k[0] in ("handle", "scalar", "string", "struct", "array"):
                pr.append((r[3], k[0], k[1] if len(k) > 1 else None, k[2] if k[0] == "array" else None))
        funcs[idx] = (n, pr)
        bi = m.begin_info(n, rows)
        if bi:
            staged[idx] = [(f, t) for f, _, t in m.staged(bi[1])]
    return ARREC_PY % {"hash": h, "brs": struct_size(m, "ar_build_record")[0], "structs": pformat(structs, width=110),
                       "funcs": pformat(funcs, width=110), "staged": pformat(staged, width=110)}


def emit_recorder_files(chk, h, outdir):
    m = Model(chk)
    spdx = "// SPDX-License-Identifier: Apache-2.0\n"
    files = {"recorder_hooks.h": emit_hooks_h(m), "recorder.cpp": emit_recorder_cpp(m),
             "replayer.h": REPLAY_H, "replayer.cpp": emit_replayer_cpp(m), "arrec.py": emit_arrec_py(m, h)}
    for name, content in files.items():
        if name.endswith((".h", ".cpp")):
            content = spdx + content
        with open(os.path.join(outdir, name), "w", encoding="utf-8", newline="\n") as f:
            f.write(content)


def check_hooks(header, capi):
    """Every implemented non-query entry point records itself (0001 D2.10 recording rule)."""
    with open(header, encoding="utf-8", newline="") as f:
        chk = Checker(header, f.read().replace("\r\n", "\n"))
    if chk.run():
        print(f"{header}: does not pass --check")
        return 1
    m = Model(chk)
    rec = {n: rows for _, n, rows in m.rec}
    errors = []
    for fname in sorted(os.listdir(capi)):
        if not fname.endswith(".cpp") or fname == "unimplemented.cpp":
            continue
        with open(os.path.join(capi, fname), encoding="utf-8", newline="") as f:
            text = strip_comments(f.read())
        defs = list(re.finditer(r"AR_API\s+ar_status\s+(ar_\w+)\s*\(", text))
        for i, d in enumerate(defs):
            n = d.group(1)
            body = text[d.end():defs[i + 1].start() if i + 1 < len(defs) else len(text)]
            line = text.count("\n", 0, d.start()) + 1
            if n not in rec:
                continue
            if f"AR_RECORD({n}," not in body:
                errors.append(f"{capi}/{fname}:{line}: {n} is implemented without AR_RECORD({n}, ...)")
            if any(m.classify(n, r)[0] == "hout" for r in rec[n]) and "AR_RECORD_BIND(" not in body:
                errors.append(f"{capi}/{fname}:{line}: {n} writes a handle without AR_RECORD_BIND(...)")
            if m.begin_info(n, rec[n]) and f"AR_RECORD_POST({n}," not in body:
                errors.append(f"{capi}/{fname}:{line}: {n} stages arrays without AR_RECORD_POST({n}, ...)")
    for e in errors:
        print(e)
    return 1 if errors else 0


def main(argv):
    if len(argv) == 4 and argv[1] == "--check-hooks":
        return check_hooks(argv[2], argv[3])
    if len(argv) == 4 and argv[1] == "--emit":
        return emit(argv[2], argv[3])
    if len(argv) != 3 or argv[1] not in ("--check", "--hash"):
        print("usage: abigen.py --check|--hash <header> | --emit <header> <outdir> | --check-hooks <header> <capi dir>", file=sys.stderr)
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
