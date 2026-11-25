# SPDX-License-Identifier: Apache-2.0
# Fast test of tools/gate_table.py: two fake gate.json files, median and max columns, group wall.
# Usage: pwsh -NoProfile -File tests/tools/fast_gate_table.ps1   (exit 0 = pass)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. (Join-Path $root 'tools/lib/python.ps1')
$py = Get-ArPython
$tmp = Join-Path $root 'out/tmp/157/table-test'
New-Item -ItemType Directory -Force $tmp | Out-Null
function New-Run($path, $pass, $total, $steps) {
  [ordered]@{ tier = 'gate'; pass = $pass; seconds = $total; waited = 0; budget = 180; sha = 'x'
              steps = @($steps | ForEach-Object { [ordered]@{ name = $_[0]; seconds = $_[1]; pass = $true } }) } |
    ConvertTo-Json -Depth 4 | Set-Content -Path $path
}
# Three runs with a skewed last one, so the median (20) differs from the mean (40) and from the max (90).
New-Run "$tmp/a.json" $true 100.0 @(@('alpha', 10.0), @('analytic cpu', 4.0), @('analytic cuda', 7.0))
New-Run "$tmp/b.json" $true 120.0 @(@('alpha', 20.0), @('analytic cpu', 9.0), @('analytic cuda', 5.0))
New-Run "$tmp/c.json" $false 200.0 @(@('alpha', 90.0), @('analytic cpu', 30.0), @('analytic cuda', 6.0))
$check = Join-Path $root 'tools/check.ps1'
$out = @(& $py -I (Join-Path $root 'tools/gate_table.py') --check $check "$tmp/a.json" "$tmp/b.json" "$tmp/c.json")
$code = $LASTEXITCODE
$fail = 0
function Expect($pattern, $what) {
  if (-not ($out | Where-Object { $_ -match $pattern })) { Write-Host "FAIL gate_table: $what"; $script:fail++ }
}
Expect '^alpha\s+10\.0\s+20\.0\s+90\.0\s+20\.0\s+90\.0$' 'median 20.0 and max 90.0 of 10, 20, 90'
Expect '^TIER seconds\s+100\.0\s+120\.0\s+200\.0\s+120\.0\s+200\.0$' 'tier row median 120.0 max 200.0'
Expect '^group g1 wall\s+7\.0\s+9\.0\s+30\.0\s+9\.0\s+30\.0$' 'group wall is the slowest member per run'
Expect '^result\s+PASS\s+PASS\s+FAIL$' 'result row shows PASS PASS FAIL'
if ($code -ne 1) { Write-Host "FAIL gate_table: exit $code with a failed run, expected 1"; $fail++ }
$out2 = @(& $py -I (Join-Path $root 'tools/gate_table.py') --check $check "$tmp/a.json" "$tmp/b.json")
if ($LASTEXITCODE -ne 0) { Write-Host "FAIL gate_table: exit $LASTEXITCODE with only passing runs, expected 0"; $fail++ }
# --cases on canned step outputs: slowest first across kinds, a scene that git knows gets its commit.
# The doctest case name is unique per run, so `git log -S` finds no commit for it ("?").
$cd = Join-Path $tmp 'cases'
New-Item -ItemType Directory -Force $cd | Out-Null
$uniq = 'canned_' + [guid]::NewGuid().ToString('N')
Set-Content "$cd/cpu-gate-tests.out" @('build: sha=x dirty=0 kernels=embedded', "  0.250 s: $uniq/fast", "  3.500 s: $uniq/slow", "  1.000 s: $uniq/mid")
Set-Content "$cd/gpu-gate-tests.out" @("  0.500 s: $uniq/gpu")
Set-Content "$cd/analytic-cpu.out" @('PASS analytic a5_emitter cpu: worst z +1.00 (x at y) against per-pixel z 5.00 + eps_sys 0, 32x32 at 1024 spp, seed 1, 2.0 s; reproduce: r')
Set-Content "$cd/analytic-cuda.out" @('PASS analytic a5_emitter cuda: worst z +1.00 (x at y) against per-pixel z 5.00 + eps_sys 0, 32x32 at 1024 spp, seed 1, 0.7 s; reproduce: r')
Set-Content "$cd/integrator-cpu.out" @('PASS integrator d2_roulette cpu: ok (1.5 s)', 'PASS integrator cpu: 1 checks in 1.5 s')
Set-Content "$cd/integrator-cuda.out" @('PASS integrator d2_roulette cuda: ok (0.1 s)')
$co = @(& $py -I (Join-Path $root 'tools/gate_table.py') --cases --dir $cd --top 4)
if ($LASTEXITCODE -ne 0) { Write-Host "FAIL gate_table --cases: exit $LASTEXITCODE"; $fail++ }
$rows = @($co | Where-Object { $_ -match '^\s*[0-9]+\.[0-9]{2}\s' })
if ($rows.Count -ne 4) { Write-Host "FAIL gate_table --cases: $($rows.Count) rows, expected 4"; $fail++ }
elseif ($rows[0] -notmatch "3\.50\s+case\s+cpu\s+\?.*$uniq/slow") { Write-Host "FAIL gate_table --cases: the slowest case is not first: $($rows[0])"; $fail++ }
elseif ($rows[1] -notmatch '2\.00\s+scene\s+cpu\s+[0-9a-f]{7,}\s.*a5_emitter') { Write-Host "FAIL gate_table --cases: a5_emitter has no added commit: $($rows[1])"; $fail++ }
elseif ($rows[2] -notmatch '1\.50\s+integrator\s+cpu') { Write-Host "FAIL gate_table --cases: third row is not the integrator check: $($rows[2])"; $fail++ }
if (-not ($co | Where-Object { $_ -match '^step totals: .*case cpu 4\.8 s' })) { Write-Host 'FAIL gate_table --cases: step totals line'; $fail++ }
Set-Content "$cd/gpu-gate-tests.out" @('no timed line')
$null = @(& $py -I (Join-Path $root 'tools/gate_table.py') --cases --dir $cd 2>&1)
if ($LASTEXITCODE -eq 0) { Write-Host 'FAIL gate_table --cases: a step output without timed lines was accepted'; $fail++ }

