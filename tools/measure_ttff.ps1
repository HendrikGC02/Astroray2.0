# SPDX-License-Identifier: Apache-2.0
# tools/measure_ttff.ps1 [-Scene b1m,b5m,s3_textured] [-Engine cycles,astroray] [-Runs 3] [-Hold 12] [-Rebuild] [-Shot] [-Summarise]
# Time to first frame against Cycles (0007 d35: V7a the gate, V7b the phases) and the first M-8 numbers
# (0005 Risks, 0009 B-24) on S1 = b1m, S2 = b5m (300 materials) and S3 = s3_textured. For every scene and
# engine it starts -Runs fresh windowed Blender processes (tools/ttff_probe.py), each under the machine
# lock; one process switches the viewport to Rendered, back to Solid and to Rendered again (the first and the
# second switch) and times every redraw of the viewport region from a POST_PIXEL draw handler (ruling 83, tools/ttff_probe.py;
# V7a by a framebuffer patch read and by the engine's first-frame event, the table shows both). Cycles runs with its log on
# (--log cycles,context --log-level 4), from which V7b is read: the first `Rendered N samples` line minus (the
# `Total time spent synchronizing data: T` line minus T). Astroray's V7b is its first-frame mark minus its
# first ar_scene_edit_begin mark (blender/astroray render_final.Timeline).
# The .blend of each scene is written once into out/ttff/ (scenes/<name>/build.py, scenes/lib/blend_writer.py).
# Output: out/ttff/<scene>_<engine>_r<k>.json per run, out/ttff/summary.json, the per-run table and the
# medians, history lines in out/bench/history-ttff-<yyyyMMdd>.jsonl (the lead commits them to bench/history/),
# and `<python> tools/report.py` plots them. Per run it records how busy the machine was (other build or
# render processes) because other agents share it.
param(
  [string[]]$Scene = @('b1m', 'b5m', 's3_textured'),
  [string[]]$Engine = @('cycles', 'astroray'),
  [int]$Runs = 3,
  [double]$Hold = 12,
  [switch]$Rebuild,
  # V7a detection (ruling 83): read = a small framebuffer patch read in the POST_PIXEL handler; event = the first redraw
  # at or after the engine's own first displayable frame; both = both, and the table shows their agreement; none = no V7a.
  # auto = both for Astroray, event for Cycles (a read there hung Blender in 095; see -Read to try it again).
  [ValidateSet('auto', 'read', 'event', 'both', 'none')][string]$Source = 'auto',
  [ValidateSet('patch', 'post_view', 'timer')][string]$Read = 'patch',   # where the patch is read (tools/ttff_probe.py)
  [int]$Cell = 3,                       # the patch is 32 x 18 cells of Cell pixels
  # plain = Cycles runs with `--log cycles` (V7b needs it) and the read method alone: the V7a that is compared with Astroray.
  # context = `--log cycles,context`, which the event method needs for the anchor lines that put Cycles' log on the probe's
  # clock; Blender then writes about 6000 context lines a second and Cycles is measurably slower (127: 20 to 25 %), so these
  # runs show the agreement of the two methods, never the ratio.
  [ValidateSet('context', 'plain')][string]$CyclesLog = 'plain',
  [string]$Fmt = 'UBYTE',               # read_color data_format
  [int]$Chans = 4,
  [int]$TimeoutS = 900,
  [switch]$Shot,       # also keep Blender's own screenshot of its viewport area per switch (out/ttff/*_blender<n>.png)
  [switch]$Summarise   # only re-read out/ttff/*.json and print the tables
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
. (Join-Path $PSScriptRoot 'lib/lock.ps1')
. (Join-Path $PSScriptRoot 'lib/proc.ps1')
. (Join-Path $PSScriptRoot 'lib/python.ps1')
. (Join-Path $PSScriptRoot 'lib/blender.ps1')
$out = Join-Path $root 'out/ttff'
New-Item -ItemType Directory -Force $out | Out-Null
$bench = @{ b1m = 'b1m'; b5m = 'b5m'; s3_textured = 'b_tx'; s3_hdri = 's3_hdri' }   # scene builder -> bench id (bench/scenes.toml)
$Scene = @($Scene | ForEach-Object { $_ -split ',' })   # -File passes a comma list as one string
$unknown = @($Scene | Where-Object { -not $bench.ContainsKey($_) })
if ($unknown) { throw "measure_ttff: no bench id for scene $($unknown -join ', '); add it to `$bench" }

function Get-BenchMachineId([string]$Cli, [string]$Rec) {
  # perf.ps1's neutral id, read from the CLI's own bench line of a minimal run (one source; cli/bench.cpp hardware_id).
  $o = @(& $Cli bench $Rec --size 320x180 --passes 1 --warmup 0 --device cuda 2>&1 | ForEach-Object { "$_" })
  $line = $o | Where-Object { $_ -like '{*' } | Select-Object -First 1
  $id = if ($line) { ($line | ConvertFrom-Json).machine } else { $null }
  if (-not $id) { throw "the CLI printed no bench line to read the machine id from ($Cli bench $Rec): $($o | Select-Object -Last 3)" }
  return $id
}

function Build-Blend([string]$name) {
  $blend = Join-Path $out "$name.blend"
  if ((Test-Path $blend) -and -not $Rebuild) { return $blend }
  $py = Get-ArPython
  & $py -I (Join-Path $root "scenes/$name/build.py") --out (Join-Path $out "$name.arrec") --blend $blend
  if ($LASTEXITCODE -ne 0) { throw "scene builder $name failed" }
  $null = Invoke-ArBlender -Script (Join-Path $root 'scenes/lib/blend_writer.py') -TimeoutS 900 `
    -Args @('--no-extension', "$blend.json", $blend)
  if ($LASTEXITCODE -ne 0) { throw "blend_writer for $name failed" }
  return $blend
}

function Get-Busy {
  # What else is running that would disturb a timing: build tools, engine and Blender processes, and CPU load.
  $names = 'cl', 'link', 'nvcc', 'ninja', 'cmake', 'ctest', 'astroray_cli', 'astroray_tests_cpu', 'astroray_tests_gpu', 'blender'
  $others = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $names -contains $_.Name } | ForEach-Object { $_.Name })
  $cpu = (Get-CimInstance Win32_PerfFormattedData_PerfOS_Processor -Filter "Name='_Total'").PercentProcessorTime
  return @{ cpu_percent = [int]$cpu; processes = ($others -join ',') }
}

function Invoke-Run([string]$name, [string]$eng, [int]$k) {
  $blend = Join-Path $out "$name.blend"
  $result = Join-Path $out "${name}_${eng}_r$k.json"
  Remove-Item "$result*" -ErrorAction SilentlyContinue
  $log = Join-Path $out "${name}_${eng}_r$k.log"
  $logArgs = if ($eng -eq 'cycles') { @('--log', $(if ($CyclesLog -eq 'plain') { 'cycles' } else { 'cycles,context' }), '--log-level', '4') } else { @() }
  $src = if ($Source -eq 'auto') { if ($CyclesLog -eq 'plain' -and $eng -eq 'cycles') { 'read' } else { 'both' } } else { $Source }
  $args_ = @('--scene', $name, '--engine', $eng, '--blend', $blend, '--out', $result, '--hold', "$Hold", '--source', $src, '--read', $Read, '--cell', "$Cell", '--fmt', $Fmt, '--chans', "$Chans") +
    $(if ($eng -eq 'astroray' -and $name -eq 'b5m') { @('--m8') } else { @() }) + $(if ($Shot) { @('--shot') } else { @() })
  $before = Get-Busy
  $r = Invoke-ArBlender -Gui -Script (Join-Path $root 'tools/ttff_probe.py') -BlenderArgs $logArgs -Args $args_ -TimeoutS $TimeoutS
  $timedOut = $r.ExitCode -eq -1
  $after = Get-Busy
  foreach ($f in @($r.Log, "$($r.Log).err")) { if (Test-Path $f) { Copy-Item $f ($log + $(if ($f.EndsWith('.err')) { '.err' } else { '' })) -Force } }
  if (-not (Test-Path $result)) { Write-Host "FAIL ${name} ${eng} r${k}: no result (timeout=$timedOut); see $log"; return $null }
  $o = Get-Content $result -Raw | ConvertFrom-Json
  $o | Add-Member -NotePropertyName busy_before -NotePropertyValue $before -Force
  $o | Add-Member -NotePropertyName busy_after -NotePropertyValue $after -Force
  $o | Add-Member -NotePropertyName log -NotePropertyValue $log -Force
  return $o
}

# Cycles V7b and the first-sample time from the log, per switch (segments start at TTFF_SWITCH n), and the offset that
# puts log time on the probe's epoch clock. With `--log context` Blender logs each `bpy.context.area` access with its
# own clock; the probe prints the epoch of each such access with the line number (TTFF_ANCHOR <line> <epoch>), so the
# k-th marker pairs with the k-th log line `ttff_probe.py:<line>: area=` and the median of (epoch - log time) converts
# a log time to epoch. The spread of the pairs is returned so a bad conversion shows.
function Read-CyclesLog([string]$log) {
  $res = @{}; $markers = @(); $ctx = @{}
  $cur = 0; $sync = $null; $T = $null; $first = $null
  # Blender's log lines are in one of the two captured streams; the probe's markers are in both.
  $f = @("$log.err", $log) | Where-Object { (Test-Path $_) -and (Select-String -Path $_ -Pattern 'synchronizing data' -Quiet) } | Select-Object -First 1
  if ($f) {
    foreach ($ln in [IO.File]::ReadLines($f)) {
      if ($ln -match 'TTFF_ANCHOR (\d+) ([0-9.]+)') { $markers += , @([int]$Matches[1], [double]$Matches[2]); continue }
      if ($ln -match '^\s*(\d+):(\d+\.\d+)\s+context\s+\|.*ttff_probe\.py:(\d+): area=') {
        $kk = [int]$Matches[3]; if (-not $ctx.ContainsKey($kk)) { $ctx[$kk] = @() }
        $ctx[$kk] += [int]$Matches[1] * 60 + [double]$Matches[2]; continue
      }
      if ($ln -match 'TTFF_SWITCH (\d)') { $cur = [int]$Matches[1]; $sync = $null; $first = $null; continue }
      if ($cur -eq 0) { continue }
      $ts = $null
      if ($ln -match '^\s*(\d+):(\d+\.\d+)') { $ts = [int]$Matches[1] * 60 + [double]$Matches[2] }
      if ($null -eq $ts) { continue }
      if ($null -eq $sync -and $ln -match 'Total time spent synchronizing data:\s*([0-9.]+)') { $sync = $ts; $T = [double]$Matches[1] }
      elseif ($null -ne $sync -and $null -eq $first -and $ln -match 'Rendered \d+ samples') {
        $first = $ts
        $res[$cur] = @{ first_log_s = $first; v7b_ms = ($first - ($sync - $T)) * 1000.0; sync_ms = $T * 1000.0; sync_to_first_ms = ($first - $sync) * 1000.0 }
      }
    }
  }
  $offs = @(); $idx = @{}
  foreach ($m in $markers) {
    $kk = $m[0]; $i = if ($idx.ContainsKey($kk)) { $idx[$kk] } else { 0 }
    if ($ctx.ContainsKey($kk) -and $i -lt $ctx[$kk].Count) { $offs += ($m[1] - $ctx[$kk][$i]) }
    $idx[$kk] = $i + 1
  }
  $off = $null; $spread = $null
  if ($offs.Count -gt 0) { $off = Median $offs; $spread = ($offs | Measure-Object -Maximum).Maximum - ($offs | Measure-Object -Minimum).Minimum }
  return @{ switches = $res; offset = $off; offset_spread_s = $spread; anchors = $offs.Count }
}

function Median([double[]]$v) {
  if ($v.Count -eq 0) { return $null }
  $s = $v | Sort-Object; $n = $s.Count
  if ($n % 2) { return [double]$s[($n - 1) / 2] } else { return ([double]$s[$n / 2 - 1] + [double]$s[$n / 2]) / 2 }
}

$rows = @()   # one per run and switch
$m8 = @()
if (-not $Summarise) {
  foreach ($name in $Scene) { $null = Build-Blend $name }
  for ($k = 1; $k -le $Runs; $k++) {
    foreach ($name in $Scene) {
      foreach ($eng in $Engine) {
        $o = Invoke-Run $name $eng $k
        if ($null -eq $o) { continue }
        $cl = if ($eng -eq 'cycles') { Read-CyclesLog $o.log } else { $null }
        $c = if ($cl) { $cl.switches } else { @{} }
        foreach ($sw in @($o.switches)) {
          $rd = @($sw.redraws)
          $row = [ordered]@{ scene = $name; engine = $eng; run = $k; switch = [int]$sw.n; v7a_ms = $null; v7a_method = $null
            v7a_read_ms = $null; v7a_event_ms = $null; agree_redraws = $null; v7b_ms = $null
            redraw_ms = if ($rd.Count -gt 2) { 1000.0 * (Median @(for ($i = 1; $i -lt [math]::Min($rd.Count, 40); $i++) { $rd[$i] - $rd[$i - 1] })) } else { $null }
            error = $sw.error; anchors = $null; anchor_spread_ms = $null
            cpu_before = $o.busy_before.cpu_percent; others_before = $o.busy_before.processes; others_after = $o.busy_after.processes }
          if ($null -ne $sw.read_s) { $row.v7a_read_ms = [double]$sw.read_s * 1000.0 }
          if ($null -ne $sw.event_s) { $row.v7a_event_ms = [double]$sw.event_s * 1000.0 }
          if ($eng -eq 'cycles' -and $c.ContainsKey([int]$sw.n)) {
            $row.v7b_ms = $c[[int]$sw.n].v7b_ms
            if ($null -ne $cl.offset) {
              # the first redraw at or after Cycles' first `Rendered N samples` line, the log time put on the epoch clock
              $row.anchors = $cl.anchors; $row.anchor_spread_ms = $cl.offset_spread_s * 1000.0
              $rel = $c[[int]$sw.n].first_log_s + $cl.offset - [double]$sw.t_on
              $hit = @($rd | Where-Object { $_ -ge $rel })
              if ($hit.Count) { $row.v7a_event_ms = [double]$hit[0] * 1000.0 }
            }
          }
          # The headline: the read where it worked, otherwise the event; the other is kept for the agreement.
          if ($null -ne $row.v7a_read_ms) { $row.v7a_ms = $row.v7a_read_ms; $row.v7a_method = 'read' }
          elseif ($null -ne $row.v7a_event_ms) { $row.v7a_ms = $row.v7a_event_ms; $row.v7a_method = 'event' }
          if ($null -ne $row.v7a_read_ms -and $null -ne $row.v7a_event_ms) {
            $ir = [array]::IndexOf($rd, ($rd | Where-Object { [math]::Abs($_ * 1000.0 - $row.v7a_read_ms) -lt 1e-6 } | Select-Object -First 1))
            $ie = [array]::IndexOf($rd, ($rd | Where-Object { [math]::Abs($_ * 1000.0 - $row.v7a_event_ms) -lt 1e-6 } | Select-Object -First 1))
            if ($ir -ge 0 -and $ie -ge 0) { $row.agree_redraws = $ir - $ie }   # redraws between the two answers (0 = the same redraw)
          }
          if ($eng -eq 'astroray') {
            $job = @($o.jobs)[[int]$sw.n - 1]
            if ($job) {
              $mk = $job.marks
              if ($mk.first_frame -and $mk.edit_begin) { $row.v7b_ms = ($mk.first_frame - $mk.edit_begin) * 1000.0 }
              $row.phases = [ordered]@{
                switch_to_view_update_ms = if ($mk.view_update) { ($mk.view_update - $sw.t_on) * 1000.0 } else { $null }
                view_update_to_edit_begin_ms = if ($mk.edit_begin) { ($mk.edit_begin - $mk.view_update) * 1000.0 } else { $null }
                settings_world_ms = $job.phase_s.settings_world * 1000.0; scan_ms = $job.phase_s.scan * 1000.0
                images_ms = $job.phase_s.images * 1000.0; materials_ms = $job.phase_s.materials * 1000.0
                lights_ms = $job.phase_s.lights * 1000.0; meshes_ms = $job.phase_s.meshes * 1000.0
                instances_ms = $job.phase_s.instances * 1000.0
                main_thread_sync_ms = if ($mk.edit_commit) { ($mk.edit_commit - $mk.edit_begin) * 1000.0 } else { $null }
                commit_to_session_start_ms = if ($mk.session_start) { ($mk.session_start - $mk.edit_commit) * 1000.0 } else { $null }
                session_start_to_rendering_ms = if ($mk.rendering) { ($mk.rendering - $mk.session_start) * 1000.0 } else { $null }
                rendering_to_first_frame_ms = if ($mk.first_frame -and $mk.rendering) { ($mk.first_frame - $mk.rendering) * 1000.0 } else { $null }
                first_frame_after_switch_ms = if ($mk.first_frame) { ($mk.first_frame - $sw.t_on) * 1000.0 } else { $null }
              }
              $row.materials = @($job.materials)
            }
          }
          $rows += [pscustomobject]$row
        }
        if ($o.m8) { $m8 += [pscustomobject]@{ run = $k; m8 = $o.m8 } }
        Write-Host ("run {0} {1} {2}: V7a {3}" -f $k, $name, $eng, (($rows | Where-Object { $_.scene -eq $name -and $_.engine -eq $eng -and $_.run -eq $k } | ForEach-Object { '{0:N0} ms' -f $_.v7a_ms }) -join ' / '))
      }
    }
  }
  $rows | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $out 'rows.json')
  $rows = @(Get-Content (Join-Path $out 'rows.json') -Raw | ConvertFrom-Json)
  $m8 | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $out 'm8.json')
} else {
  $rows = @(Get-Content (Join-Path $out 'rows.json') -Raw | ConvertFrom-Json)
  $m8 = @(Get-Content (Join-Path $out 'm8.json') -Raw | ConvertFrom-Json)
}

'--- per run (V7a in the process, ms: headline, by which method, and each method; V7b = log phases; agree = redraws between read and event) ---'
$rows | Sort-Object scene, engine, switch, run | Format-Table scene, engine, switch, run,
  @{ n = 'V7a'; e = { if ($null -ne $_.v7a_ms) { '{0:N0}' -f $_.v7a_ms } else { '-' } } }, v7a_method,
  @{ n = 'read'; e = { if ($null -ne $_.v7a_read_ms) { '{0:N0}' -f $_.v7a_read_ms } else { '-' } } },
  @{ n = 'event'; e = { if ($null -ne $_.v7a_event_ms) { '{0:N0}' -f $_.v7a_event_ms } else { '-' } } },
  agree_redraws,
  @{ n = 'V7b'; e = { if ($null -ne $_.v7b_ms) { '{0:N0}' -f $_.v7b_ms } else { '-' } } },
  @{ n = 'redraw'; e = { if ($null -ne $_.redraw_ms) { '{0:N0}' -f $_.redraw_ms } else { '-' } } },
  anchors, cpu_before, others_before, others_after -AutoSize | Out-String -Width 220

'--- medians and ratios to Cycles (gate: <= 1.5x, goal 1.0x) ---'
$hist = @()
$date = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
$sha = (git rev-parse --short HEAD).Trim()
$machine = Get-BenchMachineId (Join-Path $root 'out/bin/astroray_cli.exe') (Join-Path $out "$($Scene[0]).arrec")
$gpu = (& nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>$null) -split ','
$driver = if ($gpu.Count -ge 2) { $gpu[1].Trim() } else { 'unknown' }
foreach ($name in $Scene) {
  foreach ($sw in 1, 2) {
    $cyc = @($rows | Where-Object { $_.scene -eq $name -and $_.engine -eq 'cycles' -and $_.switch -eq $sw -and $null -ne $_.v7a_ms } | ForEach-Object { $_.v7a_ms })
    $ar = @($rows | Where-Object { $_.scene -eq $name -and $_.engine -eq 'astroray' -and $_.switch -eq $sw -and $null -ne $_.v7a_ms } | ForEach-Object { $_.v7a_ms })
    $mc = Median $cyc; $ma = Median $ar
    $ratio = if ($mc -and $ma) { $ma / $mc } else { $null }
    '{0,-12} switch {1}: cycles {2,7:N0} ms (n={3}, {4:N0}..{5:N0})  astroray {6,7:N0} ms (n={7}, {8:N0}..{9:N0})  ratio {10}' -f `
      $name, $sw, $mc, $cyc.Count, ($cyc | Measure-Object -Minimum).Minimum, ($cyc | Measure-Object -Maximum).Maximum, $ma, $ar.Count,
      ($ar | Measure-Object -Minimum).Minimum, ($ar | Measure-Object -Maximum).Maximum, $(if ($ratio) { '{0:N2}x' -f $ratio } else { 'n/a' })
    foreach ($e in 'cycles', 'astroray') {
      foreach ($metric in 'v7a', 'v7a_read', 'v7a_event', 'v7b') {
        $v = @($rows | Where-Object { $_.scene -eq $name -and $_.engine -eq $e -and $_.switch -eq $sw -and $null -ne $_."${metric}_ms" } | ForEach-Object { $_."${metric}_ms" })
        if ($v.Count -eq 0) { continue }
        $mn = ($v | Measure-Object -Minimum).Minimum; $mx = ($v | Measure-Object -Maximum).Maximum
        $hist += ([ordered]@{ sha = $sha; machine = $machine; driver = $driver; cuda = 'cuda'; scene = $bench[$name]
          metric = "first_frame.$metric.$e.switch$sw"; value = [math]::Round((Median $v), 1); unit = 'ms'; n = $v.Count
          spread = [math]::Round($mx - $mn, 1); date = $date } | ConvertTo-Json -Compress)
      }
    }
  }
}
if ($hist.Count -gt 0) {
  $hp = Join-Path $root ('out/bench/history-ttff-' + (Get-Date -Format 'yyyyMMdd') + '.jsonl')
  New-Item -ItemType Directory -Force (Split-Path $hp) | Out-Null
  Set-Content -Path $hp -Value $hist
  "history lines: $hp ($($hist.Count))"
}

