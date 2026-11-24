# SPDX-License-Identifier: GPL-3.0-or-later
"""F12 rendering: one engine scene per job, one session per render() (0007 d23, d27, d28, d48).

Ref: docs/decisions/0007-blender-integration.md d23 (update() and render() run on the F12 job
thread), d27 (render() loops on ar_session_wait, posts previews, maps test_break() to
ar_session_cancel and returns the partial result), d28 (nothing shared between engine instances),
d48 (per view layer: ar_edit_reset, then a full sync with full reads); 0008 d13a and rulings 74-75
(enumerate with ar_session_pass_count before reading a pass); Cycles intern/cycles/blender/session.cpp
(Apache-2.0), BlenderSession::render and ::reset_session, for the update/render split.
The timing constants are 0009 B-37's: 100 ms wait timeout, 1 s preview interval.
"""
import contextlib
import ctypes
import os
import time
import weakref

import numpy as np

from . import diagnostics, native
from .materials import translate
from .sync import camera, classify, edit as edit_mod, images, instances, keys, lights, mesh, settings
from .sync import world as world_sync
from .sync.cache import SyncCache

WAIT_TIMEOUT_MS = 100      # 0009 B-37: F12 wait timeout
PREVIEW_INTERVAL_S = 1.0   # 0009 B-37: F12 preview interval

DEVICES = {"CPU": "AR_DEVICE_CPU", "CUDA": "AR_DEVICE_CUDA"}
# View layer pass flag -> engine pass name (0008 d10). An engine that cannot produce one reports
# PASS_UNSUPPORTED (ruling 74), so enabling a pass the film lacks is never silent.
VIEW_LAYER_PASSES = (("use_pass_z", "Depth"), ("use_pass_position", "Position"),
                     ("use_pass_normal", "Normal"), ("use_pass_uv", "UV"),
                     ("use_pass_object_index", "IndexOB"), ("use_pass_material_index", "IndexMA"),
                     ("use_pass_mist", "Mist"), ("use_pass_vector", "Vector"))

_contexts = {}  # device name -> _Context; one per device for the extension's lifetime (0007 d4)


class _Context:
    """An ar_context, the recording path it was created with, and the jobs whose scenes live on it
    (weakly: an engine instance owns its job). shutdown() walks contexts, then jobs."""

    def __init__(self, handle, record):
        self.handle, self.record = handle, record
        self.jobs = weakref.WeakSet()


class RenderFailed(RuntimeError):
    pass


class Timeline:
    """Wall-clock marks and per-phase seconds of one job, for the first-frame phases (0007 d35 V7b) and the
    translation cost (0005 M-8). One per Job, so no process-global state. With ASTRORAY_TTFF_LOG=1 every
    mark is also a log line `astroray ttff <label> <epoch seconds>`."""

    def __init__(self):
        self.log = os.environ.get("ASTRORAY_TTFF_LOG") == "1"
        self.marks = {}      # label -> first epoch time it was marked
        self.phase_s = {}    # phase -> seconds, summed over the job's syncs
        self.materials = []  # per translated material: (inline_s, walk_s, send_s, op nodes)

    def mark(self, label):
        """Records the first time `label` is reached; later calls do nothing."""
        if label not in self.marks:
            self.marks[label] = time.time()
            if self.log:
                print(f"astroray ttff {label} {self.marks[label]:.6f}", flush=True)

    @contextlib.contextmanager
    def phase(self, name):
        t0 = time.perf_counter()
        try:
            yield
        finally:
            self.phase_s[name] = self.phase_s.get(name, 0.0) + time.perf_counter() - t0


def _abi():
    from . import _abi  # staged by the build, never committed
    return _abi


def _ops():
    from . import _ops  # staged by the build, never committed
    return _ops


def device_of(scene):
    """The F12 device: scene.astroray.device (an enum, so always CPU or CUDA)."""
    name = scene.astroray.device
    if name not in native.devices():
        raise RenderFailed(f"astroray: device {name} is unavailable (the engine context is poisoned)")
    return name


def _user_dir():
    import bpy
    return bpy.utils.extension_path_user(__package__, path="", create=True)


def _log_path():
    return os.path.join(_user_dir(), "astroray_context.log")


def cache_dir():
    """The OptiX disk cache directory (0007 d45): the extension's user directory, so a second Blender
    start reuses the compiled modules instead of paying the cold compile (0009 B-18 warm, not cold).
    Named through the context log's directory, so a harness that loads this module outside an installed
    extension redirects both by replacing _log_path (extension_path_user raises for a non-extension package)."""
    return os.path.dirname(_log_path())


