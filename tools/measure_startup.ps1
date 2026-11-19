# SPDX-License-Identifier: Apache-2.0
# tools/measure_startup.ps1 -Step startup|motion|edit|lights|nsys|sync [-Runs 10]
# One-off measurement driver of task 094 (deleted when the task lands; its commands stay in the PR Result).
# Every step holds the machine lock for all of its timed runs and prints one line per run and a summary.
#   startup  B-18: ar_scene_create -> first pass complete, 10 fresh processes warm (OptiX cache_dir and
#            CUDA_CACHE_PATH populated) and 10 cold (both empty, new per run); the global caches are never touched.
#   motion   B-18 motion variant: astroray_tests_gpu "optix_pipeline/creation times cold and warm", fresh process and
#            fresh CUDA_CACHE_PATH per run (the test empties its own OptiX cache directory).
#   edit     B-22: edit commit -> accumulation reset (epoch change) and first pass, idle and busy sessions.
#   lights   B-23: commit -> first pass of 10,000 lamps and of 1 M emissive triangles against their baselines.
#   nsys     B-28/B-29: Nsight Systems kernel table of `astroray_cli bench` on b1m at 1080p and 4K.
#   sync     B-31: S1 (b1m) and S2 (b5m) in Blender: full mesh read, positions only, instance loop, plain move.
# Ref: docs/decisions/0009-build-test-process.md B-18, B-22, B-23, B-28, B-29, B-31; 0007 V3.
param(
  [Parameter(Mandatory)][ValidateSet('startup', 'motion', 'edit', 'lights', 'nsys', 'sync')][string]$Step,
  [int]$Runs = 10
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
. (Join-Path $PSScriptRoot 'lib/lock.ps1')
. (Join-Path $PSScriptRoot 'lib/python.ps1')
. (Join-Path $PSScriptRoot 'lib/blender.ps1')
$work = Join-Path $root 'out/measure094'
New-Item -ItemType Directory -Force $work | Out-Null
$py = Get-ArPython

# The child process of startup, edit and lights: the ABI through ctypes (scenes/lib/ar.py), one JSON line out.
$child = @'
import ctypes, json, os, sys, tempfile, time
import numpy as np
root, mode, cache_dir, logp = sys.argv[1:5]
extra = sys.argv[5:]
sys.path.insert(0, os.path.join(root, "scenes", "lib"))
import ar, meshes
pc = time.perf_counter


class Sc(ar.Scene):
    """ar.Scene with the scene's cache_dir set (the stock constructor passes none) and no recording."""
    def __init__(self, cache_dir, log_path, device="cuda"):
        self.path = os.path.join(tempfile.gettempdir(), "m094.arrec")
        self._partial = self.path + ".partial"
        self.description_path = None
        self._desc = {"meshes": [], "materials": [], "instances": [], "lights": [], "world": None,
                      "camera": None, "settings": None}
        self._tmp = tempfile.TemporaryDirectory()
        self.abi = ar._load("_ar_abi", os.path.join(ar.GEN, "_abi.py"))
        self.op_id = ar._load("_ar_ops", os.path.join(ar.GEN, "_ops.py")).OP_ID
        self.lib = ctypes.CDLL(ar.DLL)
        self.abi.check(self.lib)
        self.abi.bind(self.lib)
        a = self.abi
        self.ctx, self.scene_h, self.edit_h = ctypes.c_void_p(), ctypes.c_void_p(), ctypes.c_void_p()
        self.session_h = ctypes.c_void_p()
        self._keys, self._next_key = set(), 1
        self.size, self._camera, self._finished = None, None, False
        desc = a.ar_context_desc(a.AR_DEVICE_CPU if device == "cpu" else a.AR_DEVICE_CUDA, 0, log_path.encode(), None)
        self._call(self.lib.ar_context_create(ctypes.byref(desc), ctypes.byref(self.ctx)), "ar_context_create")
        self.t_ctx = pc()
        sd = a.ar_scene_desc(cache_dir.encode() if cache_dir else None, 2, 0)
        self.t0 = pc()
        self._call(self.lib.ar_scene_create(self.ctx, ctypes.byref(sd), ctypes.byref(self.scene_h)), "ar_scene_create")
        self.t_scene = pc()
        self.new_edit()

    def new_edit(self):
        h = ctypes.c_void_p()
        self._call(self.lib.ar_scene_edit_begin(self.scene_h, ctypes.byref(h)), "ar_scene_edit_begin")
        self.edit_h = h

    def commit(self):
        gen = ctypes.c_uint64()
        t = pc()
        self._call(self.lib.ar_edit_commit(self.edit_h, ctypes.byref(gen)), "ar_edit_commit")
        return t, pc()

    def session(self, w, h):
        a, lib = self.abi, self.lib
        sd = a.ar_session_desc(a.AR_SESSION_MODE_VIEWPORT, w, h, a.AR_DENOISER_NONE)
        self._call(lib.ar_session_create(self.scene_h, ctypes.byref(sd), ctypes.byref(self.session_h)), "ar_session_create")
        self.view = a.ar_view_desc()
        ctypes.memmove(ctypes.byref(self.view.camera), ctypes.byref(self._camera), ctypes.sizeof(a.ar_camera_desc))
        self.view.region_width, self.view.region_height, self.view.render_scale = w, h, 1.0
        self._call(lib.ar_session_set_view(self.session_h, ctypes.byref(self.view)), "ar_session_set_view")
        return self.session_h

    def progress(self):
        p = self.abi.ar_progress()
        self.lib.ar_session_get_progress(self.session_h, ctypes.byref(p))
        return p

    def spin(self, pred, what, timeout=120.0):
        t = pc()
        while True:
            p = self.progress()
            now = pc()
            if p.state == self.abi.AR_STATE_FAILED:
                raise RuntimeError("session FAILED while waiting for " + what)
            if pred(p):
                return now, p
            if now - t > timeout:
                raise RuntimeError(f"timeout waiting for {what}: state {p.state} spp {p.samples_per_pixel}")


def plane_scene(s, w, h, spp, seed=1):
    plane = s.key()
    s.mesh_from(plane, meshes.plane(8.0, 8.0), "plane")
    s.world((1.0, 1.0, 1.0), 1.0)
    s.settings(spp, w, h, seed=seed, sampler="independent")
    s.camera((0.0, 0.0, 3.0), (0.0, 0.0, 0.0), 35.0, 36.0, w, h, up=(0.0, 1.0, 0.0))
    g = s.graph()
    g.store("base_weight", 1.0)
    s.instances([{"geom": plane, "materials": [s.material_from_graph(g)]}])


def log_lines(path):
    try:
        return open(path, encoding="utf-8", errors="replace").read().splitlines()
    except OSError:
        return []


def pipeline_times(path):
    import re
    for ln in log_lines(path):
        m = re.search(r"traversal pipeline created in ([0-9.]+) ms \(module ([0-9.]+) ms, pipeline ([0-9.]+) ms\)", ln)
        if m:
            return [float(x) for x in m.groups()]
    return None


def startup():
    s = Sc(cache_dir, logp)
    a = s.abi
    plane_scene(s, 256, 256, 1)
    t_built = pc()
    tc0, tc1 = s.commit()
    s.session(256, 256)
    t_sess = pc()
    s._call(s.lib.ar_session_start(s.session_h), "ar_session_start")
    t_start = pc()
    tf, p = s.spin(lambda p: p.state == a.AR_STATE_CONVERGED and p.samples_per_pixel >= 1, "the first pass")
    took = pipeline_times(logp)
    out = {"scene_create_call_ms": (s.t_scene - s.t0) * 1e3, "built_ms": (t_built - s.t0) * 1e3,
           "commit_return_ms": (tc1 - s.t0) * 1e3, "session_create_ms": (t_sess - s.t0) * 1e3,
           "start_ms": (t_start - s.t0) * 1e3, "first_pass_done_ms": (tf - s.t0) * 1e3,
           "context_create_ms": (s.t0 - s.t_ctx) * 1e3, "last_pass_ms": p.last_pass_ms,
           "pipeline_wait_and_create_ms": took[0] if took else None, "module_ms": took[1] if took else None,
           "pipeline_ms": took[2] if took else None}
    if took:
        # the session thread starts at session_create; its wait for the init thread ends when init is done
        out["init_done_est_ms"] = (t_sess - s.t0) * 1e3 + took[0] - took[1] - took[2]
    print("RESULT " + json.dumps(out), flush=True)
    os._exit(0)


def edit():
    s = Sc(cache_dir, logp)
    a = s.abi
    W, H = 1920, 1080
    plane_scene(s, W, H, 2)
    s.commit()
    s.session(W, H)
    s._call(s.lib.ar_session_start(s.session_h), "ar_session_start")
    s.spin(lambda p: p.state == a.AR_STATE_CONVERGED and p.samples_per_pixel >= 2, "the first epoch")
    rows = []

    def measure(kind, i, busy):
        p0 = s.progress()
        e0, g0 = p0.epoch, p0.pending_generation
        t0 = pc()
        if kind == "view":
            m = s.view.camera.cam_to_world.m
            m[3] += 0.01 * (1 if i % 2 else -1)
            s._call(s.lib.ar_session_set_view(s.session_h, ctypes.byref(s.view)), "ar_session_set_view")
            t1 = pc()
        else:
            s.new_edit()
            if kind == "seed":
                s.settings(100000 if busy else 2, W, H, seed=1000 + i, sampler="independent")
            elif kind == "world":
                s.world((0.5 + 0.01 * (i % 40), 0.5, 0.5), 1.0)
            t0, t1 = s.commit()
        te, pe = s.spin(lambda p: p.epoch != e0, "the epoch change")
        tf, pf = s.spin(lambda p: p.epoch == pe.epoch and p.samples_per_pixel >= 1, "the first pass of the epoch")
        rows.append({"kind": kind, "busy": busy, "call_ms": (t1 - t0) * 1e3, "to_epoch_ms": (te - t0) * 1e3,
                     "to_first_pass_done_ms": (tf - t0) * 1e3, "last_pass_ms": pf.last_pass_ms,
                     "start_est_ms": (tf - t0) * 1e3 - pf.last_pass_ms})
        if not busy:
            s.spin(lambda p: p.state == a.AR_STATE_CONVERGED and p.epoch == pe.epoch, "convergence")
            time.sleep(0.05)

    n = int(extra[0]) if extra else 30
    for kind in ("seed", "world", "view"):
        for i in range(n):
            measure(kind, i, False)
    # busy: the session renders a long epoch, so an edit lands in a pass in flight
    s.new_edit()
    s.settings(100000, W, H, seed=7, sampler="independent")
    s.commit()
    time.sleep(0.5)
    for kind in ("seed", "world", "view"):
        for i in range(n):
            measure(kind, i, True)
            time.sleep(0.05)
    print("RESULT " + json.dumps(rows), flush=True)
    os._exit(0)


def b1m_scene(s, emissive):
    sys.path.insert(0, os.path.join(root, "scenes", "b1m"))
    import build as b1m
    mesh = s.key()
    pos, cv, fs, nrm, uv = b1m.grid()
    s.mesh(mesh, pos, cv, fs, nrm, uv, "S1_mesh")
    if emissive:
        mat = s.material({"base_color": (0.8, 0.4, 0.2), "specular_roughness": 0.5}, emission=((1.0, 0.9, 0.8), 2.0))
    else:
        mat = s.material({"base_color": (0.8, 0.4, 0.2), "specular_roughness": 0.5})
    s.instances([{"geom": mesh, "materials": [mat]}])
    b1m.sun_lamp(s, (0.0, 0.0, 0.0), b1m.LAMP_DISTANCE, b1m.LAMP_SIZE, b1m.SUN_STRENGTH, b1m.SUN_EULER)
    s.world((0.3, 0.35, 0.4), 1.0)
    s.settings(1, 1920, 1080, seed=b1m.SEED, sampler="sobol")
    s.camera((0.0, -16.0, 12.0), (0.0, 0.0, 0.0), 35.0, 36.0, 1920, 1080)


def lights():
    kind = extra[0]
    s = Sc(cache_dir, logp)
    a = s.abi
    if kind == "b1m":
        b1m_scene(s, False)
    elif kind == "b1m_emissive":
        b1m_scene(s, True)
    else:
        n = int(extra[1])
        plane_scene(s, 1920, 1080, 1)
        rng = np.random.default_rng(5)
        for i in range(n):
            x, y = rng.uniform(-3.5, 3.5, 2)
            s.light("point", xf=ar.translation((x, y, 1.0)), power=0.5, size=(0.02, 0.0))
    t_built = pc()
    tc0, tc1 = s.commit()
    s.session(1920, 1080)
    s._call(s.lib.ar_session_start(s.session_h), "ar_session_start")
    tf, p = s.spin(lambda p: p.state == a.AR_STATE_CONVERGED and p.samples_per_pixel >= 1 and p.active_generation >= 1, "the first pass")
    pr = s.progress()
    out = {"kind": kind, "lamps": int(extra[1]) if len(extra) > 1 else None,
           "build_ms": (t_built - s.t_scene) * 1e3, "commit_call_ms": (tc1 - tc0) * 1e3,
           "commit_to_first_pass_ms": (tf - tc0) * 1e3, "first_pass_ms": p.last_pass_ms,
           "device_free": pr.device_free_bytes, "device_total": pr.device_total_bytes,
           "ledger": [int(x) for x in pr.ledger_bytes]}
    print("RESULT " + json.dumps(out), flush=True)
    os._exit(0)


{"startup": startup, "edit": edit, "lights": lights}[mode]()
'@
# The Blender script of step sync (B-31): the addon's own sync functions timed on S1 and S2.
$syncScript = @'
import importlib, json, os, statistics, sys, time
import numpy as np
import bpy
_a = sys.argv[sys.argv.index("--") + 1:]
work, runs, ROOT = _a[0], int(_a[1]), _a[2]
for sub in ("tests/blender", "tools", "scenes/lib"):
    sys.path.insert(0, os.path.join(ROOT, sub))
import blend_writer, smoke_a1
smoke_a1.install_and_enable()
M = smoke_a1.MODULE
mesh = importlib.import_module(M + ".sync.mesh")
inst = importlib.import_module(M + ".sync.instances")
keys = importlib.import_module(M + ".sync.keys")
pc = time.perf_counter


class Stub:
    def __init__(self): self.calls = 0
    def call(self, *a): self.calls += 1


def med(v): return statistics.median(v)


for scene in ("b1m", "b5m"):
    blend = os.path.join(work, scene + ".blend")
    t = pc()
    blend_writer.build(blend + ".json", blend)   # in this process: the extension is enabled
    bpy.ops.wm.open_mainfile(filepath=blend, load_ui=False)
    print(f"{scene}: blend written and opened in {pc() - t:.1f} s", flush=True)
    dg = bpy.context.evaluated_depsgraph_get()
    seen, objs = set(), []
    for it in dg.object_instances:
        ob = it.object
        if ob.type == "MESH" and ob.data.as_pointer() not in seen:
            seen.add(ob.data.as_pointer())
            objs.append(ob)
    meshes = [ob.data for ob in objs]
    tris = sum(len(m.loops) - 2 * len(m.polygons) for m in meshes)
    n_inst = sum(1 for it in dg.object_instances if it.object.type == "MESH")
    print(f"{scene}: {len(meshes)} unique meshes, {tris} triangles, {n_inst} mesh instances", flush=True)
    lays = [mesh.layout(m) for m in meshes]
    arrs = [mesh.alloc(l) for l in lays]
    # cold: the first read of every mesh into freshly allocated arrays (first touch of both sides)
    t = pc()
    for m, a in zip(meshes, arrs): mesh.fill(m, a)
    cold_full = (pc() - t) * 1e3
    warm_full = []
    for _ in range(runs):
        t = pc()
        for m, a in zip(meshes, arrs): mesh.fill(m, a)
        warm_full.append((pc() - t) * 1e3)
    arrs2 = [mesh.alloc(l) for l in lays]
    t = pc()
    for m, a in zip(meshes, arrs2): mesh.fill_positions(m, a.positions, a.corner_normals)
    cold_pos = (pc() - t) * 1e3
    warm_pos = []
    for _ in range(runs):
        t = pc()
        for m, a in zip(meshes, arrs2): mesh.fill_positions(m, a.positions, a.corner_normals)
        warm_pos.append((pc() - t) * 1e3)
    per = 1e6 / tris
    print(f"RESULT {scene} full mesh read: cold {cold_full:.1f} ms = {cold_full * per:.1f} ms per 1M tris; "
          f"warm median {med(warm_full):.1f} ms = {med(warm_full) * per:.1f} per 1M (min {min(warm_full) * per:.1f}, max {max(warm_full) * per:.1f})", flush=True)
    print(f"RESULT {scene} positions only: cold {cold_pos:.2f} ms = {cold_pos * per:.2f} ms per 1M tris; "
          f"warm median {med(warm_pos):.2f} ms = {med(warm_pos) * per:.2f} per 1M (min {min(warm_pos) * per:.2f}, max {max(warm_pos) * per:.2f})", flush=True)
    km = keys.KeyMap()
    t = pc()
    table = inst.build(dg, km)
    cold_inst = (pc() - t) * 1e3
    warm_inst = []
    for _ in range(runs):
        t = pc()
        table = inst.build(dg, km)
        warm_inst.append((pc() - t) * 1e3)
    k10 = 1e4 / max(table.n, 1)
    print(f"RESULT {scene} instance loop ({table.n} instances): cold {cold_inst:.1f} ms = {cold_inst * k10:.1f} ms per 10k; "
          f"warm median {med(warm_inst):.1f} ms = {med(warm_inst) * k10:.1f} per 10k (min {min(warm_inst) * k10:.1f}, max {max(warm_inst) * k10:.1f})", flush=True)
    # a plain move: one object's translation changes, then the transform patch is built (the engine call is a stub)
    mover = next(o for o in bpy.context.scene.objects if o.type == "MESH")
    uid = mover.original.session_uid if hasattr(mover, "original") else mover.session_uid
    cold_mv, warm_mv = None, []
    for i in range(runs + 1):
        mover.location.x += 0.01
        bpy.context.view_layer.update()
        dg = bpy.context.evaluated_depsgraph_get()
        stub = Stub()
        t = pc()
        ok = inst.sync_transforms(stub, None, table, dg, {uid})
        dt = (pc() - t) * 1e3
        if not ok or stub.calls != 1:
            raise RuntimeError(f"{scene}: sync_transforms returned {ok}, {stub.calls} calls")
        if i == 0: cold_mv = dt
        else: warm_mv.append(dt)
    print(f"RESULT {scene} plain move patch: cold {cold_mv:.3f} ms; warm median {med(warm_mv):.3f} ms (min {min(warm_mv):.3f}, max {max(warm_mv):.3f})", flush=True)
print("sync done", flush=True)
os._exit(0)
'@
$childPath = Join-Path $work 'm094_child.py'
Set-Content -Path $childPath -Value $child -Encoding utf8

function Invoke-Child([string]$mode, [string]$cacheDir, [string]$cudaCache, [string[]]$extra, [string]$tag) {
  $logp = Join-Path $work "ctx-$tag.log"
  Remove-Item $logp -ErrorAction SilentlyContinue
  $saved = $env:CUDA_CACHE_PATH
  try {
    if ($cudaCache) { New-Item -ItemType Directory -Force $cudaCache | Out-Null; $env:CUDA_CACHE_PATH = $cudaCache }
    $o = & $py -I $childPath $root $mode $cacheDir $logp @extra 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { throw "child $mode failed ($LASTEXITCODE): $($o | Select-Object -Last 6 | Out-String)" }
  } finally { $env:CUDA_CACHE_PATH = $saved }
  $line = $o | Where-Object { $_ -like 'RESULT *' } | Select-Object -Last 1
  if (-not $line) { throw "child $mode printed no RESULT: $($o | Select-Object -Last 6 | Out-String)" }
  return ($line.Substring(7) | ConvertFrom-Json)
}

function Show-Stats([string]$name, [double[]]$v) {
  $s = $v | Sort-Object
  $n = $s.Count
  $med = if ($n % 2) { $s[[int](($n - 1) / 2)] } else { ($s[$n / 2 - 1] + $s[$n / 2]) / 2 }
  Write-Host ("{0,-34} n={1} min {2,9:N2} median {3,9:N2} max {4,9:N2}" -f $name, $n, $s[0], $med, $s[-1])
}

$null = Enter-ArMachineLock
try {
  switch ($Step) {
    'startup' {
      $dirs = Join-Path $work 'warm'
      Remove-Item -Recurse -Force $dirs -ErrorAction SilentlyContinue
      $null = Invoke-Child startup (Join-Path $dirs 'optix') (Join-Path $dirs 'cuda') @() 'prime'
      $res = @{ warm = @(); cold = @() }
      foreach ($kind in 'warm', 'cold') {
        for ($i = 0; $i -lt $Runs; $i++) {
          if ($kind -eq 'warm') { $o = Join-Path $dirs 'optix'; $c = Join-Path $dirs 'cuda' }
          else {
            $d = Join-Path $work "cold/$i"
            Remove-Item -Recurse -Force $d -ErrorAction SilentlyContinue
            New-Item -ItemType Directory -Force (Join-Path $d 'optix') | Out-Null
            $o = Join-Path $d 'optix'; $c = Join-Path $d 'cuda'
          }
          $r = Invoke-Child startup $o $c @() "$kind$i"
          $res[$kind] += $r
          Write-Host ("{0} {1}: scene_create call {2:N1} commit {3:N1} session {4:N1} first pass {5:N1} ms; init done est {6:N1}; module {7} pipeline {8}" -f $kind, $i, $r.scene_create_call_ms, $r.commit_return_ms, $r.session_create_ms, $r.first_pass_done_ms, $r.init_done_est_ms, $r.module_ms, $r.pipeline_ms)
        }
        Show-Stats "$kind scene_create -> first pass" @($res[$kind] | ForEach-Object { $_.first_pass_done_ms })
        Show-Stats "$kind init done (estimate)" @($res[$kind] | ForEach-Object { $_.init_done_est_ms })
        Show-Stats "$kind pipeline module_ms" @($res[$kind] | ForEach-Object { $_.module_ms })
        Show-Stats "$kind pipeline_ms" @($res[$kind] | ForEach-Object { $_.pipeline_ms })
      }
      $n = (Get-ChildItem (Join-Path $work 'cold/0') -Recurse -File | Measure-Object Length -Sum)
      Write-Host "cold run 0 left $($n.Count) cache files, $([math]::Round($n.Sum / 1MB, 2)) MiB under $work/cold/0"
    }
    'motion' {
      $exe = Join-Path $root 'out/bin/astroray_tests_gpu.exe'
      $rows = @()
      for ($i = 0; $i -lt $Runs; $i++) {
        $c = Join-Path $work "motion/$i/cuda"
        Remove-Item -Recurse -Force (Split-Path $c) -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force $c | Out-Null
        $saved = $env:CUDA_CACHE_PATH; $env:CUDA_CACHE_PATH = $c
        try { $o = & $exe --tier=gate '--test-case=optix_pipeline/creation*' 2>&1 | ForEach-Object { "$_" } } finally { $env:CUDA_CACHE_PATH = $saved }
        if ($LASTEXITCODE -ne 0) { $o | Select-Object -Last 10 | ForEach-Object { Write-Host $_ }; throw "gpu test exited $LASTEXITCODE" }
        $m = @{}
        foreach ($ln in $o) {
          if ($ln -match 'cold cache\s+module ([0-9.]+) ms, pipeline ([0-9.]+) ms') { $m.cm = [double]$Matches[1]; $m.cp = [double]$Matches[2] }
          if ($ln -match 'motion variant module ([0-9.]+) ms, pipeline ([0-9.]+) ms') { $m.mm = [double]$Matches[1]; $m.mp = [double]$Matches[2] }
          if ($ln -match 'warm cache\s+module ([0-9.]+) ms, pipeline ([0-9.]+) ms') { $m.wm = [double]$Matches[1]; $m.wp = [double]$Matches[2] }
        }
        if ($m.Count -ne 6) { $o | Select-Object -Last 12 | ForEach-Object { Write-Host $_ }; throw 'the test printed no timing lines' }
        $rows += [pscustomobject]$m
        Write-Host ("run {0}: cold module {1} pipeline {2}; motion module {3} pipeline {4}; warm module {5} pipeline {6}" -f $i, $m.cm, $m.cp, $m.mm, $m.mp, $m.wm, $m.wp)
      }
      Show-Stats 'cold base module+pipeline' @($rows | ForEach-Object { $_.cm + $_.cp })
      Show-Stats 'cold motion module+pipeline' @($rows | ForEach-Object { $_.mm + $_.mp })
      Show-Stats 'warm base module+pipeline' @($rows | ForEach-Object { $_.wm + $_.wp })
    }
    'edit' {
      $r = Invoke-Child edit (Join-Path $work 'edit/optix') $null @('30') 'edit'
      foreach ($busy in $false, $true) {
        foreach ($k in 'seed', 'world', 'view') {
          $sel = @($r | Where-Object { $_.kind -eq $k -and $_.busy -eq $busy })
          Write-Host "--- $k, busy=$busy"
          Show-Stats 'commit/set call' @($sel | ForEach-Object { $_.call_ms })
          Show-Stats 'call start -> epoch change' @($sel | ForEach-Object { $_.to_epoch_ms })
          Show-Stats 'call start -> first pass done' @($sel | ForEach-Object { $_.to_first_pass_done_ms })
          Show-Stats 'last pass' @($sel | ForEach-Object { $_.last_pass_ms })
          Show-Stats 'call start -> new pass start (est)' @($sel | ForEach-Object { $_.start_est_ms })
        }
      }
    }
    'lights' {
      $rows = @()
      foreach ($spec in @(@('plane', '1'), @('plane', '10000'), @('b1m', ''), @('b1m_emissive', ''))) {
        for ($i = 0; $i -lt 5; $i++) {
          $ex = if ($spec[1]) { @($spec[0], $spec[1]) } else { @($spec[0]) }
          $r = Invoke-Child lights (Join-Path $work 'warm/optix') (Join-Path $work 'warm/cuda') $ex "lights$i"
          Write-Host ("{0} lamps {1} run {2}: build {3:N1} commit call {4:N1} commit->first pass {5:N1} ms (pass {6:N1}); ledger sum {7:N1} MiB" -f $r.kind, $spec[1], $i, $r.build_ms, $r.commit_call_ms, $r.commit_to_first_pass_ms, $r.first_pass_ms, (($r.ledger | Measure-Object -Sum).Sum / 1MB))
        }
      }
    }
    'nsys' {
      $nsys = 'C:\Program Files\NVIDIA Corporation\Nsight Systems 2024.6.2\target-windows-x64\nsys.exe'
      $cli = Join-Path $root 'out/bin/astroray_cli.exe'
      foreach ($size in '1920x1080', '3840x2160') {
        $base = Join-Path $work ("nsys_" + $size)
        Write-Host "=== b1m at $size, bench without profiler"
        & $cli bench (Join-Path $root 'out/scenes/b1m.arrec') --size $size --passes 32 --warmup 8 --device cuda 2>&1 | Where-Object { $_ -match '^\{' } | ForEach-Object { $o = $_ | ConvertFrom-Json; Write-Host ("{0} {1:g6} {2}" -f $o.metric, $o.value, $o.unit) }
        Write-Host "=== b1m at $size, under nsys"
        & $nsys profile --trace=cuda --force-overwrite=true -o $base $cli bench (Join-Path $root 'out/scenes/b1m.arrec') --size $size --passes 32 --warmup 8 --device cuda 2>&1 | Where-Object { $_ -match '^\{' } | ForEach-Object { $o = $_ | ConvertFrom-Json; Write-Host ("{0} {1:g6} {2}" -f $o.metric, $o.value, $o.unit) }
        & $nsys stats --report cuda_gpu_kern_sum --format csv --force-export=true "$base.nsys-rep" 2>&1 | Set-Content (Join-Path $work "kern_$size.csv")
        Get-Content (Join-Path $work "kern_$size.csv") | Select-Object -First 40 | ForEach-Object { Write-Host $_ }
      }
    }
    'sync' {
      foreach ($scene in 'b1m', 'b5m') {
        if (-not (Test-Path (Join-Path $work "$scene.blend.json"))) {
          & $py -I (Join-Path $root "scenes/$scene/build.py") --out (Join-Path $work "$scene.arrec") --blend (Join-Path $work "$scene.blend")
        }
      }
      $syncPath = Join-Path $work 'm094_sync.py'
      Set-Content -Path $syncPath -Value $syncScript -Encoding utf8
      $r = Invoke-ArBlender -Script $syncPath -Args @($work, "$Runs", $root) -TimeoutS 580
      if ($r.ExitCode -ne 0) { throw "sync script exited $($r.ExitCode)" }
    }
  }
} finally {
  Exit-ArMachineLock
}
