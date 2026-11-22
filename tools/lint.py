# SPDX-License-Identifier: Apache-2.0
"""Repository lint: licences, NVIDIA material, ASCII scripts, process caps, skips, task files, build flags, doc citations.

Run: <python> -I tools/lint.py [--self-test] [--section name]
Ref: docs/decisions/0009-build-test-process.md d10, d28, d31; 0001 D7.1 and V4.
Findings print as `path:line: [section] message`; exit 1 on any finding.
"""
import argparse
import fnmatch
import json
import os
import re
import subprocess
import sys

CASES = "tests/cpu/lint_cases"

# Frozen: files already over the 150-line cap at the base commit; they may not grow.
V1_LESSONS_OVER_CAP = {
    "docs/v1-lessons/feature-inventory.md": 288,
    "docs/v1-lessons/research/blender.md": 242,
    "docs/v1-lessons/research/gpu-build.md": 339,
    "docs/v1-lessons/research/gr-astro.md": 297,
    "docs/v1-lessons/research/lights.md": 332,
    "docs/v1-lessons/research/materials.md": 350,
    "docs/v1-lessons/research/misc.md": 290,
    "docs/v1-lessons/research/spectral.md": 286,
    "docs/v1-lessons/research/survey.md": 232,
    "docs/v1-lessons/research/testing.md": 325,
    "docs/v1-lessons/research/transport.md": 361,
    "docs/v1-lessons/research/volumes.md": 189,
}

SPDX_EXT = (".h", ".hpp", ".c", ".cpp", ".cu", ".py", ".ps1", ".cmake")
APACHE_DIRS = ("include", "src", "cli", "tools", "tests", "data", "scenes", "bench")
SPDX_RE = re.compile(r"SPDX-License-Identifier:\s*([A-Za-z0-9.+-]+)")
TASK_RE = re.compile(r"^tasks/(\d{3})-[^/]+\.md$")
FIELDS = ["Goal:", "Read first:", "Scope:", "Out of scope:", "Reference:", "Do:",
          "Acceptance:", "Look at:", "Canary:", "Budget:", "Depends on:", "Size:", "Progress:"]
# Spelled so that this file does not match its own pattern.
SKIP_RE = re.compile(r"doctest::(?:skip|may_fail|should_fail)|unittest\.(?:skip\w*|expected[F]ailure)|\bexpected[F]ailure\b")


