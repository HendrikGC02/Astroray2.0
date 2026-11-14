# Shared helpers for the measurement scripts. Dot-source after env.ps1.
$script:Root = $PSScriptRoot
New-Item -ItemType Directory -Force "$PSScriptRoot\out\results" | Out-Null
function Time-Cmd([scriptblock]$sb) { $w = [Diagnostics.Stopwatch]::StartNew(); & $sb *> $null; $w.Stop(); return [math]::Round($w.Elapsed.TotalSeconds, 3) }
function Min-Med([double[]]$v) { $s = $v | Sort-Object; [pscustomobject]@{ min = $s[0]; median = $s[[int][math]::Floor(($s.Count - 1) / 2 + 0.5 * (($s.Count + 1) % 2))] } }
function Gpu-Snap { $g = (nvidia-smi --query-gpu=utilization.gpu,clocks.sm,power.draw,temperature.gpu --format=csv,noheader) -join ''; $c = (Get-CimInstance Win32_Processor | Measure-Object LoadPercentage -Average).Average; "$g | cpu load $c%" }
function Log($f, $line) { Add-Content -Path "$PSScriptRoot\out\results\$f" -Value $line; Write-Host $line }