def _ok(lib, status, what, last_error=None, handle=None):
    """Raises with the engine's last-error text when `status` is not AR_OK. `last_error` names the
    ar_<object>_last_error function that goes with `handle`."""
    if status == 0:
        return
    text = ""
    if last_error is not None:
        buf = (ctypes.c_uint8 * 512)()
        if getattr(lib, last_error)(handle, 512, buf) == 0 and buf[0]:
            text = ": " + bytes(buf).split(b"\0")[0].decode("utf-8", "replace")
    raise RenderFailed(f"astroray: {what} returned status {status}{text}")


def context(device):
    """The _Context of `device`, created on first use, recording when ASTRORAY_RECORD is set (0007 d4).
    The path is read here and passed in the descriptor: the engine DLL has its own C runtime, which
    does not see variables that Python set after the process started."""
    record = os.environ.get("ASTRORAY_RECORD")
    held = _contexts.get(device)
    if held is not None:
        return held
    lib, abi = native.load(), _abi()
    abi.bind(lib)
    ctx = ctypes.c_void_p()
    desc = abi.ar_context_desc(getattr(abi, DEVICES[device]), 0, _log_path().encode(),
                               record.encode() if record else None)
    _ok(lib, native.call(lib.ar_context_create, ctypes.byref(desc), ctypes.byref(ctx)),
        "ar_context_create")
    _contexts[device] = _Context(ctx, record)
    return _contexts[device]


def record_warning(device):
    """A visible line when ASTRORAY_RECORD no longer matches what the device's context was created
    with: the recorder opens with the context, so a later change cannot take effect."""
    held = _contexts.get(device)
    now = os.environ.get("ASTRORAY_RECORD")
    if held is not None and held.record != now:
        return (f"ASTRORAY_RECORD is {now!r} but this device's context was created with {held.record!r}; "
                "the recording does not follow (restart Blender to change it)")
    return None


def shutdown():
    """Ends every live job (session, then scene: 0007 d28), then destroys the contexts (a recording
    is complete only after this, 0001 D1.4). A later Job.close or __del__ finds nothing to destroy."""
    lib = native.load()
    for held in _contexts.values():
        for job in list(held.jobs):
            job.close()
    for dev in list(_contexts):
        native.call(lib.ar_context_destroy, _contexts.pop(dev).handle)


def break_requested(engine):
    """Blender's test_break(); a module function so a test can stand in for the user's Escape (a
    Python subclass cannot override the engine's own RNA method)."""
    return engine.test_break()


def view_layer_passes(view_layer):
    return tuple(name for flag, name in VIEW_LAYER_PASSES if getattr(view_layer, flag))


def render_size(scene):
    r = scene.render
    return (max(1, int(r.resolution_x * r.resolution_percentage / 100)),
            max(1, int(r.resolution_y * r.resolution_percentage / 100)))


def _held_object(inst, depsgraph):
    """The object a later mesh read may hold. An instanced object is a temporary copy that dies when the
    iteration moves on, so its evaluated original is held instead; an original that is not in the depsgraph
    comes back unevaluated, and reading its mesh would silently send the wrong geometry."""
    if not inst.is_instance:
        return inst.object
    orig = inst.object.original
    ev = orig.evaluated_get(depsgraph)
    if ev.as_pointer() == orig.as_pointer():
        raise RuntimeError(f"astroray sync: instanced object {orig.name!r} has no evaluated copy in the depsgraph")
    return ev


def scan(depsgraph, keymap):
    """(geometry key -> evaluated object, material uid -> material) of the instances in a depsgraph."""
    geoms, mats = {}, {}
    for inst in depsgraph.object_instances:
        if inst.object.type not in instances.GEOMETRY_TYPES:
            continue
        gid = keys.geometry_identity(inst, depsgraph)
        if gid is not None:
            geoms.setdefault(keymap.key(gid), _held_object(inst, depsgraph))
        for slot in inst.object.material_slots:
            if slot.material is not None:
                mats.setdefault(slot.material.original.session_uid, slot.material)
    return geoms, mats