class Ctx:
    def __init__(self, root, files):
        self.root = root
        self.files = sorted(files)  # forward-slash relative paths
        self.out = []

    def find(self, path, line, section, msg):
        self.out.append(f"{path}:{line}: [{section}] {msg}")

    def lines(self, path):
        with open(os.path.join(self.root, path), encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()


def spdx(c):
    zip_spdx(c)
    for p in c.files:
        base = os.path.basename(p)
        if not (p.endswith(SPDX_EXT) or base == "CMakeLists.txt"):
            continue
        top = p.split("/")[0]
        if top == "third_party":
            continue
        m = None
        for i, ln in enumerate(c.lines(p)[:5], 1):
            m = SPDX_RE.search(ln)
            if m:
                break
        if not m:
            c.find(p, 1, "spdx", "missing SPDX-License-Identifier in first 5 lines")
            continue
        want = None
        if top in APACHE_DIRS:
            want = "Apache-2.0"
        elif top == "blender":
            want = "Apache-2.0" if p.endswith(("_abi.py", "_ops.py")) else "GPL-3.0-or-later"
        if want and m.group(1) != want:
            c.find(p, i, "spdx", f"licence is {m.group(1)}, expected {want}")


def built_zips(c):
    """Yield (zip path, ZipFile) for extension zips under out/extension, when built."""
    import glob
    import zipfile

    for z in sorted(glob.glob(os.path.join(c.root, "out", "extension", "*.zip"))):
        with zipfile.ZipFile(z) as zf:
            yield os.path.relpath(z, c.root).replace(os.sep, "/"), zf


def zip_spdx(c):
    # Ref: 0001 V4 (built zip part): files in the zip follow the same rule as the tree.
    for zp, zf in built_zips(c):
        for name in zf.namelist():
            if not name.endswith(".py"):
                continue
            head = zf.read(name).decode("utf-8", "replace").splitlines()[:5]
            m = next((SPDX_RE.search(ln) for ln in head if SPDX_RE.search(ln)), None)
            want = "Apache-2.0" if name in ("_abi.py", "_ops.py") else "GPL-3.0-or-later"
            if not m:
                c.find(zp, 1, "spdx", f"{name}: missing SPDX-License-Identifier")
            elif m.group(1) != want:
                c.find(zp, 1, "spdx", f"{name}: licence is {m.group(1)}, expected {want}")


def zip_nvidia(c):
    pats = ["optix*", "nvsdk_ngx*", "nvngx*", "nvrtc*", "nvcuda*", "nvoptix*", "*.cubin", "*.ptx", "*.cu"]
    for zp, zf in built_zips(c):
        for name in zf.namelist():
            if any(fnmatch.fnmatch(os.path.basename(name).lower(), q) for q in pats):
                c.find(zp, 1, "nvidia", f"{name}: NVIDIA or loose kernel file in the built zip")

def own_optix_wrapper(c, p, base):
    # Our own OptiX wrappers (src/device/cuda/optix_*.h) carry the project SPDX line;
    # SDK headers do not. Their content is still checked for NVIDIA licence text below.
    if not (p.startswith("src/device/cuda/") and fnmatch.fnmatch(base, "optix*.h")):
        return False
    head = c.lines(p)[:3]
    return any("SPDX-License-Identifier: Apache-2.0" in ln for ln in head)


def nvidia(c):
    zip_nvidia(c)
    pats = ["optix*.h", "nvsdk_ngx*", "nvrtc*", "*.dll", "*.lib", "*.cubin", "*.ptx"]
    for p in c.files:
        base = os.path.basename(p).lower()
        if any(fnmatch.fnmatch(base, q) for q in pats) and not own_optix_wrapper(c, p, base):
            c.find(p, 1, "nvidia", "NVIDIA material must not be tracked")
            continue
        if p.startswith("docs/"):
            continue
        with open(os.path.join(c.root, p), "rb") as f:
            head = f.read(2048)
        if b"\0" in head:
            continue
        # A licence header is a copyright/licence line in the first lines, not any mention.
        for n, ln in enumerate(head.decode("utf-8", "replace").splitlines()[:10], 1):
            if re.match(r"\s*(//|/\*|\*|#)", ln) and re.search(r"copyright|licen[sc]e", ln, re.I) and "NVIDIA Corporation" in ln:
                c.find(p, n, "nvidia", "NVIDIA Corporation licence header outside docs/")
                break


def ascii_ps1(c):
    for p in c.files:
        if not p.endswith(".ps1"):
            continue
        with open(os.path.join(c.root, p), "rb") as f:
            for n, ln in enumerate(f.read().split(b"\n"), 1):
                if any(b > 127 for b in ln):
                    c.find(p, n, "ascii-ps1", "non-ASCII character")
                    break


def caps(c):
    def over(p, cap):
        n = len(c.lines(p))
        if n > cap:
            c.find(p, 1, "caps", f"{n} lines, cap {cap}")

    def count(pat, cap, what):
        m = [p for p in c.files if fnmatch.fnmatchcase(p, pat) and p.count("/") == pat.count("/")]
        if len(m) > cap:
            c.find(m[cap], 1, "caps", f"{len(m)} {what} files, at most {cap}")
        return m

    if "CLAUDE.md" in c.files:
        over("CLAUDE.md", 60)
    rules = count(".claude/rules/*.md", 10**6, "rule")
    agents = count(".claude/agents/*.md", 3, "agent")
    skills = count(".claude/skills/*/SKILL.md", 2, "skill")
    for p in rules:
        over(p, 40)
    for p in agents:
        over(p, 60)
    for p in skills:
        over(p, 80)
    for p, cap in (("docs/STATUS.md", 50), ("tasks/README.md", 40), ("tools/README.md", 30)):
        if p in c.files:
            over(p, cap)
    for p in c.files:
        if TASK_RE.match(p):
            over(p, 60)
        elif p.startswith("docs/v1-lessons/") and p.endswith(".md"):
            n = len(c.lines(p))
            frozen = V1_LESSONS_OVER_CAP.get(p)
            if frozen is None and n > 150:
                c.find(p, 1, "caps", f"{n} lines, cap 150")
            elif frozen is not None and n > frozen:
                c.find(p, 1, "caps", f"{n} lines, frozen at {frozen}")
    if ".claude/settings.json" in c.files:
        try:
            with open(os.path.join(c.root, ".claude/settings.json"), encoding="utf-8") as f:
                if "hooks" in json.load(f):
                    c.find(".claude/settings.json", 1, "caps", "hooks key is not allowed")
        except ValueError as e:
            c.find(".claude/settings.json", 1, "caps", f"invalid JSON: {e}")
    for p in c.files:
        parts = p.split("/")
        if p == "AGENTS.md" or parts[0] in (".codex", ".opencode"):
            c.find(p, 1, "caps", "forbidden agent file or directory")
    for p in ["CLAUDE.md"] + rules + agents:
        if p not in c.files:
            continue
        for n, ln in enumerate(c.lines(p), 1):
            if re.search(r"\d{4}-\d{2}-\d{2}", ln):
                c.find(p, n, "caps", "date in process file")
            if re.search(r"claude-[a-z]+-\d", ln):
                c.find(p, n, "caps", "model id in process file")


def skip(c):
    for p in c.files:
        if not p.endswith((".cpp", ".h", ".hpp", ".cu", ".py")) or p.startswith("third_party/"):
            continue
        for n, ln in enumerate(c.lines(p), 1):
            if not SKIP_RE.search(ln):
                continue
            m = re.search(r"task (\d{3})\b", ln)
            if not m:
                c.find(p, n, "skip", "skip/xfail must name `task NNN` on the same line")
            elif not any(f.startswith(f"tasks/{m.group(1)}-") and f.endswith(".md") for f in c.files):
                c.find(p, n, "skip", f"task {m.group(1)} has no tasks/{m.group(1)}-*.md")


def taskfile(c):
    for p in c.files:
        m = TASK_RE.match(p)
        if not m:
            continue
        L = c.lines(p)
        if not L or not L[0].startswith(f"# {m.group(1)} "):
            c.find(p, 1, "taskfile", f"first line must start with `# {m.group(1)} `")
        pos, last = {}, 0
        for fld in FIELDS:
            i = next((k for k, ln in enumerate(L) if ln.startswith(fld)), None)
            if i is None:
                c.find(p, 1, "taskfile", f"missing field {fld}")
            elif i < last:
                c.find(p, i + 1, "taskfile", f"field {fld} out of order")
            else:
                pos[fld] = i
                last = i
        if "Progress:" in pos:
            tail = L[pos["Progress:"] + 1:]
            while tail and not tail[-1].strip():
                tail.pop()
            if len(tail) > 10:
                c.find(p, pos["Progress:"] + 1, "taskfile", f"{len(tail)} lines after Progress:, at most 10")


def budgets(c):
    """Ref: 0009 d13 (table), d15 (family sizes): tools/budgets.toml is its machine copy."""
    import tomllib
    toml_path, rec = "tools/budgets.toml", "docs/decisions/0009-build-test-process.md"
    if toml_path not in c.files:
        return
    if rec not in c.files:
        c.find(toml_path, 1, "budgets", f"{rec} not found")
        return
    table = {}
    for ln in c.lines(rec):
        if re.match(r"\| B-\d\d \|", ln):
            cells = [x.strip() for x in ln.strip().strip("|").split(" | ")]
            table[cells[0]] = cells[2]
    try:
        with open(os.path.join(c.root, toml_path), "rb") as f:
            data = tomllib.load(f)
    except (OSError, ValueError) as e:
        c.find(toml_path, 1, "budgets", f"cannot parse: {e}")
        return
    rows = {b.get("id"): b for b in data.get("budget", [])}
    for i in sorted(set(table) - set(rows)):
        c.find(toml_path, 1, "budgets", f"{i} is in 0009 d13 but not in budgets.toml")
    for i in sorted(set(rows) - set(table)):
        c.find(toml_path, 1, "budgets", f"{i} is in budgets.toml but not in 0009 d13")
    for i in sorted(set(rows) & set(table)):
        b = rows[i]
        if b.get("text") != table[i]:
            c.find(toml_path, 1, "budgets", f"{i} text differs from the 0009 d13 limit cell")
        nums = [float(t) for t in re.findall(r"\d+(?:\.\d+)?", b.get("text", ""))]
        if "limit" in b and float(b["limit"]) not in nums:
            c.find(toml_path, 1, "budgets", f"{i} limit {b['limit']} is not in its text")
    if data.get("family", {}).get("gate_statistical") != 2000:
        c.find(toml_path, 1, "budgets", "[family] gate_statistical must be 2000 (0009 d15)")
    fp = data.get("family_provisional", {})
    for k, v in (("full_statistical", 2000), ("analytic_scenes", 256)):
        if fp.get(k) != v:
            c.find(toml_path, 1, "budgets", f"[family_provisional] {k} must be {v} (0009 d15)")
    sizes = {**fp, **data.get("family", {})}
    for k, n in data.get("family_members", {}).items():
        if k not in sizes or not isinstance(n, int) or n > sizes[k]:
            c.find(toml_path, 1, "budgets", f"[family_members] {k} = {n} exceeds or lacks its family size m")


HISTORY_KEYS = {"sha": str, "machine": str, "driver": str, "cuda": str, "scene": str,
                "metric": str, "value": (int, float), "unit": str, "n": int,
                "spread": (int, float), "date": str}
DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d+)?(Z|\+00:00)$")


