# SPDX-License-Identifier: GPL-3.0-or-later
"""Context manager over the ar_edit functions of _abi.py (0007 d6, d9, d16).

The wrapper takes the generated binding module and the loaded library, so it holds no global
state and can be built over a stub in headless tests.
"""
import ctypes

import numpy as np

AR_OK = 0


class EditError(RuntimeError):
    pass


class Edit:
    def __init__(self, abi, lib, scene):
        self._abi, self._lib, self._scene = abi, lib, scene
        self._edit = None
        self._views = []
        self.generation = None

    def _call(self, name, *args):
        fn = getattr(self._lib, name)
        st = fn(*args)
        if st != AR_OK:
            raise EditError(f"{name} returned status {st}{self._last_error()}")

    def _last_error(self):
        if self._edit is None:
            return ""
        buf = (ctypes.c_uint8 * 512)()
        fn = self._lib.ar_edit_last_error
        if fn(self._edit, 512, buf) != AR_OK:
            return ""
        return ": " + bytes(buf).split(b"\0")[0].decode("utf-8", "replace")

    def __enter__(self):
        self._abi.bind(self._lib, ("ar_scene_edit_begin", "ar_edit_item_commit", "ar_edit_commit",
                                   "ar_edit_abort", "ar_edit_last_error"))
        out = ctypes.c_void_p()
        self._call("ar_scene_edit_begin", self._scene, ctypes.byref(out))
        self._edit = out
        return self

    def __exit__(self, exc_type, exc, tb):
        self._drop_views()
        try:
            if exc_type is not None:
                self._call("ar_edit_abort", self._edit)  # a bad edit keeps the previous image
            else:
                gen = ctypes.c_uint64()
                self._call("ar_edit_commit", self._edit, ctypes.byref(gen))
                self.generation = gen.value
        finally:
            self._edit = None
        return False

    @property
    def handle(self):
        return self._edit

    def call(self, name, *args):
        """Any other ar_edit_* function; the first argument is the edit handle."""
        self._abi.bind(self._lib, (name,))
        self._call(name, self._edit, *args)

    def staged(self, array_ptr, dtype, shape):
        """A NumPy view over an engine-owned staged array; invalid after item_commit."""
        n = int(np.prod(shape))
        ptr = ctypes.cast(array_ptr, ctypes.POINTER(np.ctypeslib.as_ctypes_type(dtype)))
        view = np.ctypeslib.as_array(ptr, shape=(n,)).reshape(shape)
        self._views.append(view)
        return view

    def item_commit(self, key):
        self._drop_views()
        self.call("ar_edit_item_commit", key)

    def _drop_views(self):
        # The engine owns the memory and may move or free it at commit; make late writes fail.
        for v in self._views:
            v.flags.writeable = False
        self._views.clear()