def full_sync(edit, abi, op_id, depsgraph, scene, view_layer, keymap, tcache, istate, need_camera=True, tl=None,
              checks=None, icache=None):
    """Everything of one view layer, in 0007 d20's apply order with full reads. Returns (report
    lines for what Blender asks for and the engine does not do, the camera fields). The viewport
    passes need_camera=False: its view comes from the region (set_view), so a scene without a camera
    syncs, and the camera fields are None; F12 raises without one.
    `tl` (a Timeline) receives the seconds of each step and the cost of each material. `checks`
    (mesh.LayoutChecks) and `icache` (instances.InstanceCache) are the Job's per-key caches."""
    tl = tl or Timeline()
    lightgroups = instances.lightgroup_indices(view_layer)
    with tl.phase("settings_world"):
        fields, lines = settings.build(scene, _colorspace(), view_layer_passes(view_layer), lightgroups)
        settings.set_settings(edit, abi, fields)
        world = scene.world
        world_sync.sync(edit, abi, world.evaluated_get(depsgraph) if world else None, keymap, op_id)

    with tl.phase("scan"):
        geoms, mats = scan(depsgraph, keymap)
    # Images after the world and before the materials (0007 d20): a TEX_IMAGE holds only the image's key.
    with tl.phase("images"):
        trees = [m.original.evaluated_get(depsgraph).node_tree for m in mats.values()]
        if world and world.node_tree:
            trees.append(world.node_tree)
        lines.extend(images.sync(edit, abi, keymap, images.used_images(trees), istate))
    with tl.phase("materials"):
        for uid, mat in mats.items():
            t0 = time.perf_counter()
            em = tcache.get(mat.original.evaluated_get(depsgraph), keymap, op_id)
            t1 = time.perf_counter()
            translate.set_material(edit, abi, keymap, uid, em)
            t2 = time.perf_counter()
            tl.materials.append((em.inline_s, t1 - t0 - em.inline_s, t2 - t1, len(em.nodes)))
    with tl.phase("lights"):
        lines.extend(lights.sync(edit, abi, depsgraph, keymap, lightgroups=lightgroups))
    with tl.phase("meshes"):
        for key, ob in geoms.items():
            mesh.sync_object(edit, abi, ob, key, checks)
    with tl.phase("instances"):
        instances.set_instances(edit, abi, instances.build(depsgraph, keymap, lightgroups=lightgroups, cache=icache))
    if not need_camera:
        return lines, None
    cam, cam_lines = camera.final(depsgraph, scene)
    camera.set_camera(edit, abi, cam)
    return lines + cam_lines, cam


def _colorspace():
    import bpy
    return bpy.data.colorspace