def history(c):
    """Ref: 0009 d22 (history line), d29; tasks/011: one JSON object per line, fixed keys."""
    last = ""
    for p in c.files:
        if not re.match(r"^bench/history/[^/]+\.jsonl$", p):
            continue
        for n, ln in enumerate(c.lines(p), 1):
            try:
                o = json.loads(ln)
            except ValueError as e:
                c.find(p, n, "history", f"not JSON: {e}")
                continue
            if not isinstance(o, dict):
                c.find(p, n, "history", "line is not a JSON object")
                continue
            if set(o) != set(HISTORY_KEYS):
                miss, extra = sorted(set(HISTORY_KEYS) - set(o)), sorted(set(o) - set(HISTORY_KEYS))
                c.find(p, n, "history", f"keys differ: missing {miss}, unexpected {extra}")
                continue
            for k, t in HISTORY_KEYS.items():
                if isinstance(o[k], bool) or not isinstance(o[k], t):
                    c.find(p, n, "history", f"key {k} has the wrong type")
            if isinstance(o["n"], int) and not isinstance(o["n"], bool) and o["n"] < 1:
                c.find(p, n, "history", "n must be >= 1")
            if isinstance(o["date"], str):
                if not DATE_RE.match(o["date"]):
                    c.find(p, n, "history", "date is not ISO 8601 UTC (YYYY-MM-DDTHH:MM:SSZ)")
                elif o["date"] < last:
                    c.find(p, n, "history", "date is earlier than the previous line")
                else:
                    last = o["date"]


# Ref: 0001 D9.1 (closed list of what ships vendored or as a fetched binary).
D9_1_ALLOWED = {"doctest", "embree", "oidn", "onetbb", "nanovdb", "stb_image", "tinyexr", "mikktspace"}
DEP_FIELDS = ["name", "version", "url", "sha256", "spdx", "kind", "purpose", "record"]
DEP_KINDS = {"vendored", "fetched-binary", "fetched-not-redistributable", "tool-python", "tool-binary"}


