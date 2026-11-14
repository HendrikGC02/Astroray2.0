# Runs every Q1 GUI measurement group, one isolated windowed Blender per group, each self-terminating.
param([string[]]$Only = @())
. "$PSScriptRoot\lib.ps1"
$ws = '100 100 1000 650'
$groups = @(
  @{n='matrix';  a=@('group=matrix');  t=900},
  @{n='stall';   a=@('group=stall');   t=300},
  @{n='paced';   a=@('group=paced');   t=900},
  @{n='latency'; a=@('group=latency'); t=900},
  @{n='sweep';   a=@('group=sweep');   t=200},
  @{n='sweep_r2'; a=@('group=sweep','tag=_r2'); t=200},
  @{n='sweep_r3'; a=@('group=sweep','tag=_r3'); t=200}
)
foreach ($g in $groups) {
  if ($Only.Count -and ($Only -notcontains $g.n)) { continue }
  $rc = Invoke-IsolatedBlender -Script "$SB\gui_present.py" -ScriptArgs $g.a -TimeoutSec $g.t -LogName "gui_present_$($g.n).log" -WindowSize $ws
  Write-Host "group $($g.n) rc=$rc"
}
if ($Only.Count -and ($Only -notcontains 'fullregion')) { return }
# region-size validation: viewport maximised to a real 1920x1080-class region
$rc = Invoke-IsolatedBlender -Script "$SB\gui_present.py" -ScriptArgs @('group=matrix','res=1080','paths=BASE,A32,B16,B11','cached=','full=1','tag=_fullregion') -TimeoutSec 300 -LogName 'gui_present_fullregion.log' -WindowSize '0 0 1920 1080'
Write-Host "group fullregion rc=$rc"