class Job:
    """One engine instance's scene; update() resyncs it, render() runs a session on it."""

    kind = "FINAL"                # which Diagnostics report this job writes
    snapshot_replaces = False     # True: each new report revision replaces the engine's entries (the viewport)

    def __init__(self):
        self.lib, self.abi, self.op_id = native.load(), _abi(), _ops().OP_ID
        self.device = None
        self.scene_h = ctypes.c_void_p()
        self.keymap, self.cache, self.tcache = keys.KeyMap(), SyncCache(), translate.TranslationCache()
        self.istate = images.ImageState()
        self.icache = instances.InstanceCache()  # per-object instance work, for self.keymap
        self.checks = mesh.LayoutChecks()        # geometry keys whose direct read passed the layout check
        self.camera = None
        self.lines = []
        self.session = ctypes.c_void_p()
        self.first = True
        # Everything render() needs from Blender, read by update() (0007 d23: render() reads no scene).
        self.size, self.samples, self.layer = None, None, None
        self.generation = None  # what update() committed: the render must show at least this (0007 d25)
        self.subject_names = ({}, {})  # (ar_key -> identity, uid -> name) that label report subjects; set by update()
        self.subject_names = ({}, {})  # (ar_key -> identity, uid -> name) for the Diagnostics panel; set by update()
        self._context = None
        self.timeline = Timeline()

    def update(self, depsgraph, need_camera=True):
        """ar_edit_reset, then a full sync of the depsgraph's view layer (0007 d48)."""
        lib, abi = self.lib, self.abi
        scene, view_layer = depsgraph.scene, depsgraph.view_layer
        self.device = device_of(scene)
        warning = record_warning(self.device)
        held = context(self.device)
        if self._context is not None and self._context is not held:
            raise RenderFailed("astroray: the device changed between updates of one render; start it again")
        self._context = held
        held.jobs.add(self)
        ctx = held.handle
        self.size, self.samples, self.layer = render_size(scene), scene.astroray.samples, view_layer.name
        if not self.scene_h.value:
            workers = max(1, min(8, (os.cpu_count() or 2) // 2))
            desc = abi.ar_scene_desc(cache_dir().encode(), workers, 0)
            _ok(lib, native.call(lib.ar_scene_create, ctx, ctypes.byref(desc), ctypes.byref(self.scene_h)),
                "ar_scene_create", "ar_context_last_error", ctx)
        if not self.first:  # later updates of one engine instance: drop what Blender changed
            actions, _ = instances.classify_updates(depsgraph, self.cache, self.icache)
            self.tcache.invalidate([a for a in actions if a.kind == classify.MATERIALS])
            self.istate.note_updates(actions, classify.IMAGES)
        self.timeline.mark("edit_begin")
        with edit_mod.Edit(abi, lib, self.scene_h) as e:
            e.call("ar_edit_reset")
            self.lines, self.camera = full_sync(e, abi, self.op_id, depsgraph, scene, view_layer,
                                                self.keymap, self.tcache, self.istate, need_camera, self.timeline,
                                                self.checks, self.icache)
        self.generation = e.generation
        self.subject_names = (diagnostics.reverse_keys(self.keymap), diagnostics.uid_names())
        self.timeline.mark("edit_commit")
        self.cache.remember(depsgraph, scene)
        if warning:
            self.lines.append(warning)
        self.first = False

    def render(self, engine):
        """Runs a FINAL session to completion; Combined goes into the RenderResult. Uses only what
        update() captured."""
        lib, abi = self.lib, self.abi
        if self.size is None or not self.scene_h.value:
            raise RenderFailed("astroray: render() before a successful update()")
        w, h = self.size
        diagnostics.begin(self.kind, self.device)
        for line in self.lines:
            engine.report({"WARNING"}, line)
            diagnostics.add_host(self.kind, diagnostics.WARNING, line)
        try:
            sd = abi.ar_session_desc(abi.AR_SESSION_MODE_FINAL, w, h, abi.AR_DENOISER_NONE)
            _ok(lib, native.call(lib.ar_session_create, self.scene_h, ctypes.byref(sd),
                                 ctypes.byref(self.session)),
                "ar_session_create", "ar_scene_last_error", self.scene_h)
            view = camera.view_desc(abi, self.camera, w, h)
            _ok(lib, native.call(lib.ar_session_set_view, self.session, ctypes.byref(view)),
                "ar_session_set_view", "ar_session_last_error", self.session)
            _ok(lib, native.call(lib.ar_session_start, self.session), "ar_session_start",
                "ar_session_last_error", self.session)
        except BaseException:
            self._close_session()  # a half-built session is destroyed at once
            raise
        result = engine.begin_result(0, 0, w, h, layer=self.layer)
        try:
            self._wait(engine, result, w, h, self.samples)
        finally:
            engine.end_result(result)
            self._close_session()

    def _wait(self, engine, result, w, h, samples):
        lib, abi, s = self.lib, self.abi, self.session
        prog, reported, revision = abi.ar_progress(), set(), None
        last_preview, cancelled = time.monotonic(), False
        while True:
            native.call(lib.ar_session_wait, s, WAIT_TIMEOUT_MS)
            native.call(lib.ar_session_get_progress, s, ctypes.byref(prog))
            revision = self._report(engine, reported, revision)
            engine.update_progress(min(1.0, prog.samples_per_pixel / max(1, samples)))
            engine.update_stats("", f"Sample {prog.samples_per_pixel}/{samples}")
            if prog.state in (abi.AR_STATE_CONVERGED, abi.AR_STATE_FAILED, abi.AR_STATE_CLOSED):
                break
            if break_requested(engine) and not cancelled:
                native.call(lib.ar_session_cancel, s)
                cancelled = True
            if cancelled and prog.state in (abi.AR_STATE_PAUSED, abi.AR_STATE_TRIMMED):
                break
            if time.monotonic() - last_preview >= PREVIEW_INTERVAL_S:
                last_preview = time.monotonic()
                self._copy_combined(engine, result, w, h)
        revision = self._report(engine, reported, None)
        if prog.state == abi.AR_STATE_FAILED:
            _ok(lib, 1, "the session (state FAILED)", "ar_session_last_error", s)
        if prog.state == abi.AR_STATE_CONVERGED and prog.active_generation < self.generation:
            # Rule 5: an image of an older scene than update() committed is a failure, never a result.
            raise RenderFailed(f"astroray: the render converged on generation {prog.active_generation}, but update() "
                               f"committed {self.generation} (failed generation {prog.failed_generation})")
        self._copy_combined(engine, result, w, h, final=True)

    def _subjects(self):
        """(reverse key map, uid names) that label a report entry's subject."""
        return self.subject_names

    def _subjects(self):
        """(reverse key map, uid names) that label a report entry's subject."""
        return self.subject_names

    def _report(self, engine, reported, revision):
        """Prints entries the engine added since `revision`; ERROR entries fail the render."""
        lib, abi, s = self.lib, self.abi, self.session
        info = abi.ar_report_info()
        native.call(lib.ar_session_report_info, s, ctypes.byref(info))
        if revision is not None and info.revision == revision:
            return revision
        names = {getattr(abi, n): n[len("AR_REPORT_"):] for n in dir(abi) if n.startswith("AR_REPORT_")}
        failed = None
        if self.snapshot_replaces:
            diagnostics.clear_engine(self.kind)
        for i in range(info.entry_count):
            e = abi.ar_report_entry()
            native.call(lib.ar_session_report_entry, s, i, ctypes.byref(e))
            detail = bytes(e.detail).split(b"\0")[0].decode("utf-8", "replace")
            level = {abi.AR_LEVEL_INFO: "INFO", abi.AR_LEVEL_WARNING: "WARNING"}.get(e.level, "ERROR")
            # Listed before the `reported` test: the store keeps each key once itself, and the panel must show
            # entries the status bar already printed.
            diagnostics.add_engine(self.kind, level, names.get(e.code, str(e.code)), e.code,
                                   diagnostics.subject_label(*self._subjects(), e.subject), detail)
            ident = (e.code, e.subject, detail)
            if ident in reported:
                continue
            reported.add(ident)
            text = f"{names.get(e.code, e.code)}: {detail}"
            engine.report({level}, text)
            diagnostics.add_engine(self.kind, level, names.get(e.code, str(e.code)), e.code,
                                   diagnostics.subject_label(*self._subjects(), e.subject), detail)
            if level == "ERROR" and failed is None:
                failed = text
        if failed is not None:
            raise RenderFailed("astroray: " + failed)
        return info.revision

    def _copy_combined(self, engine, result, w, h, final=False):
        """Combined (MEAN, rows bottom-up, 3 components) into the RenderResult's RGBA pass."""
        rgb = read_pass(self.lib, self.abi, self.session, "Combined", self.abi.AR_QUANTITY_MEAN)
        rgba = np.ones((h, w, 4), np.float32)
        rgba[:, :, :3] = rgb
        result.layers[0].passes["Combined"].rect.foreach_set(rgba.reshape(-1))
        if not final:
            engine.update_result(result)

    def _close_session(self):
        if self.session.value:
            native.call(self.lib.ar_session_destroy, self.session)
            self.session = ctypes.c_void_p()

    def close(self):
        """Session, then scene (0007 d28); idempotent, so a __del__ after shutdown() does nothing."""
        self._close_session()
        if self.scene_h.value:
            native.call(self.lib.ar_scene_destroy, self.scene_h)
            self.scene_h = ctypes.c_void_p()
        if self._context is not None:
            self._context.jobs.discard(self)


def read_pass(lib, abi, session, name, quantity):
    """One pass as (height, width, components); enumerates first (rulings 74 and 75)."""
    n = ctypes.c_uint32()
    native.call(lib.ar_session_pass_count, session, ctypes.byref(n))
    for i in range(n.value):
        info = abi.ar_pass_info()
        native.call(lib.ar_session_pass_info, session, i, ctypes.byref(info))
        if bytes(info.name).split(b"\0")[0].decode("utf-8") != name:
            continue
        comps = 1 if quantity == abi.AR_QUANTITY_COUNT else info.component_count  # COUNT is per pixel
        buf = np.zeros(info.width * info.height * comps, np.float32)
        _ok(lib, native.call(lib.ar_session_read_pass, session, i, quantity, buf.size,
                             buf.ctypes.data_as(ctypes.POINTER(ctypes.c_float))),
            f"ar_session_read_pass({name})", "ar_session_last_error", session)
        return buf.reshape(info.height, info.width, comps)
    raise RenderFailed(f"astroray: the session offers no {name} pass")