def deps(c):
    """Ref: tasks/008, 0001 D9.1, D9.6: deps.toml is complete, on the closed list, and renders THIRD_PARTY.md."""
    import importlib.util
    import tomllib
    toml_path = "third_party/deps.toml"
    if toml_path not in c.files:
        return
    try:
        with open(os.path.join(c.root, toml_path), "rb") as f:
            data = tomllib.load(f)
    except (OSError, ValueError) as e:
        c.find(toml_path, 1, "deps", f"cannot parse: {e}")
        return
    names = set()
    for d in data.get("dep", []):
        name = d.get("name", "?")
        names.add(name)
        for k in DEP_FIELDS:
            if not isinstance(d.get(k), str) or not d[k].strip():
                c.find(toml_path, 1, "deps", f"{name}: missing field {k}")
        if d.get("kind") not in DEP_KINDS:
            c.find(toml_path, 1, "deps", f"{name}: kind {d.get('kind')!r} is not one of {sorted(DEP_KINDS)}")
        if not re.fullmatch(r"[0-9a-f]{64}", str(d.get("sha256", ""))):
            c.find(toml_path, 1, "deps", f"{name}: sha256 is not 64 lowercase hex digits")
        if d.get("kind") in ("vendored", "fetched-binary") and name not in D9_1_ALLOWED:
            c.find(toml_path, 1, "deps", f"{name}: not on the 0001 D9.1 closed list")
    notice = "THIRD_PARTY.md"
    if notice not in c.files:
        c.find(toml_path, 1, "deps", f"{notice} is missing: run tools/thirdparty.py")
    else:
        spec = importlib.util.spec_from_file_location(
            "thirdparty", os.path.join(os.path.dirname(os.path.abspath(__file__)), "thirdparty.py"))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        with open(os.path.join(c.root, notice), encoding="utf-8", newline="") as f:
            if f.read().replace("\r\n", "\n") != mod.render(data):
                c.find(notice, 1, "deps", "differs from tools/thirdparty.py output: run it")
        req = ".github/ci-requirements.txt"
        try:
            with open(os.path.join(c.root, req), encoding="utf-8", newline="") as f:
                have = f.read().replace("\r\n", "\n")
        except OSError:
            have = None
        if have != mod.render_requirements(data):
            c.find(req, 1, "deps", "missing or differs from tools/thirdparty.py output: run it")
    vendored = {p.split("/")[1] for p in c.files if p.startswith("third_party/") and p.count("/") >= 2}
    for v in sorted(vendored - names):
        c.find(f"third_party/{v}", 1, "deps", f"{v}: vendored directory has no entry in deps.toml")
# ---- kernel section ---------------------------------------------------------
# Ref: docs/decisions/0002-kernel-architecture.md D1, D2.
KERNEL_EXT = (".h", ".hpp", ".cuh", ".inl", ".cpp", ".cu")
KERNEL_PORTABILITY = "src/kernel/portability.h"
KERNEL_BANNED = {
    "virtual": "virtual functions",
    "throw": "exceptions",
    "try": "exceptions",
    "catch": "exceptions",
    "new": "heap allocation",
    "delete": "heap allocation",
    "malloc": "heap allocation",
    "free": "heap allocation",
    "__noinline__": "__noinline__",
    "thread_local": "thread-local storage",
}
KERNEL_ASM = {"asm", "__asm", "__asm__"}
KERNEL_MATH = (r"sqrt|exp|expm1|log|sin|cos|tan|atan2|atan|acos|asin|pow|fma|fabs|floor|ceil|cosh|sinh|tanh"
               r"|log2|log10|exp2|fmod|cbrt|hypot|round|trunc")
# ar::sqrt( is the sanctioned spelling; bare `::sqrt(` and `sqrt(` are not.
MATH_CALL_RE = re.compile(rf"(?<![\w.>])(?<!\w::)(?:{KERNEL_MATH})f?\s*\(")
KERNEL_TOKEN_RE = re.compile(r"""
    (?P<hex>0[xX][0-9a-fA-F']+[uUlL]*)
  | (?P<num>(?:\d[\d']*(?P<dot>\.[\d']*)?|(?P<lead>\.\d[\d']*))(?P<exp>[eE][+-]?\d+)?(?P<suf>[A-Za-z_]*))
  | (?P<id>[A-Za-z_]\w*)
  | (?P<op>\S)
""", re.X)
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]*)[>"]')
PP_RE = re.compile(r"^\s*#\s*(\w+)\s*(.*)$")


def blank_comments(text):
    """Replace comments with spaces, keeping newlines and offsets; strings are respected."""
    out, i, n = [], 0, len(text)
    while i < n:
        ch = text[i]
        if ch == '"' or (ch == "'" and not (i and text[i - 1].isalnum())):
            j = i + 1
            while j < n and text[j] != ch and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(x if x == "\n" else " " for x in text[i:j]))
            i = j
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def blank_strings(text):
    return re.sub(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'',
                  lambda m: m.group(0)[0] + " " * (len(m.group(0)) - 2) + m.group(0)[-1], text)


def kernel_header(c, p, name):
    """A quoted include passes only if it names a file inside src/kernel/, found
    relative to the including file or to src/kernel/ (so "vector" and "../x.h" fail)."""
    for base in (os.path.dirname(p), "src/kernel"):
        q = os.path.normpath(os.path.join(base, name)).replace(os.sep, "/")
        if q.startswith("src/kernel/") and q in c.files:
            return True
    return False


