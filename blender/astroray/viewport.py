# SPDX-License-Identifier: GPL-3.0-or-later
"""Viewport rendering: view_update syncs deltas, view_draw presents (0007 d23, d25, d26, d49).

One ViewportJob per RenderEngine instance in viewport mode. It is a render_final.Job: its scene
lives on the same per-device context record, so render_final.shutdown() closes viewport sessions
in the same order as F12 jobs (session, then scene).

Ref: docs/decisions/0007-blender-integration.md d20 (classification table and apply order),
d23 (view_update and view_draw run on the main thread and never block; destruction in two steps:
__del__ calls ar_session_close and parks the handles, a timer destroys session then scene once the
session reports CLOSED), d25 (a frame is shown only if its generation is >= the last one shown),
d26 (view_draw tags a redraw while rendering; a timer polls only while async work is pending after
convergence), d49 (set_view only when the view hash changed); docs/decisions/RULINGS.md 69;
Cycles intern/cycles/blender/session.cpp, BlenderSession::view_update and ::view_draw.
The poll period is 0009 B-37's 50 ms.
"""
import ctypes
import sys
import time
import weakref

import numpy as np

from . import native, present, render_final
from .materials import translate
from .sync import camera, classify, edit as edit_mod, images, instances, keys, mesh, settings
from .sync import world as world_sync, lights

POLL_S = 0.05  # 0009 B-37: completion poll period
MAX_REAP_FAILURES = 20  # consecutive failed teardown ticks (one second) before the reaper stops retrying


