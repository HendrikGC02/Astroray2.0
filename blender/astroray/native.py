# SPDX-License-Identifier: GPL-3.0-or-later
"""Loads astroray_core.dll by full path and checks it against the staged binding.

Ref: docs/decisions/0007-blender-integration.md d3 (identity check), d44
(OSError poisons the context), d45 (DLL by full path after add_dll_directory).
"""
import ctypes
import os

_lib = None
_poisoned = False


class PoisonedContext(RuntimeError):
    pass


def _bin_dir():
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), "bin")


def _build_line(abi, lib):
    rec = abi.ar_build_record()
    abi.bind(lib, ("ar_build_info",))
    st = lib.ar_build_info(ctypes.byref(rec))
    if st != 0:
        raise abi.AbiMismatch(f"ar_build_info returned status {st}")

    def s(field):
        return bytes(getattr(rec, field)).split(b"\0")[0].decode("utf-8", "replace")

    return (f"Astroray core {s('version')} git {s('git_sha')}{' dirty' if rec.dirty else ''} "
            f"{s('build_type')} cuda {s('cuda')} optix {s('optix')} embree {s('embree')} oidn {s('oidn')}")


def load():
    """Load the DLL and verify hash and layouts; raises AbiMismatch or OSError."""
    global _lib
    if _lib is not None:
        return _lib
    from . import _abi  # staged by the build, never committed

    bin_dir = _bin_dir()
    os.add_dll_directory(bin_dir)
    lib = ctypes.CDLL(os.path.join(bin_dir, "astroray_core.dll"))
    _abi.check(lib)
    print(_build_line(_abi, lib), flush=True)
    _lib = lib
    return lib


def call(fn, *args):
    """Every engine call goes through here: an OSError means a poisoned context."""
    global _poisoned
    if _poisoned:
        raise PoisonedContext("engine context is poisoned; restart Blender")
    try:
        return fn(*args)
    except OSError:
        _poisoned = True
        raise


def poisoned():
    return _poisoned


def devices():
    """Devices offered to the user; only the CPU once the context is poisoned."""
    return ["CPU"] if _poisoned else ["CPU", "CUDA"]