def kernel_scan(c, p):
    is_port = p == KERNEL_PORTABILITY
    lines = blank_comments("\n".join(c.lines(p))).split("\n")
    seen = set()

    def find(line, rule, msg):
        if (line, rule) not in seen:
            seen.add((line, rule))
            c.find(p, line, "kernel", msg)

    # Preprocessor pass: includes, the AR_DEBUG region of each line, and which
    # lines are directives (their braces and semicolons are not C++ statements).
    debug = [False] * len(lines)
    pp = [False] * len(lines)
    stack = []  # one bool per open #if: is it an AR_DEBUG region right now
    cont = False
    keep = []
    for n, ln in enumerate(lines, 1):
        debug[n - 1] = any(stack)
        m = PP_RE.match(ln)
        pp[n - 1] = bool(m) or cont
        cont = pp[n - 1] and ln.rstrip().endswith("\\")
        if not m:
            keep.append(ln)
            continue
        d, rest = m.group(1), m.group(2)
        keep.append(ln if d == "define" else "")
        if d == "include":
            im = INCLUDE_RE.match(ln)
            ok = im and ((im.group(1) == '"' and kernel_header(c, p, im.group(2)))
                         or (im.group(1) == "<" and im.group(2) in ("cstdint", "cfloat"))
                         or (is_port and im.group(1) == "<" and im.group(2) in ("cmath", "intrin.h")))
            if not ok:
                find(n, "include", f"#include {rest} is not allowed in kernel code")
        elif d in ("if", "ifdef"):
            stack.append(bool(re.search(r"\bAR_DEBUG\b", rest)) and "!" not in rest)
        elif d == "ifndef":
            stack.append(False)
        elif d in ("else", "elif") and stack:
            stack[-1] = False
        elif d == "endif" and stack:
            stack.pop()
    text = blank_strings("\n".join(keep))
    starts = [0] + [m.end() for m in re.finditer("\n", text)]

    def line_of(off):
        lo, hi = 0, len(starts) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            lo, hi = (mid, hi) if starts[mid] <= off else (lo, mid - 1)
        return lo + 1

    if not is_port:
        for m in MATH_CALL_RE.finditer(text):
            find(line_of(m.start()), "math", "direct math call; use the ar:: wrappers of portability.h")
        for m in re.finditer(r"\bstd\s*::", text):
            find(line_of(m.start()), "std", "std:: is not allowed in kernel code (containers, algorithms, math)")

    toks = [(m.lastgroup if m.lastgroup in ("hex", "id", "op") else "num", m) for m in KERNEL_TOKEN_RE.finditer(text)]
    parens = []  # token before each open parenthesis
    braces = []  # True where the brace opens a namespace or extern block
    stmt = []
    prev = None
    for k, (kind, m) in enumerate(toks):
        t = m.group(0)
        ln = line_of(m.start())
        if kind == "id":
            if t in KERNEL_BANNED:
                find(ln, t, f"banned in kernel code ({KERNEL_BANNED[t]}): {t}")
            elif t in KERNEL_ASM and not is_port:
                find(ln, "asm", "asm is allowed only in portability.h")
            elif t == "long" and k + 1 < len(toks) and toks[k + 1][1].group(0) == "double":
                find(ln, "longdouble", "banned in kernel code: long double")
            elif re.fullmatch(r"(?:f|s|sn|v)?printf", t) and not debug[ln - 1]:
                find(ln, "printf", "printf is allowed only inside #ifdef AR_DEBUG")
        elif kind == "num":
            if (m.group("dot") or m.group("lead") or m.group("exp")) and not m.group("suf"):
                if (parens[-1] if parens else None) not in ("Real", "float", "double"):
                    find(ln, "literal " + t, f"unsuffixed floating literal {t}; write Real({t})")
        if pp[ln - 1]:
            if t == "(":
                parens.append(prev)
            elif t == ")" and parens:
                parens.pop()
            prev = t
            continue
        if t == "(":
            parens.append(prev)
        elif t == ")" and parens:
            parens.pop()
        if kind == "id" and t in ("AR_NS_BEGIN", "AR_NS_END"):
            stmt = []  # these macros open and close the namespace; no `;` follows them
        elif kind == "op" and t in ";{}" and not parens:
            if t == "{":
                braces.append(any(x in ("namespace", "extern") for x in stmt))
            elif t == "}" and braces:
                braces.pop()
            elif t == ";" and stmt and stmt[0] == "static" and all(braces):
                call = "(" in stmt and ("=" not in stmt or stmt.index("(") < stmt.index("="))
                if not call and not {"const", "constexpr"} & set(stmt):
                    find(ln, "static", "static non-const variable at namespace scope")
            stmt = []
        else:
            stmt.append(t)
        prev = t

    if p.endswith((".h", ".hpp", ".cuh")) and not is_port and p != "src/kernel/core/real.h":
        if not any(k == "id" and m.group(0) == "AR_NS_BEGIN" for k, m in toks):
            find(1, "nsbegin", "header does not open AR_NS_BEGIN")


def kernel(c):
    for p in c.files:
        if p.startswith("src/kernel/") and p.endswith(KERNEL_EXT):
            kernel_scan(c, p)


# ---- seam section: 0001 D5.2 and V3, R39 --------------------------------------
SEAM_SRC_EXT = (".h", ".hpp", ".cuh", ".inl", ".cpp", ".cu", ".c")
SEAM_RULES = (  # (include path pattern, directories allowed to include it); first match wins
    # Our own device wrappers (opaque, vendor headers stay behind them) may be tested directly.
    (re.compile(r"^device/(cpu|cuda)/"), ("src/device/", "src/kernel_gpu/", "tests/cpu/", "tests/gpu/")),
    (re.compile(r"nvrtc", re.I), ()),
    (re.compile(r"ngx", re.I), ("src/denoise/dlss/",)),
    (re.compile(r"embree", re.I), ("src/device/cpu/",)),
    (re.compile(r"cuda|optix", re.I), ("src/device/", "src/kernel_gpu/")),
)


def seam(c):
    for p in c.files:
        if not p.endswith(SEAM_SRC_EXT) or p.startswith("third_party/"):
            continue
        for n, ln in enumerate(c.lines(p), 1):
            m = INCLUDE_RE.match(ln)
            if not m:
                continue
            for pat, allowed in SEAM_RULES:
                if pat.search(m.group(2)):
                    if not p.startswith(allowed):
                        where = "anywhere" if not allowed else "outside " + " or ".join(allowed)
                        c.find(p, n, "seam", f"include of {m.group(2)} is not allowed {where}")
                    break


