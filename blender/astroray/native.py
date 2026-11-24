# SPDX-License-Identifier: GPL-3.0-or-later
"""Loads astroray_core.dll by full path and checks it against the staged binding.

Ref: docs/decisions/0007-blender-integration.md d3 (identity check), d44
(OSError poisons the context), d45 (DLL by full path after add_dll_directory).
"""
import ctypes
import os

_lib = None
_poisoned = False
_build_text = None  # the build record line, read once by load()
_cuda_lost = None  # the message shown when a sticky CUDA error ended CUDA for this process

# 0001 D2.5: a sticky CUDA error reaches the host as "GPU error: <text>. Save your work and restart
# Blender." and every CUDA call afterwards fails with AR_ERR_DEVICE; the CPU device keeps working.
GPU_ERROR = "GPU error:"
RESTART = "Save your work and restart Blender."


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
    global _build_text
    _build_text = _build_line(_abi, lib)
    print(_build_text, flush=True)
    _lib = lib
    return lib


def build_line():
    """The build record line load() read (version, git, build type, dependency versions); None before load()."""
    return _build_text


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
    """Devices offered to the user; only the CPU once the context is poisoned or CUDA is lost."""
    return ["CPU"] if _poisoned or _cuda_lost else ["CPU", "CUDA"]


def cuda_lost():
    """The message of the sticky CUDA error, or None."""
    return _cuda_lost


def note_failure(lib, abi, failure, session=None):
    """Called with the text of a failed engine job. A GPU error (0001 D2.5) ends CUDA for the process:
    returns the message to show (the engine's full text with the status name), after which devices()
    offers only the CPU. Returns None for any other failure. `session` is the failed session handle, if
    it still exists: the report entry's detail is cut at 127 characters, its last-error text is not."""
    global _cuda_lost
    # Matching the text is sound because 0001 D2.5 fixes it: "GPU error: <text>. Save your work and restart Blender."
    if GPU_ERROR not in failure and RESTART not in failure:
        return None
    text, status = failure, ""
    if session is not None and session.value:
        if lib.ar_session_start(session) == abi.AR_ERR_DEVICE:  # a failed session answers every call so
            status = "AR_ERR_DEVICE: "
        buf = (ctypes.c_uint8 * 512)()
        if lib.ar_session_last_error(session, 512, buf) == 0 and buf[0]:
            text = bytes(buf).split(b"\0")[0].decode("utf-8", "replace")
    if _cuda_lost is None:
        _cuda_lost = (f"astroray: {status}{text} CUDA is unavailable until Blender restarts; "
                      "only the CPU device is offered.")
    return _cuda_lost
