# SPDX-License-Identifier: Apache-2.0
"""Loads and validates a scene's meta.toml (0009 d20).

Fields: class (A, D, O, B, G), truth (a table: `formula` names the closed form in
scenes/lib/truths.py, the other keys are its parameters; the builder reads its scene values from
here so the truth and the scene cannot drift apart), tolerance, eps_sys (default 0), spectra,
units ("m"), assets, single_witness, build (optional table: `on_demand` = true keeps the scene out
of a default tools/scenes.py run, for scenes that need fetched assets or are big; `sources` lists repository
folders outside the scene's own whose .py and .toml files the builder imports, hashed with it).
`tolerance` is a table naming the rule, never a number chosen per scene: kind = "z_se" is
|mean - truth| <= z * SE + eps_sys with z from the analytic family (0009 d15, d16); kind =
"oracle" is the per-pixel Sidak test of d16; kind = "exact" is for deterministic classes.
Ref: docs/decisions/0009-build-test-process.md d16, d20.
"""
import os
import tomllib

CLASSES = ("A", "D", "O", "B", "G")
TOLERANCE_KINDS = ("z_se", "oracle", "exact")
SPECTRA = ("rgb", "blackbody", "table", "none")
KEYS = {"class", "truth", "tolerance", "eps_sys", "spectra", "units", "assets", "single_witness", "build"}
REQUIRED = {"class", "units", "spectra"}


class MetaError(ValueError):
    pass


def load(path):
    """Returns the validated meta.toml at `path` as a dict (eps_sys, assets and single_witness
    filled with their defaults)."""
    with open(path, "rb") as f:
        m = tomllib.load(f)
    where = os.path.relpath(path)
    if unknown := set(m) - KEYS:
        raise MetaError(f"{where}: unknown keys {sorted(unknown)}")
    if missing := REQUIRED - set(m):
        raise MetaError(f"{where}: missing keys {sorted(missing)}")
    if m["class"] not in CLASSES:
        raise MetaError(f"{where}: class must be one of {CLASSES}, got {m['class']!r}")
    if m["units"] != "m":
        raise MetaError(f"{where}: units must be \"m\" (0003 d9), got {m['units']!r}")
    spectra = m["spectra"]
    if not isinstance(spectra, list) or not spectra or any(s not in SPECTRA for s in spectra):
        raise MetaError(f"{where}: spectra must be a non-empty list drawn from {SPECTRA}")
    if m["class"] in ("A", "O"):
        truth = m.get("truth")
        if not isinstance(truth, dict) or not isinstance(truth.get("formula"), str):
            raise MetaError(f"{where}: class {m['class']} needs a truth table with a `formula` name")
        if "tolerance" not in m:
            raise MetaError(f"{where}: class {m['class']} needs a tolerance table")
    if "tolerance" in m:
        tol = m["tolerance"]
        if not isinstance(tol, dict) or tol.get("kind") not in TOLERANCE_KINDS:
            raise MetaError(f"{where}: tolerance.kind must be one of {TOLERANCE_KINDS}")
        if m["class"] == "A" and tol["kind"] != "z_se":
            raise MetaError(f"{where}: analytic scenes use tolerance.kind = \"z_se\" (0009 d16)")
    eps = m.get("eps_sys", 0.0)
    if isinstance(eps, bool) or not isinstance(eps, (int, float)) or eps < 0:
        raise MetaError(f"{where}: eps_sys must be a number >= 0")
    m["eps_sys"] = float(eps)
    assets = m.get("assets", [])
    if not isinstance(assets, list) or any(not isinstance(a, str) for a in assets):
        raise MetaError(f"{where}: assets must be a list of manifest names")
    m["assets"] = assets
    sw = m.get("single_witness", False)
    if not isinstance(sw, bool):
        raise MetaError(f"{where}: single_witness must be true or false")
    m["single_witness"] = sw
    b = m.get("build", {})
    if not isinstance(b, dict) or set(b) - {"on_demand", "sources"}:
        raise MetaError(f"{where}: build is a table with only on_demand and sources")
    if not isinstance(b.get("on_demand", False), bool):
        raise MetaError(f"{where}: build.on_demand must be true or false")
    src = b.get("sources", [])
    if not isinstance(src, list) or any(not isinstance(x, str) or os.path.isabs(x) or ".." in x.split("/") for x in src):
        raise MetaError(f"{where}: build.sources is a list of repository-relative folders")
    m["build"] = {"on_demand": b.get("on_demand", False), "sources": src}
    return m


def scene_dir_meta(scene_dir):
    return load(os.path.join(scene_dir, "meta.toml"))