# ---- flags section: 0002 D1, D12, D14 -------------------------------------------
# Banned in cmake/ and CMakeLists.txt, comments included: even a mention must be reworded.
FLAG_BANS = (
    ("--use_fast_math", re.compile(r"(?<![\w-])-{1,2}use_fast_math")),
    ("-rdc", re.compile(r"(?<![\w-])-{1,2}rdc")),
    ("--relocatable-device-code", re.compile(r"(?<![\w-])--relocatable-device-code")),
    ("--Ofast-compile", re.compile(r"(?<![\w-])--Ofast-compile")),
    ("--split-compile", re.compile(r"(?<![\w-])--split-compile")),
    ("/fp:fast", re.compile(r"(?<!\w)/fp:fast")),
)


def flags(c):
    """Ref: 0002 D1, D12, D14: no build file may carry a flag that breaks the FP or module rules."""
    for p in c.files:
        if p != "CMakeLists.txt" and not p.startswith("cmake/"):
            continue
        for n, ln in enumerate(c.lines(p), 1):
            for name, pat in FLAG_BANS:
                if pat.search(ln):
                    c.find(p, n, "flags", f"banned build flag {name} (0002 D1, D12, D14)")


# Ref: 0009 d21. Only these licences may be committed; share-alike, non-commercial,
# no-derivatives and none-stated never match.
ASSET_SPDX_RE = re.compile(r"CC0(-1\.0)?|CC-BY(-\d\.\d)?|MIT|Apache-2\.0")
ASSET_FILE_CAP = 2 * 1024 * 1024
ASSET_TOTAL_CAP = 50 * 1024 * 1024
IMAGE_MAX_PX = 1600


# ---- data section: 0001 D8.4 and V9 (file part) ---------------------------------
DATA_MANIFEST = "data/MANIFEST.toml"
DATA_FIELDS = ["citation", "version", "sha256", "command"]


def data(c):
    """Ref: 0001 D8.4, V9: every file under data/ has a manifest entry whose hash matches it."""
    import hashlib
    import tomllib
    files = [p for p in c.files if p.startswith("data/") and p not in (DATA_MANIFEST, "data/LICENSE")]
    if DATA_MANIFEST not in c.files:
        for p in files:
            c.find(p, 1, "data", f"{DATA_MANIFEST} is missing")
        return
    try:
        with open(os.path.join(c.root, DATA_MANIFEST), "rb") as f:
            entries = tomllib.load(f).get("file", [])
    except (OSError, ValueError) as e:
        c.find(DATA_MANIFEST, 1, "data", f"cannot parse: {e}")
        return
    by_path = {}
    for e in entries:
        path = e.get("path")
        if not isinstance(path, str) or not path:
            c.find(DATA_MANIFEST, 1, "data", "entry without a path")
            continue
        full = "data/" + path
        if full in by_path:
            c.find(full, 1, "data", "listed twice in MANIFEST.toml")
        by_path[full] = e
        for k in DATA_FIELDS:
            if not isinstance(e.get(k), str) or not e[k].strip():
                c.find(full, 1, "data", f"MANIFEST.toml entry has no {k}")
        if not any(isinstance(e.get(k), str) and e[k].strip() for k in ("spdx", "url")):
            c.find(full, 1, "data", "MANIFEST.toml entry has neither spdx nor url")
        if full not in c.files:
            c.find(full, 1, "data", "listed in MANIFEST.toml but the file does not exist")
        elif isinstance(e.get("sha256"), str):
            with open(os.path.join(c.root, full), "rb") as f:
                got = hashlib.sha256(f.read()).hexdigest()
            if got != e["sha256"]:
                c.find(full, 1, "data", "sha256 differs from MANIFEST.toml")
    for p in files:
        if p not in by_path:
            c.find(p, 1, "data", "no entry in MANIFEST.toml")


def assets(c):
    """Ref: tasks/088, 0009 d21: asset licences and size caps, image size caps."""
    import tomllib
    data_files = [p for p in c.files if p.startswith(("tests/data/", "scenes/"))]
    sizes = {p: os.path.getsize(os.path.join(c.root, p)) for p in c.files}
    if sum(sizes[p] for p in data_files) > ASSET_TOTAL_CAP:
        c.find("tests/data", 1, "assets", f"tests/data + scenes exceed {ASSET_TOTAL_CAP >> 20} MB")
    for p in data_files:
        if sizes[p] > ASSET_FILE_CAP:
            c.find(p, 1, "assets", f"{sizes[p]} bytes, cap {ASSET_FILE_CAP} (2 MB)")
    manifest = "assets/manifest.toml"
    if manifest in c.files:
        try:
            with open(os.path.join(c.root, manifest), "rb") as f:
                entries = tomllib.load(f).get("asset", [])
        except (OSError, ValueError) as e:
            c.find(manifest, 1, "assets", f"cannot parse: {e}")
            entries = []
        for a in entries:
            spdx_id = a.get("spdx", "")
            for p in a.get("committed", []):
                if p not in c.files:
                    c.find(manifest, 1, "assets", f"{a.get('name')}: committed path {p} does not exist")
                elif not ASSET_SPDX_RE.fullmatch(spdx_id):
                    c.find(p, 1, "assets", f"licence {spdx_id!r} of {a.get('name')} may not be committed")
    for p in c.files:
        if not p.endswith(".png") or p.startswith("third_party/"):
            continue
        if sizes[p] > ASSET_FILE_CAP:
            c.find(p, 1, "assets", f"image {sizes[p]} bytes, cap {ASSET_FILE_CAP} (2 MB)")
        with open(os.path.join(c.root, p), "rb") as f:
            head = f.read(24)
        if head[:8] != b"\x89PNG\r\n\x1a\n" or head[12:16] != b"IHDR":
            c.find(p, 1, "assets", "not a valid PNG header")
            continue
        w, h = int.from_bytes(head[16:20], "big"), int.from_bytes(head[20:24], "big")
        if max(w, h) > IMAGE_MAX_PX:
            c.find(p, 1, "assets", f"image {w}x{h}, cap {IMAGE_MAX_PX} px")