'--- Astroray phases, median over runs (ms; switch 1 / switch 2) ---'
foreach ($name in $Scene) {
  $a = @($rows | Where-Object { $_.scene -eq $name -and $_.engine -eq 'astroray' -and $_.phases })
  if ($a.Count -eq 0) { continue }
  foreach ($key in $a[0].phases.PSObject.Properties.Name) {
    $m = foreach ($sw in 1, 2) { $v = @($a | Where-Object { $_.switch -eq $sw -and $null -ne $_.phases.$key } | ForEach-Object { $_.phases.$key }); if ($v.Count) { '{0,8:N1}' -f (Median $v) } else { '       -' } }
    '{0,-12} {1,-32} {2}' -f $name, $key, ($m -join ' /')
  }
}

'--- M-8 (S2 b5m): translation per material, first switch ---'
$s2 = @($rows | Where-Object { $_.scene -eq 'b5m' -and $_.engine -eq 'astroray' -and $_.switch -eq 1 -and $_.materials })
foreach ($r in $s2) {
  $mats = @($r.materials)
  $inl = $mats | ForEach-Object { $_[0] * 1000 }; $walk = $mats | ForEach-Object { $_[1] * 1000 }; $send = $mats | ForEach-Object { $_[2] * 1000 }
  $tot = for ($i = 0; $i -lt $mats.Count; $i++) { $inl[$i] + $walk[$i] + $send[$i] }
  $nodes = $mats | ForEach-Object { $_[3] }
  'run {0}: {1} materials; per material median ms: inline {2:N3} walk {3:N3} serialise+send {4:N3} total {5:N3}; sum {6:N1} ms; ops/material median {7}; per 100 ops {8:N3} ms' -f `
    $r.run, $mats.Count, (Median $inl), (Median $walk), (Median $send), (Median $tot), ($tot | Measure-Object -Sum).Sum, (Median $nodes),
    ((Median $tot) / (Median $nodes) * 100)
}
foreach ($m in $m8) {
  'run {0}: resend of {1} materials: send {2:N1} ms, commit {3:N1} ms, commit return to active generation {4:N1} ms (reached={5})' -f `
    $m.run, $m.m8.materials, ($m.m8.send_s * 1000), ($m.m8.commit_s * 1000), ($m.m8.active_after_commit_s * 1000), $m.m8.reached
}