class ViewportJob(render_final.Job):
    def __init__(self):
        super().__init__()
        self.present = present.Present()
        self.views = camera.ViewSync()
        self.events = []          # lifecycle, in order; the tests read it
        self.frames = None        # a list when a test wants every presented frame logged
        self.acquired = None      # likewise every acquired frame: id, generation, whether it was presented (d25)
        self.check_uploads = 0    # test-only: this many uploads are read back from the GPU and compared
        self.upload_checks = []
        self.retired = []         # sessions closed and waiting for the reaper to destroy them (d23)
        self.parked = False       # RenderEngine.__del__ has run: the reaper ends the job once nothing is left
        self.register_timer = None  # test-only seam: replaces bpy.app.timers.register
        self._reaper_on = False
        self._reap_failures = 0
        self.errors = []          # every teardown problem, also printed (never silent)
        self.failure = None
        self.table = None         # instance table of the last full instance send, for transform patches
        self.geom_keys = set()
        self.sent_images = set()  # original session_uids of the images the engine has
        self._max = (0, 0)
        self._started = False
        self._last_id = 0
        self._engine = None
        self._reported, self._revision = set(), None
        self._polling = False
        self.t_first_frame = None  # monotonic time of the first upload

    # ---- view_update

    def view_update(self, engine, context, depsgraph):
        self._engine = weakref.ref(engine)
        if self.failure:
            return
        try:
            self._sync(depsgraph)
            for line in self.lines:
                engine.report({"WARNING"}, line)
            self.lines = []
        except Exception as e:
            self._fail(engine, e)

    def _sync(self, depsgraph):
        if self.first:
            super().update(depsgraph, need_camera=False)
            self._after_full(depsgraph)
            return
        actions = classify.classify(depsgraph, self.cache)
        if not actions:
            return
        kinds = {a.kind for a in actions}
        if classify.FULL_RESYNC in kinds:
            self.tcache.forget_all()
            self.first = True
            super().update(depsgraph, need_camera=False)
            self._after_full(depsgraph)
            return
        self.tcache.invalidate([a for a in actions if a.kind == classify.MATERIALS])
        self.istate.note_updates(actions, classify.IMAGES)  # as Job.update does
        with edit_mod.Edit(self.abi, self.lib, self.scene_h) as e:
            self._apply(e, depsgraph, actions, kinds)
        self.cache.remember(depsgraph, depsgraph.scene)
        self.events.append("delta " + ",".join(classify.KIND_NAMES[k] for k in sorted(kinds)))

    def _after_full(self, depsgraph):
        self.table = None
        geoms, mats = render_final.scan(depsgraph, self.keymap)
        self.geom_keys = set(geoms)
        self.sent_images = {im.original.session_uid for im in self._used_images(depsgraph, mats)}
        self.events.append("full")

    @staticmethod
    def _used_images(depsgraph, mats):
        """Images of the materials and the world, the walk full_sync makes."""
        trees = [m.original.evaluated_get(depsgraph).node_tree for m in mats.values()]
        world = depsgraph.scene.world
        if world and world.node_tree:
            trees.append(world.node_tree)
        return images.used_images(trees)

    def _apply(self, e, depsgraph, actions, kinds):
        """One edit, in 0007 d20's order. Camera actions need nothing: the viewport's camera is the view."""
        abi, scene, view_layer = self.abi, depsgraph.scene, depsgraph.view_layer
        lightgroups = instances.lightgroup_indices(view_layer)
        C = classify
        if C.SETTINGS in kinds:
            fields, self.lines = settings.build(scene, render_final._colorspace(),
                                                render_final.view_layer_passes(view_layer), lightgroups)
            settings.set_settings(e, abi, fields)
        if C.WORLD in kinds:
            world = scene.world
            world_sync.sync(e, abi, world.evaluated_get(depsgraph) if world else None, self.keymap, self.op_id)
        geoms, mats = render_final.scan(depsgraph, self.keymap)
        new_geoms = set(geoms) - self.geom_keys
        if kinds & {C.IMAGES, C.WORLD, C.MATERIALS, C.INSTANCES}:
            # Images after the world and before the materials (d20): those a change names and those new to the scene.
            changed = {a.uid for a in actions if a.kind == C.IMAGES}
            used = self._used_images(depsgraph, mats)
            todo = [im for im in used if im.original.session_uid in changed or im.original.session_uid not in self.sent_images]
            self.lines.extend(images.sync(e, abi, self.keymap, todo, self.istate))
            self.sent_images |= {im.original.session_uid for im in todo}
        if C.MATERIALS in kinds or C.INSTANCES in kinds:
            for uid, mat in mats.items():
                before = self.tcache.translations
                em = self.tcache.get(mat.original.evaluated_get(depsgraph), self.keymap, self.op_id)
                if self.tcache.translations != before:  # a miss: changed, or new with its object
                    translate.set_material(e, abi, self.keymap, uid, em)
        if C.LIGHTS in kinds:
            self.lines.extend(lights.sync(e, abi, depsgraph, self.keymap, lightgroups=lightgroups))
        touched = {a.uid for a in actions if a.kind == C.GEOMETRIES}
        for key, ob in geoms.items():
            data = ob.data.original.session_uid if ob.data is not None else None
            if key in new_geoms or ob.original.session_uid in touched or data in touched:
                with mesh.mesh_of(ob) as me:
                    mesh.sync_mesh(e, abi, me, key, ob.original.name)
        self.geom_keys |= new_geoms
        inst = [a for a in actions if a.kind == C.INSTANCES]
        if inst or new_geoms:
            moves = all(a.id_type == "OBJECT" and a.mask == C.TRANSFORM for a in inst)
            if not (inst and moves and not new_geoms and self.table is not None
                    and instances.sync_transforms(e, abi, self.table, depsgraph, {a.uid for a in inst})):
                self.table = instances.build(depsgraph, self.keymap, lightgroups=lightgroups)
                instances.set_instances(e, abi, self.table)

    # ---- view_draw

    def view_draw(self, engine, context, depsgraph):
        self._engine = weakref.ref(engine)
        if self.failure or self.first:
            return
        try:
            self._draw(engine, context, depsgraph)
        except Exception as e:
            self._fail(engine, e)

    def _draw(self, engine, context, depsgraph):
        lib, abi = self.lib, self.abi
        region, rv3d = context.region, context.region_data
        w, h = region.width, region.height
        self._ensure_session(w, h)
        s = self.session
        f, _ = camera.viewport(depsgraph.scene, context.space_data, rv3d, depsgraph)
        digest = camera.view_hash(rv3d, w, h, rv3d.view_perspective == "CAMERA")
        if self.views.update(lambda sess, v: native.call(lib.ar_session_set_view, sess, v), abi, s, f, digest, w, h):
            self.events.append("set_view")
        if not self._started:
            render_final._ok(lib, native.call(lib.ar_session_start, s), "ar_session_start",
                             "ar_session_last_error", s)
            self._started = True
        got = self._present_newest()
        prog = abi.ar_progress()
        native.call(lib.ar_session_get_progress, s, ctypes.byref(prog))
        self._revision = self._report(engine, self._reported, self._revision)
        if prog.state == abi.AR_STATE_FAILED:
            render_final._ok(lib, 1, "the session (state FAILED)", "ar_session_last_error", s)
        self.present.draw(engine, depsgraph.scene, w, h)
        busy = prog.state in (abi.AR_STATE_INITIALIZING, abi.AR_STATE_RENDERING)
        if busy or got:
            engine.tag_redraw()
        elif self._pending(prog) and not self._polling:
            self._polling = True
            import bpy
            bpy.app.timers.register(self._poll, first_interval=POLL_S)

    def _pending(self, prog):
        """Async work that a redraw must pick up although nothing renders: a commit not yet active, or an
        active generation whose frame is not on screen."""
        waiting_commit = prog.pending_generation > prog.active_generation and prog.failed_generation < prog.pending_generation
        return waiting_commit or self.present.generation < prog.active_generation

    def _present_newest(self):
        """Acquire after the last frame seen; upload it when d25 allows; always release. True if a frame arrived."""
        lib, abi, s = self.lib, self.abi, self.session
        frame = abi.ar_display_frame()
        native.call(lib.ar_display_acquire, s, self._last_id, ctypes.byref(frame))
        if not frame.frame_id:
            return False
        try:
            self._last_id = frame.frame_id
            shown = self.present.wants(frame)
            if self.acquired is not None:
                self.acquired.append({"id": frame.frame_id, "generation": frame.generation, "presented": shown})
            if shown:
                self.present.upload(frame)
                if self.t_first_frame is None:
                    self.t_first_frame = time.monotonic()
                if self.frames is not None:
                    self._log_frame(frame)
                if self.check_uploads > 0:
                    self.check_uploads -= 1
                    self._check_upload(frame)
        finally:
            native.call(lib.ar_display_release, s, frame.frame_id)
        return True

    def _check_upload(self, frame):
        """Test-only: the persistent texture against the host frame, bit for bit (half to float is exact)."""
        host = present.frame_words(frame).view(np.float16).reshape(frame.height, frame.width, 4).astype(np.float32)
        tex = self.present.read_back()
        self.upload_checks.append({"max_diff": float(np.abs(tex - host).max()), "size": (frame.width, frame.height),
                                   "stripes": len(self.present._shown.stripes),
                                   "colour_spread": float((host[:, :, :3].max(axis=2) - host[:, :, :3].min(axis=2)).max())})

    def _log_frame(self, frame):
        px = present.frame_words(frame).view(np.float16).reshape(frame.height, frame.width, 4)
        step = max(1, min(frame.width, frame.height) // 64)
        self.frames.append({"id": frame.frame_id, "generation": frame.generation, "t": time.monotonic(),
                            "size": (frame.width, frame.height),
                            "rgb": px[::step, ::step, :3].astype(np.float32)})

    def _ensure_session(self, w, h):
        """A session whose film is the region's; created on the first draw. A region that outgrows it gets a new
        session and the old one is retired in d23's two steps (closed now, destroyed by a timer), so the draw
        never waits for it."""
        if self.session.value and w <= self._max[0] and h <= self._max[1]:
            return
        lib, abi = self.lib, self.abi
        if self.session.value:
            self._retire_current()
            self.events.append("replace_session")
        try:
            sd = abi.ar_session_desc(abi.AR_SESSION_MODE_VIEWPORT, w, h, abi.AR_DENOISER_NONE)
            render_final._ok(lib, native.call(lib.ar_session_create, self.scene_h, ctypes.byref(sd),
                                              ctypes.byref(self.session)),
                             "ar_session_create", "ar_scene_last_error", self.scene_h)
            self.views = camera.ViewSync()
            self._last_id = 0
            self.present.generation = -1  # a new session numbers its frames afresh
            self._max = (w, h)
        except BaseException:
            self._close_session()
            raise
        self._started = False
        self.events.append("create_session")

    def _fail(self, engine, error):
        self.failure = str(error)
        self.events.append("failed")
        engine.report({"ERROR"}, self.failure)

    # ---- redraw poll (d26)

    def _poll(self):
        engine = self._engine() if self._engine else None
        if engine is None or self.failure or not self.session.value:
            self._polling = False
            return None
        abi = self.abi
        prog = abi.ar_progress()
        native.call(self.lib.ar_session_get_progress, self.session, ctypes.byref(prog))
        pending = self._pending(prog)
        engine.tag_redraw()
        if pending and prog.state not in (abi.AR_STATE_FAILED, abi.AR_STATE_CLOSED):
            return POLL_S
        self._polling = False
        return None

    # ---- destruction (d23)
    # One source of truth: `retired` (closed sessions) and `session` (the current one) hold every live handle,
    # a handle leaves its list before it is destroyed, and the one reaper timer destroys what has closed.

    def _start_reaper(self):
        """The single timer of this job; registered before any list changes, so a failure leaves state intact."""
        if self._reaper_on:
            return
        import bpy
        (self.register_timer or bpy.app.timers.register)(self._reap, first_interval=POLL_S)
        self._reaper_on = True

    def _retire_current(self):
        """Step one of d23 for the current session: ar_session_close, which never blocks; the reaper does step
        two. The reaper is started first and the handle listed before the call, so a failure of either leaves
        the session where close() finds it."""
        self._start_reaper()
        session, self.session = self.session, ctypes.c_void_p()
        self.retired.append(session)
        native.call(self.lib.ar_session_close, session)
        self.events.append("session_close")

    def park(self):
        """RenderEngine.__del__ (d23): close the current session and let the reaper end the job."""
        try:
            self._start_reaper()
        except Exception as e:
            self._note(f"no timer to destroy the viewport session ({e}); destroying it directly")
            self.close()  # destroys directly, once; raises if something failed on the way
            return
        self.parked = True
        if self.session.value:
            self._retire_current()

    def _note(self, text):
        """A teardown problem is never silent: kept, printed, and reported when the engine still exists."""
        text = "astroray: " + text
        self.errors.append(text)
        print(text, file=sys.stderr, flush=True)
        engine = self._engine() if self._engine else None
        if engine is not None:
            try:
                engine.report({"ERROR"}, text)
            except Exception:
                pass

    def _reap(self):
        """Each tick: destroy every retired session that reports CLOSED; destroy the scene only when the job is
        parked, no session is current and none is retired. Ends itself when it has nothing left to wait for.
        A failure is noted and the timer stays alive to try again, up to MAX_REAP_FAILURES ticks in a row;
        what could not be destroyed stays listed for close()."""
        try:
            return self._reap_once()
        except Exception as e:
            self._reap_failures += 1
            self._note(f"viewport teardown tick failed ({self._reap_failures}): {e}")
            if self._reap_failures < MAX_REAP_FAILURES:
                return POLL_S
            self._note("viewport teardown gave up; shutdown will destroy what is left")
            self._reaper_on = False
            return None

    def _reap_once(self):
        errors = []
        for session in list(self.retired):
            try:
                prog = self.abi.ar_progress()
                native.call(self.lib.ar_session_get_progress, session, ctypes.byref(prog))
                if prog.state == self.abi.AR_STATE_CLOSED:
                    self._destroy_session(session, "timer")
            except Exception as e:  # the others still get their turn
                errors.append(str(e))
        if errors:
            raise RuntimeError("; ".join(errors))
        self._reap_failures = 0
        if self.parked and not self.session.value and not self.retired:
            self._destroy_scene()
            self._reaper_on = False
            return None
        if not self.parked and not self.retired:
            self._reaper_on = False
            return None
        return POLL_S

    def _destroy_session(self, session, via):
        self.retired = [r for r in self.retired if r.value != session.value]  # off the list first: at most once
        native.call(self.lib.ar_session_destroy, session)
        self.events.append(f"destroy_session via {via}")

    def _destroy_scene(self):
        if self.retired or self.session.value:
            raise RuntimeError("astroray: the scene was about to be destroyed while a session is alive")
        try:
            if self.scene_h.value:
                scene, self.scene_h = self.scene_h, ctypes.c_void_p()
                native.call(self.lib.ar_scene_destroy, scene)
                self.events.append("destroy_scene")
        finally:
            if self._context is not None:  # what render_final.Job.close does after the scene
                self._context.jobs.discard(self)
            self.present.free()

    def close(self):
        """Every session (closed first, as ar_session_destroy of a live render would otherwise wait), then the
        scene (d23, d28); idempotent. Blocks: shutdown, unregister and the no-timer fallback only. It does
        what Job.close does (session, scene, release from the context's job set) over this class's lists, so
        it does not call it. One failing call does not stop the rest: every handle and the scene are tried,
        and one combined error is raised at the end."""
        handles, self.retired = list(self.retired), []
        if self.session.value:
            handles.append(self.session)
            self.session = ctypes.c_void_p()
        errors = []
        for session in handles:
            for name in ("ar_session_close", "ar_session_destroy"):
                try:
                    native.call(getattr(self.lib, name), session)
                except Exception as e:
                    errors.append(f"{name}: {e}")
            self.events.append("destroy_session via close")
        try:
            self._destroy_scene()
        except Exception as e:
            errors.append(f"ar_scene_destroy: {e}")
        if errors:
            text = "closing the viewport job failed: " + "; ".join(errors)
            self._note(text)
            raise RuntimeError("astroray: " + text)
