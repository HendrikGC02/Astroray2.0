# Reproduces every number in RESULTS.md. Serial on purpose: one GPU-heavy job at a time.
# Every Blender instance is isolated (own config/scripts/extensions/datafiles under .\blender_home, --factory-startup),
# is its own process, and is killed by its own PID on timeout. GUI scenarios quit Blender themselves.
# Usage:  .\run_all.ps1            (everything, ~25-35 min)
#         .\run_all.ps1 -Only sync  (build|scenes|sync|cycles|gui|edits|cycview|tables)
param([string[]]$Only = @('build', 'scenes', 'sync', 'cycles', 'gui', 'edits', 'cycview', 'tables'))
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\lib.ps1"

function Step($n) { return $Only -contains $n }
function Run-BG($script, $blend, $a, $log, $t = 900, $extra = @()) {
  $rc = Invoke-IsolatedBlender -Script "$SB\$script" -ScriptArgs $a -Background -BlendFile $blend -TimeoutSec $t -LogName $log -ExtraBlenderArgs $extra
  Write-Host "$script $a rc=$rc"
  return $rc
}

if (Step 'build') { & "$SB\build.ps1" }

if (Step 'scenes') {
  Run-BG 'make_scenes.py' '' @('S1') 'make_S1.log' 900 | Out-Null
  Run-BG 'make_scenes.py' '' @('S2') 'make_S2.log' 2400 | Out-Null
  Select-String -Path "$SB\out\make_S1.log", "$SB\out\make_S2.log" -Pattern '^SCENE|template tris'
}

if (Step 'sync') {
  Run-BG 'probe_fastpaths.py' "$SB\scenes\S1.blend" @('S1') 'fastpaths_S1.log' 300 | Out-Null
  Run-BG 'probe_fastpaths.py' "$SB\scenes\S2.blend" @('S2') 'fastpaths_S2.log' 300 | Out-Null
  foreach ($s in 'S1', 'S2') {
    Run-BG 'sync_bench.py' '' @($s) "sync_$s.log" 1800 | Out-Null                         # full run: breakdown + micro + GAS
    foreach ($st in 'naive', 'fast') { foreach ($r in 1..3) { Run-BG 'sync_bench.py' '' @($s, 'quick=1', "strat=$st", "run=$r") "sync_${s}_quick_${st}_$r.log" 900 | Out-Null } }   # cold repeats per strategy
  }
}

if (Step 'cycles') {
  foreach ($s in 'S1', 'S2') {
    # --debug-cycles --verbose 3 writes Cycles' own phase log to out\cycles_$s.log
    Run-BG 'cycles_bench.py' "$SB\scenes\$s.blend" @($s) "cycles_$s.log" 1800 @('--debug-cycles', '--verbose', '3') | Out-Null
  }
}

if (Step 'gui') { & "$SB\run_gui.ps1" }

if (Step 'screenshot') { Write-Host 'screenshot: see RESULTS.md (manual PowerShell snippet in gui_screenshot.py header)' }
if (Step 'edits') {
  foreach ($s in 'S1', 'S2') {
    $rc = Invoke-IsolatedBlender -Script "$SB\gui_edits.py" -ScriptArgs @($s) -BlendFile "$SB\scenes\$s.blend" -TimeoutSec 900 -LogName "gui_edits_$s.log" -WindowSize '100 100 1000 650'
    Write-Host "gui_edits $s rc=$rc"
  }
}

if (Step 'cycview') {
  foreach ($s in 'S1', 'S2') {
    $rc = Invoke-IsolatedBlender -Script "$SB\gui_cycles_viewport.py" -ScriptArgs @($s, 'full=1') -BlendFile "$SB\scenes\$s.blend" -TimeoutSec 150 -LogName "gui_cycles_viewport_$s.log" -WindowSize '0 0 1920 1080' -ExtraBlenderArgs @('--debug-cycles','--verbose','3')
    Write-Host "gui_cycles_viewport $s rc=$rc"
  }
}

if (Step 'cycview') { python "$SB\parse_cycview.py" | Tee-Object -FilePath "$SB\out\cycview_summary.txt" }
if (Step 'tables') { python "$SB\tables.py" | Out-Null; Write-Host "tables -> $SB\out\tables.md" }
