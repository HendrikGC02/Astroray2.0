# MSVC x64 env + CUDA 12.8 + OptiX path for spike B. Idempotent.
if (-not $env:SB_ENV_DONE) {
  $bt = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
  $cuda = 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8'
  cmd /c "`"$bt\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
  }
  $env:CUDA_PATH = $cuda
  $env:PATH = "$cuda\bin;$env:PATH"
  $env:SB_ENV_DONE = '1'
}
$global:OPTIX = 'C:\ProgramData\NVIDIA Corporation\OptiX SDK 9.1.0'
$global:SB = $PSScriptRoot
$global:BLENDER = 'C:\Program Files\Blender Foundation\Blender 5.3\blender.exe'