# ---- citations section: retrospective R12 (g), ruling 86 ---------------------------
CIT_RULINGS = "docs/decisions/RULINGS.md"
CIT_OWNER = "docs/decisions/0000-owner-decisions.md"
CIT_0009 = "docs/decisions/0009-build-test-process.md"
# A list continues only with short numbers, so "ruling 5, 0004 D1" does not read 0004 as a ruling.
_CONT = r"(?:\s*(?:-|–|,|and|or)\s*R?\d{1,3}(?!\.?\d))*"
CIT_RULING_RE = re.compile(r"\b[Rr]ulings? (\d{1,3}(?!\.?\d)" + _CONT + ")")
CIT_R_RE = re.compile(r"\bR(\d{1,2})(?:-R(\d{1,2}))?\b")
CIT_OWNER_RE = re.compile(r"\b[Oo]wner decisions? (\d{1,3}(?!\.?\d)" + _CONT + ")")
CIT_O_RE = re.compile(r"\bO(\d{1,3})(?:-O(\d{1,3}))?\b")
CIT_B_RE = re.compile(r"\bB-(\d\d)\b")
CIT_RULE_RE = re.compile(r"\bCLAUDE\.md`? rules? (\d+)(?:\s*-\s*(\d+))?")
_DEC = r"(?:[dD]|decisions? |items? )"
CIT_DEC_RE = re.compile(rf"\b(000[1-9]) ({_DEC}\d+[a-z]?(?:(?:-|–|, | and )(?:{_DEC}\d+[a-z]?|\d{{1,2}}[a-z]?(?!\.?\d)))*)")


def _numbered(c, path, head_re, item_re):
    """Item ids of the numbered lines under the first heading matching head_re, up to the next heading of that
    level; head_re None scans the whole file."""
    out, inside, level = set(), head_re is None, None
    for ln in c.lines(path):
        h = head_re and re.match(r"^(#+) ", ln)
        if h and inside and len(h.group(1)) <= level:
            break
        if h and re.match(head_re, ln):
            inside, level = True, len(h.group(1))
            continue
        m = inside and re.match(item_re, ln)
        if m:
            out.add(m.group(1))
    return out


def _range_ids(body):
    """Every number a citation list names: "64-85" expands, "16 and 17" lists."""
    out = []
    for part in re.split(r"\s*(?:,|and|or)\s*", body):
        ends = [int(x) for x in re.findall(r"\d+", part)]
        if len(ends) == 2 and ends[0] <= ends[1]:
            out += list(range(ends[0], ends[1] + 1))
        else:
            out += ends
    return out


def citations(c):
    """Ref: retrospective R12 (g): every ruling, owner decision, budget row, record decision and CLAUDE.md rule
    cited in docs/*.md and docs/decisions/*.md exists, and 0009 d28 names every task-file field."""
    if CIT_RULINGS not in c.files:
        return  # not a records tree
    # STATUS.md is excluded like tasks/: the lead overwrites it, and it proposes rows that open tasks create.
    docs = [p for p in c.files if p.endswith(".md") and re.match(r"^docs/(decisions/)?[^/]+$", p)
            and p != "docs/STATUS.md"]
    rulings = {int(x) for x in _numbered(c, CIT_RULINGS, None, r"^(\d+)\. ")}
    owner = {int(x) for x in _numbered(c, CIT_OWNER, None, r"^(\d+)\. ")} if CIT_OWNER in c.files else set()
    brows = {m.group(1) for ln in (c.lines(CIT_0009) if CIT_0009 in c.files else [])
             for m in [re.match(r"\| B-(\d\d) \|", ln)] if m}
    rules = {int(x) for x in _numbered(c, "CLAUDE.md", r"^## Rules", r"^(\d+)\. ")} if "CLAUDE.md" in c.files else set()
    dec = {}
    for p in c.files:
        m = re.match(r"^docs/decisions/(000[1-9])-[^/]+\.md$", p)
        if m:
            s = dec.setdefault(m.group(1), set())
            s |= _numbered(c, p, r"^## Decision\s*$", r"^\s*(\d+[a-z]?)\. ")
            s |= _numbered(c, p, r"^## Decision\s*$", r"^(?:### |- \*\*|\*\*)D(\d+[a-z]?)\.")
    for p in docs:
        bare_r = p == "docs/ARCHITECTURE.md" or p.startswith("docs/decisions/")
        for n, ln in enumerate(c.lines(p), 1):
            for m in CIT_RULING_RE.finditer(ln):
                for x in _range_ids(m.group(1)):
                    if x not in rulings:
                        c.find(p, n, "citations", f"ruling {x} is not in RULINGS.md")
            if bare_r:
                for m in CIT_R_RE.finditer(ln):
                    for x in _range_ids("-".join(g for g in m.groups() if g)):
                        if x not in rulings:
                            c.find(p, n, "citations", f"R{x} is not in RULINGS.md")
            for m in list(CIT_OWNER_RE.finditer(ln)) + list(CIT_O_RE.finditer(ln)):
                for x in _range_ids("-".join(g for g in m.groups() if g)):
                    if x not in owner:
                        c.find(p, n, "citations", f"owner decision {x} is not in 0000-owner-decisions.md")
            for m in CIT_B_RE.finditer(ln):
                if m.group(1) not in brows:
                    c.find(p, n, "citations", f"B-{m.group(1)} is not a row of the 0009 d13 table")
            for m in CIT_RULE_RE.finditer(ln):
                for x in _range_ids("-".join(g for g in m.groups() if g)):
                    if x not in rules:
                        c.find(p, n, "citations", f"CLAUDE.md has no rule {x}")
            for m in CIT_DEC_RE.finditer(ln):
                for x in re.findall(r"\d+[a-z]?", m.group(2)):
                    if x not in dec.get(m.group(1), set()):
                        c.find(p, n, "citations", f"record {m.group(1)} has no decision {x}")
    d28 = []
    if CIT_0009 in c.files:
        L = c.lines(CIT_0009)
        i = next((k for k, ln in enumerate(L) if ln.startswith("28. ")), None)
        if i is not None:
            j = next((k for k in range(i + 1, len(L)) if re.match(r"^(\d+\. |#)", L[k])), len(L))
            d28 = [ln.strip() for ln in L[i:j]]
    for fld in FIELDS:
        if not any(ln.startswith(fld) for ln in d28):
            c.find(CIT_0009, 1, "citations", f"0009 d28's template lacks the task-file field {fld}")