# The shard partition of tools/analytic.py: every gate scene in exactly one shard, for several n.
$probe = @'
import importlib.util, sys
spec = importlib.util.spec_from_file_location("analytic", sys.argv[1] + "/tools/analytic.py")
a = importlib.util.module_from_spec(spec); spec.loader.exec_module(a)
bad = 0
for n in (1, 2, 3, 4):
    got = [s for i in range(1, n + 1) for s in a.shard_scenes(a.GATE_SCENES, i, n)]
    if sorted(got) != sorted(a.GATE_SCENES) or len(set(got)) != len(got):
        bad += 1; print(f"shards n={n}: {got}")
    members = sum(a.scene_members(a.shard_scenes(a.GATE_SCENES, i, n)) for i in range(1, n + 1))
    if members != a.scene_members(a.GATE_SCENES):
        bad += 1; print(f"shards n={n}: {members} members, expected {a.scene_members(a.GATE_SCENES)}")
if a.shard_scenes(a.GATE_SCENES, 2, 2)[0] != a.GATE_SCENES[1]:
    bad += 1; print("shard 2/2 does not start at the second scene")
print("shards ok" if not bad else "shards bad")
sys.exit(1 if bad else 0)
'@
$po = @(& $py -I -c $probe $root 2>&1)
if ($LASTEXITCODE -ne 0) { Write-Host 'FAIL analytic shards: partition'; $po | Select-Object -Last 5 | ForEach-Object { Write-Host "  $_" }; $fail++ }
if ($fail -gt 0) { $out | ForEach-Object { Write-Host $_ }; exit 1 }
Write-Host 'PASS gate_table'
exit 0
