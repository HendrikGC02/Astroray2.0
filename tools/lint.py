# SPDX-License-Identifier: Apache-2.0
"""Repository lint: licences, NVIDIA material, ASCII scripts, process caps, skips, task files.

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


def nvidia(c):
    pats = ["optix*.h", "nvsdk_ngx*", "nvrtc*", "*.dll", "*.lib", "*.cubin", "*.ptx"]
    for p in c.files:
        base = os.path.basename(p).lower()
        if any(fnmatch.fnmatch(base, q) for q in pats):
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


SECTIONS = {"spdx": spdx, "nvidia": nvidia, "ascii-ps1": ascii_ps1, "caps": caps,
            "skip": skip, "taskfile": taskfile, "budgets": budgets, "history": history}


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