REFS_FIELDS = ["name", "url", "commit", "sha256", "spdx", "purpose"]
REFS_MENTION_RE = re.compile(r"refs\.toml|_cache[\\/]+refs|DEPS_CACHE.*refs")


def refs(c):
    """Ref: 0009 d6: reference sources are pinned and never compiled, linked or listed as dependencies."""
    import tomllib
    toml_path = "third_party/refs.toml"
    if toml_path not in c.files:
        return
    try:
        with open(os.path.join(c.root, toml_path), "rb") as f:
            entries = tomllib.load(f).get("ref", [])
    except (OSError, ValueError) as e:
        c.find(toml_path, 1, "refs", f"cannot parse: {e}")
        return
    dep_names = set()
    if "third_party/deps.toml" in c.files:
        with open(os.path.join(c.root, "third_party/deps.toml"), "rb") as f:
            dep_names = {d.get("name") for d in tomllib.load(f).get("dep", [])}
    seen = set()
    for r in entries:
        name = r.get("name", "?")
        for k in REFS_FIELDS:
            if not isinstance(r.get(k), str) or not r[k].strip():
                c.find(toml_path, 1, "refs", f"{name}: missing field {k}")
        if not re.fullmatch(r"[0-9a-f]{40}", str(r.get("commit", ""))):
            c.find(toml_path, 1, "refs", f"{name}: commit is not 40 lowercase hex digits")
        if not re.fullmatch(r"[0-9a-f]{64}", str(r.get("sha256", ""))):
            c.find(toml_path, 1, "refs", f"{name}: sha256 is not 64 lowercase hex digits")
        if name in seen:
            c.find(toml_path, 1, "refs", f"{name}: duplicate name")
        seen.add(name)
        if name in dep_names:
            c.find(toml_path, 1, "refs", f"{name}: also a name in deps.toml (references are not dependencies)")
    for p in c.files:
        base = os.path.basename(p)
        build = base == "CMakeLists.txt" or base.endswith(".cmake")
        if not (build or (base.endswith(".ps1") and p != "tools/fetch.ps1")):
            continue
        for i, ln in enumerate(c.lines(p), 1):
            if REFS_MENTION_RE.search(ln):
                c.find(p, i, "refs", "names the reference cache or refs.toml (references are never compiled or linked)")


SECTIONS = {
    "spdx": spdx,
    "nvidia": nvidia,
    "ascii-ps1": ascii_ps1,
    "caps": caps,
    "skip": skip,
    "taskfile": taskfile,
    "budgets": budgets,
    "history": history,
    "deps": deps,
    "kernel": kernel,
    "seam": seam,
    "assets": assets,
    "data": data,
    "flags": flags,
    "citations": citations,
    "refs": refs,
}

def run(root, files, sections):
    c = Ctx(root, files)
    for name in sections:
        SECTIONS[name](c)
    return sorted(c.out)


def repo_files(root):
    r = subprocess.run(["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                       cwd=root, capture_output=True, check=True)
    fs = [f for f in r.stdout.decode().split("\0") if f and not f.startswith(CASES + "/")]
    return [f for f in fs if os.path.isfile(os.path.join(root, f))]


def tree_files(root):
    out = []
    for d, _, names in os.walk(root):
        for n in names:
            rel = os.path.relpath(os.path.join(d, n), root).replace(os.sep, "/")
            if rel != "expected.txt":
                out.append(rel)
    return out


def self_test(repo):
    base = os.path.join(repo, CASES)
    n, bad = 0, 0
    per = {}
    for sec in sorted(os.listdir(base)):
        for case in sorted(os.listdir(os.path.join(base, sec))):
            root = os.path.join(base, sec, case)
            with open(os.path.join(root, "expected.txt"), encoding="utf-8") as f:
                want = sorted(f.read().splitlines())
            got = run(root, tree_files(root), SECTIONS)
            n += 1
            per[sec] = per.get(sec, 0) + 1
            if got != want:
                bad += 1
                print(f"{sec}/{case}: expected {want} got {got}")
    for sec in SECTIONS:
        if per.get(sec, 0) < 2:
            bad += 1
            print(f"section {sec}: {per.get(sec, 0)} cases, need at least 2")
    if bad:
        print(f"self-test: {bad} failures")
        return 1
    print(f"self-test: {n} cases passed")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--section", choices=sorted(SECTIONS))
    a = ap.parse_args()
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if a.self_test:
        return self_test(repo)
    out = run(repo, repo_files(repo), [a.section] if a.section else SECTIONS)
    for ln in out:
        print(ln)
    return 1 if out else 0


if __name__ == "__main__":
    sys.exit(main())
