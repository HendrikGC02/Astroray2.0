# SPDX-License-Identifier: Apache-2.0
"""Grammar checker and ABI hash for include/astroray/astroray.h.

Usage: abigen.py --check <header> | --hash <header>
Errors print as `file:line: message`; exit status 1 when any was found.
Ref: docs/decisions/0001-product-shape.md D2.7 (hash), D2.10 (grammar).
"""
import hashlib
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


class Checker:
    def __init__(self, path, text):
        self.path = path
        self.errors = []
        self.text = text.replace("\r\n", "\n")
        self.handles = set()
        self.structs = {}  # name -> {field name: type}

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
        _, ret, name, params = m.groups()
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


def main(argv):
    if len(argv) != 3 or argv[1] not in ("--check", "--hash"):
        print("usage: abigen.py --check|--hash <header>", file=sys.stderr)
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
