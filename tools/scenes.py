# SPDX-License-Identifier: Apache-2.0
"""Regenerates every scene recording: scenes/<name>/build.py -> out/scenes/<name>.arrec (0009 d19).

Usage: <python> -I tools/scenes.py [--force] [name ...]
A default run skips scenes whose meta.toml has build.on_demand = true and prints one line naming them;
naming a scene builds it.
A recording is rebuilt only when the hash of its sources changed: the scene's own files, the
shared scenes/lib code, and the generated binding and slot table it is written against. Recordings
are never committed. Each scene's meta.toml is validated first; any failure exits non-zero.
A recording and its sidecars are a function of that hash alone, so a machine-wide cache keyed by name and hash
(ASTRORAY_SCENES_CACHE, default %LOCALAPPDATA%/astroray2/scenes-cache; none when neither exists)
lets a fresh worktree copy what another tree already built instead of rebuilding it.
Ref: docs/decisions/0009-build-test-process.md d19, d20.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCENES = os.path.join(ROOT, "scenes")
OUT = os.path.join(ROOT, "out", "scenes")
GEN = os.path.join(ROOT, "out", "build", "gen")
SHARED = [os.path.join(GEN, "_abi.py"), os.path.join(GEN, "_ops.py"),
          os.path.join(ROOT, "src", "kernel", "vm", "ops", "store_slot.h")]


def cache_dir():
    """The shared recording cache, or None when this machine has no place for one (hosted CI)."""
    d = os.environ.get("ASTRORAY_SCENES_CACHE")
    if not d and os.environ.get("LOCALAPPDATA"):
        d = os.path.join(os.environ["LOCALAPPDATA"], "astroray2", "scenes-cache")
    return d


def store_in_cache(entry, rec):
    """Copies the recording and every file the builder wrote beside it (rec.*, except the stamp) into a
    temporary folder and renames it into place, so a reader never sees half an entry; `.done` is its mark."""
    tmp = f"{entry}.tmp{os.getpid()}"
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    base = os.path.basename(rec)
    for f in os.listdir(os.path.dirname(rec)):
        if (f == base or f.startswith(base + ".")) and not f.endswith(".src"):
            shutil.copyfile(os.path.join(os.path.dirname(rec), f), os.path.join(tmp, f))
    open(os.path.join(tmp, ".done"), "w").close()
    try:
        os.rename(tmp, entry)
    except OSError:  # another tree stored the same entry first; its copy is identical by construction
        shutil.rmtree(tmp, ignore_errors=True)


sys.path.insert(0, os.path.join(SCENES, "lib"))
import meta  # noqa: E402


def scene_names():
    return sorted(d for d in os.listdir(SCENES)
                  if os.path.isfile(os.path.join(SCENES, d, "build.py")))


def source_hash(name, sources=()):
    """SHA-256 over the file names and bytes of everything the recording is a function of: the
    scene's folder, scenes/lib, and the folders its meta.toml lists under build.sources."""
    files = list(SHARED)
    for d in (os.path.join(SCENES, name), os.path.join(SCENES, "lib"), *(os.path.join(ROOT, x) for x in sources)):
        files += sorted(os.path.join(d, f) for f in os.listdir(d) if f.endswith((".py", ".toml")))
    h = hashlib.sha256()
    for p in files:
        if not os.path.exists(p):
            raise RuntimeError(f"{p} is missing: run tools/build.ps1 first")
        h.update(os.path.relpath(p, ROOT).replace("\\", "/").encode() + b"\0")
        with open(p, "rb") as f:
            h.update(f.read())
    return h.hexdigest()


def _counts(rec):
    """The `counts:` line a builder printed (triangles, instances), or nothing."""
    try:
        with open(rec + ".counts", encoding="ascii") as f:
            text = f.read().strip()
    except OSError:
        return ""
    return f"; counts: {text}" if text else ""


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("names", nargs="*", help="scenes to build (default: all)")
    ap.add_argument("--force", action="store_true", help="rebuild even when up to date")
    args = ap.parse_args()
    names = args.names or scene_names()
    if not args.names:
        # On-demand scenes (fetched assets, large textures) are built only when named; say so, never skip silently.
        skipped = [n for n in names if meta.load(os.path.join(SCENES, n, "meta.toml"))["build"]["on_demand"]]
        names = [n for n in names if n not in skipped]
        if skipped:
            print("scenes: on demand, not built: " + ", ".join(skipped))
    unknown = [n for n in names if n not in scene_names()]
    if unknown:
        print(f"FAIL scenes: no builder for {unknown}")
        return 2
    os.makedirs(OUT, exist_ok=True)
    t0 = time.perf_counter()
    built = current = cached = 0
    cache = cache_dir()
    for name in names:
        try:
            m = meta.load(os.path.join(SCENES, name, "meta.toml"))
            digest = source_hash(name, m["build"]["sources"])
        except (meta.MetaError, OSError, RuntimeError) as e:
            print(f"FAIL scenes: {name}: {e}")
            return 1
        rec = os.path.join(OUT, name + ".arrec")
        stamp = rec + ".src"
        if not args.force and os.path.exists(rec) and os.path.exists(stamp):
            with open(stamp, encoding="ascii") as f:
                if f.read().strip() == digest:
                    current += 1
                    print(f"scene {name}: up to date" + _counts(rec))
                    continue
        hit = os.path.join(cache, f"{name}-{digest[:32]}") if cache else None
        if hit and not args.force and os.path.exists(os.path.join(hit, ".done")):
            for f in os.listdir(hit):
                if f != ".done":
                    shutil.copyfile(os.path.join(hit, f), os.path.join(OUT, f))
            with open(stamp, "w", encoding="ascii") as f:
                f.write(digest + "\n")
            cached += 1
            print(f"scene {name}: from cache" + _counts(rec))
            continue
        t1 = time.perf_counter()
        r = subprocess.run([sys.executable, "-I", os.path.join(SCENES, name, "build.py"), "--out", rec],
                           capture_output=True, text=True, timeout=300)
        if r.returncode != 0:
            tail = (r.stdout + r.stderr).strip().splitlines()[-8:]
            print(f"FAIL scenes: {name} build.py exited {r.returncode}")
            print("\n".join(tail))
            return 1
        with open(stamp, "w", encoding="ascii") as f:
            f.write(digest + "\n")
        with open(rec + ".counts", "w", encoding="ascii") as f:  # a builder's own size line, kept for up-to-date runs
            f.write("".join(ln[len("counts:"):].strip() for ln in r.stdout.splitlines() if ln.startswith("counts:")))
        if hit:
            store_in_cache(hit, rec)
        built += 1
        print(f"scene {name}: built {os.path.getsize(rec)} bytes in {time.perf_counter() - t1:.2f} s" + _counts(rec))
    print(f"scenes: {built} built, {cached} from cache, {current} up to date, {time.perf_counter() - t0:.2f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
