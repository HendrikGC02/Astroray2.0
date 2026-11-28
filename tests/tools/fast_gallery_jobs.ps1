# SPDX-License-Identifier: Apache-2.0
# Fast test of tools/gallery.py's job list (--plan: no build, no render) on a canned scenes.toml: the three keys
# gallery_via = "cli", denoise and panel produce their runs and file names, a plain scene produces neither, and a
# cli scene without a recording fails by name.
# Usage: pwsh -NoProfile -File tests/tools/fast_gallery_jobs.ps1   (exit 0 = pass)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. (Join-Path $root 'tools/lib/python.ps1')
$py = Get-ArPython
$tmp = Join-Path ([IO.Path]::GetTempPath()) "astroray_gallery_jobs_$PID"
New-Item -ItemType Directory -Force $tmp | Out-Null
try {
  $good = Join-Path $tmp 'good.toml'
  @'
[[scene]]
name = "plain"
recording = "plain.arrec"
class = "G"
sets = []
showcase = ["MT"]
gallery_size = [8, 8]
gallery_spp = 1

[[scene]]
name = "pair"
recording = "pair.arrec"
class = "G"
sets = []
showcase = ["MT"]
gallery_size = [8, 8]
gallery_spp = 1
denoise = true
denoise_patch = [0.1, 0.1, 0.5, 0.5]
panel = true

[[scene]]
name = "slab"
recording = "slab.arrec"
class = "G"
sets = []
showcase = ["MT"]
gallery_size = [8, 8]
gallery_spp = 1
gallery_via = "cli"
'@ | Set-Content -Encoding utf8 $good
  $bad = Join-Path $tmp 'bad.toml'
  @'
[[scene]]
name = "norec"
class = "G"
sets = []
showcase = ["MT"]
gallery_size = [8, 8]
gallery_spp = 1
gallery_via = "cli"
'@ | Set-Content -Encoding utf8 $bad
  $lines = @(& $py -I (Join-Path $root 'tools/gallery.py') --milestone MT --scenes-toml $good --plan 2>&1 | ForEach-Object { "$_" })
  $fail = @()
  $want = @('job plain_f12 blender f12', 'job pair_f12 blender f12', 'job pair_denoise blender denoise',
            'job pair_panel windowed panel', 'job slab_cli python cli',
            'file pair_denoise_off.png', 'file pair_denoise_off.json', 'file pair_denoise_on.png', 'file pair_denoise_on.json',
            'file pair_panel.png', 'file pair_panel.json', 'file slab_cpu.png', 'file slab_cuda.json', 'file slab_diff.png')
  foreach ($w in $want) { if ($lines -notcontains $w) { $fail += "missing line: $w" } }
  foreach ($n in $lines | Where-Object { $_ -match '^(job|file) plain_(denoise|panel)|^file plain_(denoise|panel)' }) { $fail += "plain scene got: $n" }
  $out = @(& $py -I (Join-Path $root 'tools/gallery.py') --milestone MT --scenes-toml $bad --plan 2>&1 | ForEach-Object { "$_" })
  if ($LASTEXITCODE -eq 0 -or -not ($out -match 'FAIL gallery:.*norec.*no recording')) { $fail += "a cli scene without a recording must fail by name; got: $($out -join ' | ')" }
  if ($fail) { $fail | ForEach-Object { Write-Host "FAIL gallery_jobs: $_" }; exit 1 }
  Write-Host 'PASS gallery_jobs: cli, denoise and panel jobs and file names'
} finally { Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue }
